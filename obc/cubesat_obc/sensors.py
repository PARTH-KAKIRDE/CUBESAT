"""Payload sensors driven by the OBC: TSL2591 light sensor (I2C 0x29) and a USB camera."""
from __future__ import annotations

import logging
import shutil
import subprocess
from pathlib import Path

from . import icd_defs as I
from .bus import BusError, I2CBus

log = logging.getLogger("sensors")


class TSL2591:
    CMD = 0xA0  # command bit + normal operation
    ENABLE, CONTROL, ID, C0DATAL = 0x00, 0x01, 0x12, 0x14
    GAIN_MED, ATIME_100MS = 0x10, 0x00
    AGAIN, ATIME_MS = 25.0, 100.0

    def __init__(self, bus: I2CBus, addr: int = I.ICD_ADDR_TSL2591):
        self.bus, self.addr = bus, addr
        self.ok = False

    def init(self) -> bool:
        try:
            ident = self.bus.write_read(self.addr, bytes([self.CMD | self.ID]), 1)[0]
            if ident != 0x50:
                log.error("TSL2591 bad id 0x%02X", ident)
                return False
            self.bus.write(self.addr, bytes([self.CMD | self.CONTROL, self.GAIN_MED | self.ATIME_100MS]))
            self.bus.write(self.addr, bytes([self.CMD | self.ENABLE, 0x03]))  # PON | AEN
            self.ok = True
        except BusError as e:
            log.error("TSL2591 init failed: %s", e)
            self.ok = False
        return self.ok

    def read_lux(self):
        if not self.ok and not self.init():
            return None
        try:
            b = self.bus.write_read(self.addr, bytes([self.CMD | self.C0DATAL]), 4)
        except BusError as e:
            log.error("TSL2591 read failed: %s", e)
            self.ok = False
            return None
        full = b[0] | (b[1] << 8)
        ir = b[2] | (b[3] << 8)
        if full == 0xFFFF or ir == 0xFFFF:
            return float("inf")  # saturated
        if full == 0:
            return 0.0
        cpl = (self.ATIME_MS * self.AGAIN) / 408.0
        return max(0.0, (full - ir) * (1.0 - ir / full) / cpl)


class FakeTSL2591:
    def __init__(self, full: int = 1200, ir: int = 300):
        self.full, self.ir, self.ptr = full, ir, 0

    def write(self, data: bytes) -> None:
        self.ptr = data[0] & 0x1F

    def read(self, n: int) -> bytes:
        if self.ptr == 0x12:
            return bytes([0x50])[:n]
        if self.ptr == 0x14:
            return bytes([self.full & 0xFF, self.full >> 8, self.ir & 0xFF, self.ir >> 8])[:n]
        return bytes(n)


class Camera:
    """USB camera snapshot. Uses fswebcam (apt install fswebcam) if present,
    otherwise OpenCV (apt install python3-opencv)."""

    def __init__(self, device: str = "/dev/video0", resolution: str = "1280x720", dry_run: bool = False):
        self.device, self.resolution, self.dry_run = device, resolution, dry_run

    def capture(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        if self.dry_run:
            path.write_bytes(b"SIMULATED IMAGE\n")
            return path
        if shutil.which("fswebcam"):
            r = subprocess.run(["fswebcam", "-d", self.device, "-r", self.resolution, "--no-banner",
                                "-S", "5", str(path)], capture_output=True, timeout=30)
            if r.returncode == 0 and path.exists():
                return path
            raise RuntimeError(f"fswebcam failed: {r.stderr.decode(errors='replace')[-200:]}")
        import cv2  # noqa: PLC0415

        cap = cv2.VideoCapture(self.device)
        try:
            w, h = (int(x) for x in self.resolution.split("x"))
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, w)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, h)
            for _ in range(5):  # let auto-exposure settle
                cap.read()
            ok, frame = cap.read()
            if not ok:
                raise RuntimeError("camera returned no frame")
            cv2.imwrite(str(path), frame)
        finally:
            cap.release()
        return path
