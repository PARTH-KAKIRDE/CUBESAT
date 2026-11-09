#!/usr/bin/env python3
"""CubeSat EM payload: relative solar flux (TSL2591) + Earth imaging (CSI camera).

Runs on the OBC (RPi CM4). Logs one CSV row per sample and takes an image
every IMG_EVERY samples. Keeps running if the sensor or camera fails.
"""
import csv
import os
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timedelta, timezone

import board
import busio
import adafruit_tsl2591

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.environ.get("PAYLOAD_DIR", os.path.join(BASE_DIR, "payload_data"))
PERIOD_S = 10            # sampling period
IMG_EVERY = 6            # capture an image every N samples
MAX_IMG_MB = 2000        # delete oldest images once they use more than this
MIN_FREE_MB = 500        # ...or once free disk space drops below this
IMG_RESERVE_MB = 10      # headroom so the next image fits without crossing the limits
SENSOR_RESET_AFTER = 5   # re-open the I2C bus after this many consecutive sensor errors
CAM_TIMEOUT_S = 20       # kill the camera process if it takes longer than this
IST = timezone(timedelta(hours=5, minutes=30))  # fixed offset, ignores the board's timezone

GAIN = adafruit_tsl2591.GAIN_LOW
INTEGRATION = adafruit_tsl2591.INTEGRATIONTIME_100MS
# Highest count a channel can reach: 36863 at 100 ms integration, 65535 otherwise
SAT_COUNT = 36863 if INTEGRATION == adafruit_tsl2591.INTEGRATIONTIME_100MS else 65535

# rpicam-still on current Raspberry Pi OS, libcamera-still on older releases
CAM_CMD = shutil.which("rpicam-still") or shutil.which("libcamera-still")

running = True
_i2c = None


def log(msg):
    print(f"[payload] {msg}", file=sys.stderr, flush=True)


def stop(signum, frame):
    global running
    running = False


# ---------------- light sensor ----------------

def reset_i2c():
    """Tear down the I2C bus so the next init_sensor() rebuilds it from scratch."""
    global _i2c
    if _i2c is not None:
        try:
            _i2c.deinit()
        except Exception:
            pass
        _i2c = None


def init_sensor():
    global _i2c
    if _i2c is None:
        _i2c = busio.I2C(board.SCL, board.SDA)
    s = adafruit_tsl2591.TSL2591(_i2c)
    s.gain = GAIN
    s.integration_time = INTEGRATION
    return s


def read_light(s):
    # One read of both channels, so full and IR come from the same moment.
    # Raw counts are the science data; lux is only a relative indicator.
    full, ir = s.raw_luminosity
    saturated = full >= SAT_COUNT or ir >= SAT_COUNT
    lux = ""
    if not saturated:
        try:
            lux = f"{s.lux:.2f}"
        except RuntimeError:          # library raises on overflow
            saturated = True
    return dict(full=full, ir=ir, vis=max(full - ir, 0), lux=lux,
                status="SATURATED" if saturated else "OK")


# ---------------- camera ----------------

def capture_image(path):
    """Capture in a separate process so a camera hang can never stall the loop."""
    if CAM_CMD is None:
        raise RuntimeError("rpicam-still / libcamera-still not found")
    tmp = os.path.join(LOG_DIR, "capture_tmp.jpg")
    try:
        subprocess.run([CAM_CMD, "-n", "-t", "1000", "-o", tmp],
                       check=True, timeout=CAM_TIMEOUT_S,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if os.path.getsize(tmp) == 0:
            raise RuntimeError("empty image")
        fsync_path(tmp)               # image data on disk before the rename
        os.replace(tmp, path)         # atomic: never a half-written image
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


# ---------------- storage ----------------

def fsync_path(path):
    """fsync a file or directory by path."""
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def img_seq(name):
    """Sequence number of an image filename, or None if it isn't one of ours."""
    if name.startswith("img_") and name.endswith(".jpg") and name[4:-4].isdigit():
        return int(name[4:-4])
    return None


def image_names():
    # Sort by number, not text, so img_1000000 comes after img_999999
    return sorted((n for n in os.listdir(LOG_DIR) if img_seq(n) is not None),
                  key=img_seq)


def next_seq():
    names = image_names()
    return img_seq(names[-1]) + 1 if names else 0


def prune_images():
    """Delete oldest images (lowest sequence number) so the next capture stays
    under MAX_IMG_MB and above MIN_FREE_MB, with IMG_RESERVE_MB of headroom."""
    names = image_names()
    mb = 1024 * 1024
    total = sum(os.path.getsize(os.path.join(LOG_DIR, n)) for n in names)
    while names and (total > (MAX_IMG_MB - IMG_RESERVE_MB) * mb
                     or shutil.disk_usage(LOG_DIR).free < (MIN_FREE_MB + IMG_RESERVE_MB) * mb):
        oldest = names.pop(0)
        p = os.path.join(LOG_DIR, oldest)
        total -= os.path.getsize(p)
        os.remove(p)
        log(f"pruned {oldest}")


# ---------------- main loop ----------------

def main():
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    os.makedirs(LOG_DIR, exist_ok=True)
    if CAM_CMD is None:
        log("WARNING: no camera command found, images will be logged as CAM_FAIL")
    leftover = os.path.join(LOG_DIR, "capture_tmp.jpg")
    if os.path.exists(leftover):      # from a power cut mid-capture
        os.remove(leftover)

    sensor = None
    sensor_errors = 0                 # consecutive failures, triggers an I2C reset
    seq = next_seq()
    log_path = os.path.join(LOG_DIR, "light_log.csv")
    new = not os.path.exists(log_path) or os.path.getsize(log_path) == 0
    needs_nl = False                  # power cut may have left a half-written last line
    if not new:
        with open(log_path, "rb") as rf:
            rf.seek(-1, os.SEEK_END)
            needs_nl = rf.read(1) != b"\n"

    with open(log_path, "a", newline="") as f:
        w = csv.writer(f)
        if needs_nl:
            f.write("\r\n")           # csv module's default line ending
        if new:
            w.writerow(["uptime_s", "wall_time", "full", "ir", "vis",
                        "lux", "status", "image"])
        n = 0
        while running:
            t0 = time.monotonic()

            try:
                if sensor is None:
                    sensor = init_sensor()
                d = read_light(sensor)
                sensor_errors = 0
            except Exception as e:    # sensor unplugged etc: log it, retry next sample
                sensor = None
                sensor_errors += 1
                if sensor_errors >= SENSOR_RESET_AFTER:
                    log(f"{sensor_errors} consecutive sensor errors, resetting I2C bus")
                    reset_i2c()
                    sensor_errors = 0
                d = dict(full="", ir="", vis="", lux="",
                         status=f"ERR:{type(e).__name__}")
                log(f"sensor error: {e!r}")

            # Stamp the row at the light read, before the camera adds a delay
            uptime = f"{time.monotonic():.1f}"                        # seconds since boot
            stamp = datetime.now(IST).strftime("%Y-%m-%dT%H:%M:%S+05:30")  # IST

            img = ""
            if n % IMG_EVERY == 0:
                try:
                    prune_images()
                except Exception as e:      # a pruning problem is not a camera problem
                    log(f"prune error: {e!r}")
                name = f"img_{seq:06d}.jpg"
                try:
                    capture_image(os.path.join(LOG_DIR, name))
                    fsync_path(LOG_DIR)
                    img = name
                    seq += 1
                except Exception as e:
                    img = "CAM_FAIL"
                    log(f"camera error: {e!r}")

            # A failed row write (disk full, I/O error) must not kill the loop
            try:
                w.writerow([uptime, stamp,
                            d["full"], d["ir"], d["vis"], d["lux"], d["status"], img])
                f.flush()
                os.fsync(f.fileno())  # survive sudden power loss
            except OSError as e:
                log(f"log write error: {e!r}")
                try:
                    prune_images()
                except Exception as pe:
                    log(f"prune after log error failed: {pe!r}")
            n += 1

            # Sleep in short steps so SIGTERM stops the loop promptly
            while running and time.monotonic() - t0 < PERIOD_S:
                time.sleep(max(0, min(0.5, PERIOD_S - (time.monotonic() - t0))))

    log("stopped")


if __name__ == "__main__":
    main()
