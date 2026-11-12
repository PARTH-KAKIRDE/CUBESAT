"""I2C bus backends.

LinuxI2CBus  - real hardware (/dev/i2c-N via smbus2). Uses raw i2c_rdwr
               messages, NOT SMBus block reads: the ICD needs plain 32-byte reads.
SimBus       - the firmware simulation library (Software/build/host/libcubesat_sim.*,
               or the file named by CUBESAT_SIM_LIB), plus Python fake devices for
               the EPS/sensor chips.
BusError comes from the ICD master (ICD/python/icd_link.py), so the link layer,
EPS.py and Payload.py all catch the same class.
"""
from __future__ import annotations

import ctypes
import os
import platform
import time
from pathlib import Path
from typing import Dict, Optional, Protocol

from icd_link import BusError  # noqa: F401  (re-exported)


class I2CBus(Protocol):
    def write(self, addr: int, data: bytes) -> None: ...
    def read(self, addr: int, n: int) -> bytes: ...
    def write_read(self, addr: int, data: bytes, n: int) -> bytes: ...
    def wait_ms(self, ms: float) -> None: ...


class LinuxI2CBus:
    def __init__(self, bus_num: int):
        from smbus2 import SMBus, i2c_msg  # only needed on the Pi

        self._msg = i2c_msg
        self._bus = SMBus(bus_num)
        self.bus_num = bus_num

    def write(self, addr: int, data: bytes) -> None:
        try:
            self._bus.i2c_rdwr(self._msg.write(addr, list(data)))
        except OSError as e:
            raise BusError(f"i2c-{self.bus_num} 0x{addr:02X} write: {e}") from e

    def read(self, addr: int, n: int) -> bytes:
        m = self._msg.read(addr, n)
        try:
            self._bus.i2c_rdwr(m)
        except OSError as e:
            raise BusError(f"i2c-{self.bus_num} 0x{addr:02X} read: {e}") from e
        return bytes(m)

    def write_read(self, addr: int, data: bytes, n: int) -> bytes:
        """Register read with a repeated START (for sensor chips)."""
        w = self._msg.write(addr, list(data))
        r = self._msg.read(addr, n)
        try:
            self._bus.i2c_rdwr(w, r)
        except OSError as e:
            raise BusError(f"i2c-{self.bus_num} 0x{addr:02X} write_read: {e}") from e
        return bytes(r)

    def wait_ms(self, ms: float) -> None:
        time.sleep(ms / 1000.0)

    def close(self) -> None:
        self._bus.close()


class FakeDevice(Protocol):
    def write(self, data: bytes) -> None: ...
    def read(self, n: int) -> bytes: ...


def default_sim_lib() -> Path:
    name = {"Windows": "cubesat_sim.dll", "Darwin": "libcubesat_sim.dylib"}.get(platform.system(), "libcubesat_sim.so")
    return Path(__file__).resolve().parents[1] / "build" / "host" / name   # Software/build/host


class SimLib:
    """ctypes wrapper around the firmware simulation (see firmware/host/sim_api.h)."""

    def __init__(self, path: Optional[Path] = None):
        path = Path(os.environ.get("CUBESAT_SIM_LIB", path or default_sim_lib()))
        if not path.exists():
            raise FileNotFoundError(f"{path} not found: build the firmware simulation "
                                    "(firmware/build.py sim) or set CUBESAT_SIM_LIB")
        self.path = path
        self.lib = L = ctypes.CDLL(str(path))
        u8p = ctypes.POINTER(ctypes.c_uint8)
        L.sim_init.argtypes = [ctypes.c_uint32]
        L.sim_run_ms.argtypes = [ctypes.c_uint32]
        L.sim_now.restype = ctypes.c_uint32
        L.sim_i2c_write.argtypes = [ctypes.c_uint8, u8p, ctypes.c_int]
        L.sim_i2c_read.argtypes = [ctypes.c_uint8, u8p, ctypes.c_int]
        L.sim_set_present.argtypes = [ctypes.c_uint8, ctypes.c_int]
        L.sim_reset_slave.argtypes = [ctypes.c_uint8]
        L.sim_adcs_set_rate_dps.argtypes = [ctypes.c_double] * 3
        L.sim_adcs_get_rate_dps.argtypes = [ctypes.POINTER(ctypes.c_double)]
        L.sim_adcs_set_faults.argtypes = [ctypes.c_uint32]
        L.sim_adcs_dirty_mag_samples.restype = ctypes.c_uint32
        L.sim_ttc_uplink.argtypes = [u8p, ctypes.c_int, ctypes.c_int]
        L.sim_ttc_downlink.argtypes = [u8p]
        L.sim_ttc_set_pins.argtypes = [ctypes.c_int, ctypes.c_int]
        L.sim_ttc_burn_total_ms.restype = ctypes.c_uint32
        L.sim_ttc_set_radio_ok.argtypes = [ctypes.c_int]

    def init(self, seed: int = 1) -> None:
        self.lib.sim_init(seed)

    def run_ms(self, ms: int) -> None:
        if ms > 0:
            self.lib.sim_run_ms(int(ms))

    def now_ms(self) -> int:
        return self.lib.sim_now()

    def rate_dps(self):
        a = (ctypes.c_double * 3)()
        self.lib.sim_adcs_get_rate_dps(a)
        return tuple(a)

    def uplink(self, pkt: bytes, rssi: int = -100) -> None:
        buf = (ctypes.c_uint8 * len(pkt)).from_buffer_copy(pkt)
        self.lib.sim_ttc_uplink(buf, len(pkt), rssi)

    def downlink(self) -> list:
        out = []
        buf = (ctypes.c_uint8 * 255)()
        while True:
            n = self.lib.sim_ttc_downlink(buf)
            if n <= 0:
                return out
            out.append(bytes(buf[:n]))


class SimBus:
    """I2C bus backed by the firmware simulation. Time only moves in wait_ms."""

    def __init__(self, sim: SimLib, fakes: Optional[Dict[int, FakeDevice]] = None):
        self.sim = sim
        self.fakes: Dict[int, FakeDevice] = dict(fakes or {})

    def write(self, addr: int, data: bytes) -> None:
        if addr in self.fakes:
            self.fakes[addr].write(bytes(data))
            return
        buf = (ctypes.c_uint8 * max(1, len(data))).from_buffer_copy(bytes(data) or b"\0")
        if self.sim.lib.sim_i2c_write(addr, buf, len(data)) != 0:
            raise BusError(f"sim 0x{addr:02X} write NACK")

    def read(self, addr: int, n: int) -> bytes:
        if addr in self.fakes:
            return self.fakes[addr].read(n)
        buf = (ctypes.c_uint8 * n)()
        if self.sim.lib.sim_i2c_read(addr, buf, n) != 0:
            raise BusError(f"sim 0x{addr:02X} read NACK")
        return bytes(buf)

    def write_read(self, addr: int, data: bytes, n: int) -> bytes:
        self.write(addr, data)
        return self.read(addr, n)

    def wait_ms(self, ms: float) -> None:
        self.sim.run_ms(int(round(ms)))
