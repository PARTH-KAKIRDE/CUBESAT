#!/usr/bin/env python3
"""CubeSat payload for the OBC (Raspberry Pi 4B).

Sensors
  - TSL2591   relative solar flux. Pi sensor bus (i2c-3), 0x29.
  - VEML6075  UV. Pi sensor bus, 0x10. Off by default: its address is commented
              out in the ICD (ICD_ADDR_VEML6075). Uncomment it there first.
  - Pi camera CSI, through rpicam-still (libcamera-still on older Pi OS).
  - MMC5983MA magnetometer, on the ADCS board. Never read from here: ADCS samples
              it with the magnetorquers off (coils off -> settle -> sample -> coils on)
              and the OBC already reads ADCS telemetry page 0 every second.
              This module only logs the samples the OBC hands it, and only fresh ones.

Merged from Software/payload.py (standalone script) and the base repo's OBC code
(obc/cubesat_obc/sensors.py + obc_app.py in ~/Downloads/code):
  kept from payload.py  raw TSL2591 counts as the science data, the 100 ms
                        saturation limit (36863), low gain, rpicam-still,
                        atomic image writes, image pruning, fsync for power cuts,
                        repair of a half-written last line
  kept from the OBC     the i2c-3 sensor bus (never the i2c-1 STM32 link), UTC
                        timestamps, JSON lines, the OBC's mode / low battery,
                        the ADCS magnetometer freshness rule
  fixed in both         full and IR read in ONE I2C transaction; lux computed
                        from those same counts; no sample until the chip reports
                        a finished integration (AVALID); saturation detected at
                        36863 (the OBC only checked 65535 and reported 0 lux in
                        full sun); the camera runs in the background so it
                        never stalls the 1 s OBC loop; no images in SAFE or
                        on low battery

Wired into OBC/obc_app.py: Payload(...) at start-up, tick(mode, low_battery,
ADCS page-0 sensors) every 1 s tick, request_capture() for the ground
CAPTURE_IMAGE command, close() at shutdown. Settings: [payload] in obc.toml.

Standalone (no magnetometer, since this process must never talk to ADCS):
    python3 Payload.py --bus 3 --data-dir /home/pi/cubesat_data
"""
from __future__ import annotations

import argparse
import copy
import json
import logging
import os
import shutil
import shlex
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Optional

# The ICD folder is the single source of truth: constants and the I2C master come
# from Software/ICD/python (the OBC package's __init__ does the same).
_ICD_PY = Path(__file__).resolve().parent.parent / "ICD" / "python"
if str(_ICD_PY) not in sys.path:
    sys.path.insert(0, str(_ICD_PY))
import icd_defs as I                    # noqa: E402
from icd_link import BusError           # noqa: E402  (one BusError class for every bus backend)

log = logging.getLogger("payload")

# Every setting can be overridden from the [payload] table of obc.toml.
DEFAULTS = {
    "sample_period_s": 10.0,   # TSL2591 / VEML6075 sample period
    "fsync_period_s": 10.0,    # flush the log to the SD card at least this often
    "tsl2591": {
        "enabled": True,
        "gain": "low",          # low | med | high | max. Low saturates last (direct sun still saturates it)
        "integration_ms": 100,  # 100..600 in steps of 100
        "reset_after_errors": 5,
    },
    "veml6075": {
        "enabled": False,       # needs ICD_ADDR_VEML6075 uncommented in the ICD
        "integration_ms": 100,  # 50 | 100 | 200 | 400 | 800
        # Vishay app note "Designing the VEML6075 into an application",
        # open sensor, no window. Recalibrate for the real cover glass.
        "coef_a": 2.22, "coef_b": 1.33, "coef_c": 2.95, "coef_d": 1.74,
        "uva_resp_100ms": 0.001461, "uvb_resp_100ms": 0.002591,
        "reset_after_errors": 5,
    },
    "camera": {
        "enabled": True,
        "period_s": 60.0,        # automatic image every N seconds (0 = only on ground command)
        "command": "",           # "" = rpicam-still (or libcamera-still); a command line or list overrides
        "width": 0, "height": 0,  # 0 = sensor default
        "quality": 90,
        "settle_ms": 1000,       # rpicam -t: lets auto-exposure settle
        "timeout_s": 20.0,       # kill the capture if it takes longer
        "max_images_mb": 2000,   # delete the oldest images above this...
        "min_free_mb": 500,      # ...or when free space drops below this
        "reserve_mb": 10,        # headroom for the next image
    },
    "mag": {"enabled": True},
}

MODE_SAFE = I.ICD_MODE_SAFE


def merge_config(over: Optional[dict]) -> dict:
    cfg = copy.deepcopy(DEFAULTS)

    def _m(base, o):
        for k, v in (o or {}).items():
            if isinstance(v, dict) and isinstance(base.get(k), dict):
                _m(base[k], v)
            else:
                base[k] = v
    _m(cfg, over)
    return cfg


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def next_due(due: float, period: float, now: float) -> float:
    """Keep a steady cadence; after a stall or a SAFE pause, the next event is a
    full period from now (no catch-up burst)."""
    nxt = due + period
    return nxt if nxt > now else now + period


def fsync_path(path) -> None:
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


# =====================================================================
# TSL2591 (ams), relative solar flux
# =====================================================================

class TSL2591:
    CMD = 0xA0                      # command bit + normal transaction
    REG_ENABLE, REG_CONTROL, REG_ID, REG_STATUS, REG_C0DATAL = 0x00, 0x01, 0x12, 0x13, 0x14
    CHIP_ID = 0x50
    PON, AEN, AVALID = 0x01, 0x02, 0x01
    GAINS = {"low": (0x00, 1.0), "med": (0x10, 25.0), "high": (0x20, 428.0), "max": (0x30, 9876.0)}
    LUX_DF, COEF_B, COEF_C, COEF_D = 408.0, 1.64, 0.59, 0.86

    def __init__(self, bus, addr: int = I.ICD_ADDR_TSL2591, gain: str = "low", integration_ms: int = 100):
        if gain not in self.GAINS:
            raise ValueError(f"TSL2591 gain must be one of {sorted(self.GAINS)}")
        if integration_ms not in (100, 200, 300, 400, 500, 600):
            raise ValueError("TSL2591 integration_ms must be 100..600 in steps of 100")
        self.bus, self.addr = bus, addr
        self.gain_bits, self.again = self.GAINS[gain]
        self.gain_name, self.atime_ms = gain, integration_ms
        # Highest count a channel can reach: 36863 at 100 ms, 65535 otherwise.
        self.max_count = 36863 if integration_ms == 100 else 65535
        self.ok = False

    def _reg(self, reg: int, n: int) -> bytes:
        return self.bus.write_read(self.addr, bytes([self.CMD | reg]), n)

    def init(self) -> None:
        self.ok = False
        ident = self._reg(self.REG_ID, 1)[0]
        if ident != self.CHIP_ID:
            raise BusError(f"TSL2591 bad id 0x{ident:02X}")
        atime_bits = self.atime_ms // 100 - 1
        self.bus.write(self.addr, bytes([self.CMD | self.REG_CONTROL, self.gain_bits | atime_bits]))
        self.bus.write(self.addr, bytes([self.CMD | self.REG_ENABLE, self.PON | self.AEN]))
        self.ok = True

    def read(self) -> dict:
        """One sample. Raises BusError. status: OK | SATURATED | NOT_READY."""
        if not self.ok:
            self.init()
        if not (self._reg(self.REG_STATUS, 1)[0] & self.AVALID):
            # No integration finished since power-on/enable: the data registers are not valid yet.
            return {"status": "NOT_READY"}
        b = self._reg(self.REG_C0DATAL, 4)     # C0 and C1 in one transaction
        full, ir = b[0] | (b[1] << 8), b[2] | (b[3] << 8)
        saturated = full >= self.max_count or ir >= self.max_count
        lux = None
        if not saturated and full > 0:
            cpl = self.atime_ms * self.again / self.LUX_DF
            lux = max(0.0, (full - self.COEF_B * ir) / cpl, (self.COEF_C * full - self.COEF_D * ir) / cpl)
            lux = round(lux, 3)
        elif not saturated:
            lux = 0.0
        return {"status": "SATURATED" if saturated else "OK", "full": full, "ir": ir,
                "vis": max(full - ir, 0), "lux": lux,  # lux: uncalibrated, relative only
                "gain": self.gain_name, "atime_ms": self.atime_ms}


class FakeTSL2591:
    """Register-level TSL2591 for the PC simulation and tests: chip id, gain/time
    register, and AVALID set from the second STATUS read after enable."""

    def __init__(self, full: int = 1200, ir: int = 300):
        self.full, self.ir = full, ir
        self.enable = 0
        self.control = 0
        self.status_reads = 0
        self.ptr = 0
        self.present = True

    def write(self, data: bytes) -> None:
        if not self.present:
            raise BusError("TSL2591 NACK")
        self.ptr = data[0] & 0x1F
        if len(data) >= 2:
            if self.ptr == TSL2591.REG_ENABLE:
                if not self.enable & TSL2591.AEN and data[1] & TSL2591.AEN:
                    self.status_reads = 0            # integration restarts
                self.enable = data[1]
            elif self.ptr == TSL2591.REG_CONTROL:
                self.control = data[1]

    def read(self, n: int) -> bytes:
        if not self.present:
            raise BusError("TSL2591 NACK")
        if self.ptr == TSL2591.REG_ID:
            return bytes([TSL2591.CHIP_ID])[:n]
        if self.ptr == TSL2591.REG_STATUS:
            self.status_reads += 1
            on = self.enable & (TSL2591.PON | TSL2591.AEN) == TSL2591.PON | TSL2591.AEN
            return bytes([TSL2591.AVALID if on and self.status_reads > 1 else 0])[:n]
        if self.ptr == TSL2591.REG_C0DATAL:
            return bytes([self.full & 0xFF, self.full >> 8, self.ir & 0xFF, self.ir >> 8])[:n]
        return bytes(n)


# =====================================================================
# VEML6075 (Vishay), UVA / UVB / UV index
# =====================================================================

class VEML6075:
    REG_CONF, REG_UVA, REG_UVB, REG_COMP1, REG_COMP2, REG_ID = 0x00, 0x07, 0x09, 0x0A, 0x0B, 0x0C
    CHIP_ID = 0x26
    IT_CODES = {50: 0, 100: 1, 200: 2, 400: 3, 800: 4}
    MAX_COUNT = 65535

    def __init__(self, bus, addr: int, cfg: dict, clock: Callable[[], float]):
        if cfg["integration_ms"] not in self.IT_CODES:
            raise ValueError(f"VEML6075 integration_ms must be one of {sorted(self.IT_CODES)}")
        self.bus, self.addr, self.cfg, self.clock = bus, addr, cfg, clock
        self.it_ms = cfg["integration_ms"]
        self.ok = False
        self.ready_at = 0.0

    def _word(self, reg: int) -> int:
        b = self.bus.write_read(self.addr, bytes([reg]), 2)
        return b[0] | (b[1] << 8)

    def init(self) -> None:
        self.ok = False
        ident = self._word(self.REG_ID) & 0xFF
        if ident != self.CHIP_ID:
            raise BusError(f"VEML6075 bad id 0x{ident:02X}")
        conf = self.IT_CODES[self.it_ms] << 4        # SD=0 (on), continuous, normal dynamic
        self.bus.write(self.addr, bytes([self.REG_CONF, conf, 0x00]))
        # It has no data-ready bit: wait two integration times for a full result.
        self.ready_at = self.clock() + 2 * self.it_ms / 1000.0
        self.ok = True

    def read(self) -> dict:
        if not self.ok:
            self.init()
        if self.clock() < self.ready_at:
            return {"status": "NOT_READY"}
        uva, uvb = self._word(self.REG_UVA), self._word(self.REG_UVB)
        c1, c2 = self._word(self.REG_COMP1), self._word(self.REG_COMP2)
        saturated = max(uva, uvb, c1, c2) >= self.MAX_COUNT
        out = {"status": "SATURATED" if saturated else "OK", "uva": uva, "uvb": uvb,
               "comp1": c1, "comp2": c2, "it_ms": self.it_ms, "uvi": None}
        if not saturated:
            k = self.cfg
            scale = 100.0 / self.it_ms                # responsivity is given for 100 ms
            uva_c = uva - k["coef_a"] * c1 - k["coef_b"] * c2
            uvb_c = uvb - k["coef_c"] * c1 - k["coef_d"] * c2
            uvia = uva_c * k["uva_resp_100ms"] * scale
            uvib = uvb_c * k["uvb_resp_100ms"] * scale
            out["uvi"] = round(max(0.0, (uvia + uvib) / 2.0), 3)  # approximate, see coefficients
        return out


# =====================================================================
# Pi camera (non-blocking)
# =====================================================================

class PiCamera:
    """Captures in a child process; poll() is called every OBC tick and never blocks."""
    TMP_NAME = "capture_tmp.jpg"
    ERR_NAME = "capture_stderr.txt"   # libcamera is chatty: a file, not a pipe that could fill up

    def __init__(self, cfg: dict, image_dir: Path, clock: Callable[[], float]):
        self.cfg, self.dir, self.clock = cfg, Path(image_dir), clock
        cmd = cfg["command"]
        if isinstance(cmd, str):
            cmd = shlex.split(cmd)
        if not cmd:
            found = shutil.which("rpicam-still") or shutil.which("libcamera-still")
            cmd = [found] if found else None
        self.cmd = list(cmd) if cmd else None
        self.proc: Optional[subprocess.Popen] = None
        self._err = None
        self.started = 0.0
        self.target: Optional[Path] = None
        self.dir.mkdir(parents=True, exist_ok=True)
        tmp = self.dir / self.TMP_NAME
        if tmp.exists():                      # left over from a power cut mid-capture
            tmp.unlink()
        self.seq = self._next_seq()
        if self.cmd is None:
            log.warning("rpicam-still / libcamera-still not found: images will fail")

    # ---- image files
    @staticmethod
    def img_seq(name: str) -> Optional[int]:
        """Sequence number of img_<seq>[_<utc>].jpg, or None if it isn't ours."""
        if not (name.startswith("img_") and name.endswith(".jpg")):
            return None
        head = name[4:-4].split("_", 1)[0]
        return int(head) if head.isdigit() else None

    def image_names(self) -> list:
        return sorted((n for n in os.listdir(self.dir) if self.img_seq(n) is not None), key=self.img_seq)

    def _next_seq(self) -> int:
        names = self.image_names()
        return self.img_seq(names[-1]) + 1 if names else 0

    def prune(self) -> None:
        """Delete the oldest images so the next one stays under max_images_mb and
        above min_free_mb, with reserve_mb of headroom."""
        mb, c = 1024 * 1024, self.cfg
        names = self.image_names()
        total = sum((self.dir / n).stat().st_size for n in names)
        while names and (total > (c["max_images_mb"] - c["reserve_mb"]) * mb
                         or shutil.disk_usage(self.dir).free < (c["min_free_mb"] + c["reserve_mb"]) * mb):
            p = self.dir / names.pop(0)
            total -= p.stat().st_size
            p.unlink()
            log.info("pruned %s", p.name)

    # ---- capture
    @property
    def busy(self) -> bool:
        return self.proc is not None

    def start(self) -> bool:
        if self.busy:
            return False
        if self.cmd is None:
            raise RuntimeError("rpicam-still / libcamera-still not found")
        try:
            self.prune()
        except OSError as e:                 # a pruning problem is not a camera problem
            log.error("prune error: %r", e)
        c = self.cfg
        args = [*self.cmd, "-n", "-t", str(int(c["settle_ms"])), "-q", str(int(c["quality"])),
                "-o", str(self.dir / self.TMP_NAME)]
        if c["width"] and c["height"]:
            args += ["--width", str(int(c["width"])), "--height", str(int(c["height"]))]
        self.target = self.dir / f"img_{self.seq:06d}_{utc_now():%Y%m%dT%H%M%SZ}.jpg"
        self._err = open(self.dir / self.ERR_NAME, "w+b")
        try:
            self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=self._err,
                                         start_new_session=True)
        except Exception:
            self._err.close()
            raise
        self.started = self.clock()
        return True

    def poll(self) -> Optional[dict]:
        """None while idle or still capturing; otherwise the finished result."""
        if self.proc is None:
            return None
        rc = self.proc.poll()
        took = round(self.clock() - self.started, 2)
        if rc is None:
            if took < self.cfg["timeout_s"]:
                return None
            self._kill()
            return self._fail(f"timeout after {took} s", took)
        err = ""
        try:
            self._err.seek(0)
            err = self._err.read().decode(errors="replace")[-200:]
        except (OSError, ValueError):
            pass
        self._reap()
        tmp = self.dir / self.TMP_NAME
        try:
            if rc != 0:
                raise RuntimeError(f"exit {rc}: {err.strip()}")
            with open(tmp, "rb") as f:
                if f.read(2) != b"\xff\xd8":
                    raise RuntimeError("not a JPEG (empty or corrupt file)")
            fsync_path(tmp)                   # image data on disk before the rename
            os.replace(tmp, self.target)      # atomic: never a half-written image
            fsync_path(self.dir)
        except Exception as e:  # noqa: BLE001
            return self._fail(repr(e), took)
        name = self.target.name
        self.seq += 1
        return {"image": name, "capture_s": took}

    def _fail(self, why: str, took: float) -> dict:
        tmp = self.dir / self.TMP_NAME
        try:
            if tmp.exists():
                tmp.unlink()
        except OSError:
            pass
        log.error("camera: %s", why)
        return {"image": "CAM_FAIL", "error": why[:120], "capture_s": took}

    def _kill(self) -> None:
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
        self._reap()

    def _reap(self) -> None:
        if self._err is not None:
            self._err.close()
            self._err = None
        self.proc = None

    def close(self) -> None:
        if self.proc is not None:
            self._kill()


# =====================================================================
# ADCS magnetometer samples (logged, never read directly)
# =====================================================================

class MagLogger:
    """Takes the dict from AdcsClient.sensors() and returns a record for each
    NEW, FRESH sample, else None. ADCS took every sample with the coils off; a
    sample is skipped if it is invalid, stale (older than period + 500 ms, the
    OBC's rule) or the same sample as last time."""

    def __init__(self):
        self.last_sample_ms: Optional[int] = None

    def update(self, s: Optional[dict]) -> Optional[dict]:
        if not s or s.get("mag_uT") is None or s.get("mag_stale", True) or s.get("mag_age_ms") is None:
            return None
        sample_ms = (int(s["uptime_ms"]) - int(s["mag_age_ms"])) & 0xFFFFFFFF  # ADCS clock, wraps
        if sample_ms == self.last_sample_ms:
            return None
        self.last_sample_ms = sample_ms
        return {"adcs_sample_ms": sample_ms, "age_ms": int(s["mag_age_ms"]),
                "uT": [round(float(v), 3) for v in s["mag_uT"]]}


# =====================================================================
# Payload manager
# =====================================================================

class Payload:
    def __init__(self, cfg: Optional[dict], sensor_bus, data_dir, clock: Callable[[], float] = time.monotonic):
        self.cfg = merge_config(cfg)
        self.bus, self.clock = sensor_bus, clock
        self.dir = Path(data_dir) / "payload"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.t0 = clock()
        self.next_sample = self.t0
        self.next_image = self.t0 + 5.0 if self.cfg["camera"]["period_s"] > 0 else None
        self.last_fsync = self.t0
        self.dirty = False
        self.images = 0
        self.last_lux: Optional[float] = None   # for the OBC beacon/HK: None, inf (saturated) or lux
        self._f = None
        self._day = None
        self.errors = {"tsl2591": 0, "veml6075": 0}

        c = self.cfg
        self.tsl = None
        if c["tsl2591"]["enabled"] and sensor_bus is not None:
            self.tsl = TSL2591(sensor_bus, I.ICD_ADDR_TSL2591, c["tsl2591"]["gain"], c["tsl2591"]["integration_ms"])
        self.uv = None
        if c["veml6075"]["enabled"] and sensor_bus is not None:
            addr = getattr(I, "ICD_ADDR_VEML6075", None)
            if addr is None:
                log.error("VEML6075 enabled but ICD_ADDR_VEML6075 is commented out in cubesat_icd.h: "
                          "uncomment it and re-run gen_icd_py.py. UV sensor disabled.")
            else:
                self.uv = VEML6075(sensor_bus, addr, c["veml6075"], clock)
        self.cam = PiCamera(c["camera"], Path(data_dir) / "images", clock) if c["camera"]["enabled"] else None
        self.mag = MagLogger() if c["mag"]["enabled"] else None

    # ---------------------------------------------------------- public
    @staticmethod
    def camera_allowed(obc_mode: Optional[int], low_battery: bool) -> bool:
        return obc_mode != MODE_SAFE and not low_battery

    def request_capture(self, obc_mode: Optional[int] = None, low_battery: bool = False) -> str:
        """Ground CAPTURE_IMAGE. Returns started | busy | refused | disabled | error.
        The image is saved a few seconds later; the next tick logs its name."""
        if self.cam is None:
            return "disabled"
        if not self.camera_allowed(obc_mode, low_battery):
            return "refused"
        if self.cam.busy:
            return "busy"
        try:
            self.cam.start()
            return "started"
        except Exception as e:  # noqa: BLE001
            log.error("camera start: %r", e)
            return "error"

    def tick(self, obc_mode: Optional[int] = None, low_battery: bool = False,
             adcs_sensors: Optional[dict] = None) -> dict:
        """Call once per OBC tick (1 s). Never raises, never blocks on the camera.
        Returns what was new this tick (also written to payload/payload_<day>.jsonl)."""
        now = self.clock()
        rec: dict = {}
        self._step("camera", self._camera_step, rec, now, obc_mode, low_battery)
        if now >= self.next_sample:
            self.next_sample = next_due(self.next_sample, self.cfg["sample_period_s"], now)
            if self.tsl is not None:
                rec["tsl2591"] = self._sensor("tsl2591", self.tsl)
                self._update_last_lux(rec["tsl2591"])
            if self.uv is not None:
                rec["veml6075"] = self._sensor("veml6075", self.uv)
        if self.mag is not None:
            self._step("mag", lambda r: r.__setitem__("mag", self.mag.update(adcs_sensors)), rec)
            if rec.get("mag") is None:
                rec.pop("mag", None)
        if rec:
            rec = {"t": utc_now().isoformat(timespec="milliseconds"),
                   "uptime_s": round(now - self.t0, 1), "obc_mode": obc_mode, **rec}
            self._write(rec)
        if self.dirty and now - self.last_fsync >= self.cfg["fsync_period_s"]:
            self._fsync()
        return rec

    def close(self) -> None:
        if self.cam is not None:
            self.cam.close()
        if self._f is not None:
            self._fsync()
            self._f.close()
            self._f = None

    # ---------------------------------------------------------- internals
    def _step(self, what: str, fn, *args) -> None:
        try:
            fn(*args)
        except Exception as e:  # noqa: BLE001 - one part failing never stops the others
            log.error("%s: %r", what, e)

    def _camera_step(self, rec: dict, now: float, obc_mode, low_battery) -> None:
        if self.cam is None:
            return
        res = self.cam.poll()
        if res is not None:
            rec["camera"] = res
            if res["image"] != "CAM_FAIL":
                self.images += 1
        if self.next_image is not None and now >= self.next_image:
            if self.camera_allowed(obc_mode, low_battery) and not self.cam.busy:
                self.next_image = next_due(self.next_image, self.cfg["camera"]["period_s"], now)
                try:
                    self.cam.start()
                except Exception as e:  # noqa: BLE001
                    rec["camera"] = {"image": "CAM_FAIL", "error": repr(e)[:120]}
                    log.error("camera start: %r", e)

    def _sensor(self, name: str, dev) -> dict:
        try:
            d = dev.read()
            self.errors[name] = 0
            return d
        except Exception as e:  # noqa: BLE001 - unplugged sensor, bus error, bad id
            dev.ok = False
            self.errors[name] += 1
            limit = self.cfg[name]["reset_after_errors"]
            if self.errors[name] >= limit and hasattr(self.bus, "reset"):
                log.warning("%s: %d errors in a row, resetting the sensor bus", name, self.errors[name])
                self._step("bus reset", self.bus.reset)
                self.errors[name] = 0
            log.error("%s: %r", name, e)
            return {"status": f"ERR:{type(e).__name__}"}

    def _update_last_lux(self, d: dict) -> None:
        if d.get("status") == "OK":
            self.last_lux = d["lux"]
        elif d.get("status") == "SATURATED":
            self.last_lux = float("inf")
        elif d.get("status", "").startswith("ERR"):
            self.last_lux = None

    def _open(self) -> None:
        day = utc_now().strftime("%Y-%m-%d")
        if self._f is not None and day == self._day:
            return
        if self._f is not None:
            self._fsync()
            self._f.close()
        path = self.dir / f"payload_{day}.jsonl"
        needs_nl = False
        if path.exists() and path.stat().st_size > 0:
            with open(path, "rb") as rf:      # a power cut may have left half a line
                rf.seek(-1, os.SEEK_END)
                needs_nl = rf.read(1) != b"\n"
        self._f = open(path, "a", encoding="utf-8")
        self._day = day
        if needs_nl:
            self._f.write("\n")

    def _write(self, rec: dict) -> None:
        try:
            self._open()
            self._f.write(json.dumps(rec, allow_nan=False) + "\n")
            self._f.flush()
            self.dirty = True
        except (OSError, ValueError) as e:     # disk full / I/O error must not kill the loop
            log.error("payload log write: %r", e)
            if self.cam is not None:
                self._step("prune", self.cam.prune)

    def _fsync(self) -> None:
        try:
            if self._f is not None:
                self._f.flush()
                os.fsync(self._f.fileno())
            self.dirty = False
        except OSError as e:
            log.error("payload fsync: %r", e)
        self.last_fsync = self.clock()


# =====================================================================
# Standalone mode
# =====================================================================

class SMBus2Bus:
    """/dev/i2c-N through smbus2 (sudo apt install python3-smbus2), with
    repeated-START register reads. Same interface as OBC/bus.py LinuxI2CBus."""

    def __init__(self, bus_num: int):
        self.bus_num = bus_num
        self._open()

    def _open(self) -> None:
        from smbus2 import SMBus, i2c_msg
        self._msg, self._bus = i2c_msg, SMBus(self.bus_num)

    def write(self, addr: int, data: bytes) -> None:
        try:
            self._bus.i2c_rdwr(self._msg.write(addr, list(data)))
        except OSError as e:
            raise BusError(f"i2c-{self.bus_num} 0x{addr:02X} write: {e}") from e

    def write_read(self, addr: int, data: bytes, n: int) -> bytes:
        w, r = self._msg.write(addr, list(data)), self._msg.read(addr, n)
        try:
            self._bus.i2c_rdwr(w, r)
        except OSError as e:
            raise BusError(f"i2c-{self.bus_num} 0x{addr:02X} write_read: {e}") from e
        return bytes(r)

    def reset(self) -> None:
        try:
            self._bus.close()
        except OSError:
            pass
        self._open()

    def close(self) -> None:
        self._bus.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="CubeSat payload, standalone (no magnetometer)")
    ap.add_argument("--bus", type=int, default=3, help="Pi sensor bus (ICD: i2c-3)")
    ap.add_argument("--data-dir", default=os.environ.get("PAYLOAD_DIR", "payload_data"))
    ap.add_argument("--config", help="TOML file with a [payload] table (same keys as obc.toml)")
    ap.add_argument("--no-camera", action="store_true")
    ap.add_argument("--ticks", type=int, help="stop after N ticks (testing)")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="[payload] %(levelname)s %(message)s")

    cfg = {}
    if a.config:
        import tomllib
        with open(a.config, "rb") as f:
            cfg = tomllib.load(f).get("payload", {})
    cfg.setdefault("mag", {})["enabled"] = False   # never talk to ADCS from here
    if a.no_camera:
        cfg.setdefault("camera", {})["enabled"] = False
    if a.bus == 1:
        log.warning("i2c-1 is the ADCS/TT&C link bus; the ICD puts payload sensors on i2c-3")

    try:
        bus = SMBus2Bus(a.bus)
    except Exception as e:  # noqa: BLE001
        log.error("cannot open i2c-%d (%r): light/UV sensors disabled", a.bus, e)
        bus = None

    running = True

    def stop(*_):
        nonlocal running
        running = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    p = Payload(cfg, bus, a.data_dir)
    n, nxt = 0, time.monotonic()
    try:
        while running and (a.ticks is None or n < a.ticks):
            p.tick()
            n += 1
            nxt += 1.0
            while running and time.monotonic() < nxt:      # short sleeps: SIGTERM stops promptly
                time.sleep(min(0.2, max(0.0, nxt - time.monotonic())))
    finally:
        p.close()
        if bus is not None:
            bus.close()
    log.info("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
