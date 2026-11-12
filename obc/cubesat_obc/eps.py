"""EPS: the OBC drives it directly (no STM32 on the EPS board).

  INA226 (module on EPS J2, I2C 0x40) - bus voltage / current between VBUS and VBUS_DIST
  PYRO_FIRE (EPS J6 pin 5)           - Pi GPIO -> IRLZ44N gate. The J26 PYRO_ARM
                                        plug must also be fitted for it to fire.

The CN3722 MPPT charger, the BMS and the buck converters have no digital
interface, so battery state comes from the INA226 bus voltage only.
"""
from __future__ import annotations

import logging
import struct
import threading
import time
from dataclasses import dataclass
from typing import Optional

from . import icd_defs as I
from .bus import BusError, I2CBus

log = logging.getLogger("eps")


class INA226:
    REG_CONFIG, REG_SHUNT, REG_BUS, REG_POWER, REG_CURRENT, REG_CAL = 0, 1, 2, 3, 4, 5
    REG_MFR, REG_DIE = 0xFE, 0xFF

    def __init__(self, bus: I2CBus, addr: int = I.ICD_ADDR_INA226_EPS, shunt_ohm: float = 0.01,
                 max_current_a: float = 3.2):
        self.bus, self.addr = bus, addr
        self.shunt = shunt_ohm
        # Current LSB rounded up to a tidy value; CAL = 0.00512 / (LSB * R)
        self.current_lsb = max_current_a / 32768.0
        self.cal = int(0.00512 / (self.current_lsb * shunt_ohm))
        if self.cal > 0xFFFF:
            raise ValueError("calibration out of range: check shunt value")

    def _wr(self, reg: int, val: int) -> None:
        self.bus.write(self.addr, bytes([reg, (val >> 8) & 0xFF, val & 0xFF]))

    def _rd(self, reg: int) -> int:
        b = self.bus.write_read(self.addr, bytes([reg]), 2)
        return (b[0] << 8) | b[1]  # INA226 registers are big-endian

    def init(self) -> bool:
        if self._rd(self.REG_MFR) != 0x5449:
            return False
        self._wr(self.REG_CONFIG, 0x4527)  # avg 16, 1.1 ms conversions, continuous shunt+bus
        self._wr(self.REG_CAL, self.cal)
        return True

    def read(self) -> dict:
        bus_raw = self._rd(self.REG_BUS)
        cur_raw = struct.unpack(">h", struct.pack(">H", self._rd(self.REG_CURRENT)))[0]
        pwr_raw = self._rd(self.REG_POWER)
        return {
            "vbus_mV": bus_raw * 1.25,
            "current_mA": cur_raw * self.current_lsb * 1000.0,
            "power_mW": pwr_raw * 25.0 * self.current_lsb * 1000.0,
        }


class GpioOut:
    """Pi GPIO output via gpiozero/lgpio. Falls back to a dry-run stub."""

    def __init__(self, pin: int, dry_run: bool = False):
        self.pin = pin
        self.state = False
        self._dev = None
        if not dry_run:
            from gpiozero import DigitalOutputDevice  # only on the Pi

            self._dev = DigitalOutputDevice(pin, active_high=True, initial_value=False)

    def set(self, on: bool) -> None:
        self.state = bool(on)
        if self._dev is not None:
            self._dev.value = 1 if on else 0


@dataclass
class PyroConfig:
    gpio: int = 17
    enabled: bool = False
    max_ms: int = 2000
    arm_window_s: float = 10.0


class Pyro:
    """Two-step fire: arm(key) then fire(ms) within the arm window.
    Refuses if disabled in config or if the OBC says it is not allowed now."""

    def __init__(self, cfg: PyroConfig, out: GpioOut):
        self.cfg, self.out = cfg, out
        self.armed_until = 0.0
        self.firing = False
        self.fire_count = 0
        self._lock = threading.Lock()
        out.set(False)

    def arm(self, key: int) -> bool:
        if not self.cfg.enabled or key != I.GL_PYRO_KEY:
            return False
        self.armed_until = time.monotonic() + self.cfg.arm_window_s
        log.warning("pyro ARMED for %.0f s", self.cfg.arm_window_s)
        return True

    def fire(self, duration_ms: int, allowed: bool = True) -> bool:
        with self._lock:
            if not (self.cfg.enabled and allowed) or self.firing:
                return False
            if time.monotonic() > self.armed_until:
                return False
            if not 0 < duration_ms <= self.cfg.max_ms:
                return False
            self.armed_until = 0.0
            self.firing = True
            self.fire_count += 1
        log.warning("pyro FIRE %d ms", duration_ms)
        threading.Thread(target=self._pulse, args=(duration_ms,), daemon=True).start()
        return True

    def _pulse(self, ms: int) -> None:
        try:
            self.out.set(True)
            time.sleep(ms / 1000.0)
        finally:
            self.out.set(False)
            self.firing = False

    def safe(self) -> None:
        self.armed_until = 0.0
        self.out.set(False)


class Eps:
    def __init__(self, ina: Optional[INA226], low_mv: float, recover_mv: float):
        self.ina = ina
        self.low_mv, self.recover_mv = low_mv, recover_mv
        self.ok = False
        self.last: dict = {}
        self.low_battery = False
        if ina is not None:
            try:
                self.ok = ina.init()
            except BusError as e:
                log.error("INA226 init failed: %s", e)
            if not self.ok:
                log.error("INA226 not found at 0x%02X", ina.addr)

    def poll(self) -> dict:
        if self.ina is None:
            return {}
        try:
            if not self.ok:
                self.ok = self.ina.init()
            self.last = self.ina.read() if self.ok else {}
        except BusError as e:
            self.ok = False
            self.last = {}
            log.error("INA226 read failed: %s", e)
        v = self.last.get("vbus_mV")
        if v is not None:
            # Hysteresis so the OBC does not toggle modes around the threshold.
            if not self.low_battery and v < self.low_mv:
                self.low_battery = True
                log.warning("LOW BATTERY %.0f mV", v)
            elif self.low_battery and v > self.recover_mv:
                self.low_battery = False
                log.warning("battery recovered %.0f mV", v)
        return self.last


class FakeINA226:
    """Simulated INA226 registers (for --sim and tests)."""

    def __init__(self, vbus_mv: float = 7600.0, current_ma: float = 450.0, shunt_ohm: float = 0.01,
                 max_current_a: float = 3.2):
        self.vbus_mv, self.current_ma = vbus_mv, current_ma
        self.lsb = max_current_a / 32768.0
        self.ptr = 0
        self.regs = {0xFE: 0x5449, 0xFF: 0x2260, 0: 0x4127, 5: 0}

    def write(self, data: bytes) -> None:
        self.ptr = data[0]
        if len(data) >= 3:
            self.regs[self.ptr] = (data[1] << 8) | data[2]

    def read(self, n: int) -> bytes:
        r = self.ptr
        if r == 2:
            v = int(self.vbus_mv / 1.25)
        elif r == 4:
            v = int(self.current_ma / 1000.0 / self.lsb) & 0xFFFF
        elif r == 3:
            v = int(self.vbus_mv * self.current_ma / 1000.0 / (25.0 * self.lsb * 1000.0))
        else:
            v = self.regs.get(r, 0)
        return bytes([(v >> 8) & 0xFF, v & 0xFF])[:n]
