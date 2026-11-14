#!/usr/bin/env python3
"""CubeSat EPS, driven by the OBC (Raspberry Pi 4B). The EPS board has no MCU.

Hardware (EPS Rev 1.0, KiCad):
    SOLAR_IN -> fuse -> reverse-polarity P-MOSFET -> TVS -> CN3722 2S MPPT charger -> VBUS
    battery pack -> J4 -> BMS (J3) -> VBUS          (the battery sits on the bus)
    VBUS -> INA226 #1 (J11, 0x40) -> VBUS_DIST -> 5 V / 3.3 V bucks, AUX
    PYRO_FIRE: Pi GPIO17 -> IRLZ44N gate (J6 pin 5); the J26 PYRO_ARM plug must be fitted.
  The charger and the BMS switch between sun and battery on their own: in sun the
  charger feeds the bus and charges the battery; in eclipse the battery feeds the bus.
  INA226 #1 sees ONLY the load (the whole satellite). It cannot see solar or battery
  current, so this module works out the power state:
    - sun / eclipse  from the ADCS sun sensors (6 faces, page 1) and the payload TSL2591
                     (update_sun()). Lit if either says lit; eclipse only if fresh ADCS
                     data says dark (the TSL2591 looks one way, so its "dark" proves nothing).
    - charging       from the battery-voltage trend, corrected for the load current
                     (V + I_load * R_int removes load steps): sunlit + falling = the
                     panels give less than the load; at full_mv = full.
    - SOC            from the corrected voltage on a Li-ion rest-voltage curve. Rough,
                     and high while charging (flag GL_EF_SOC_FROM_VOLTAGE).

INA226 #2 (battery current), OPTIONAL, NOT FITTED YET
  Splice a second INA226 module into the battery cable, + wire, before EPS J4
  (pack + -> IN+, IN- -> J4 pin 1), A0 pad bridged to VCC (0x41), VCC/GND/SDA/SCL on
  the same i2c-3 bus, VBUS pin to IN-. No PCB change. It then gives the battery
  current (+ discharging, - charging), the charger's solar power and charge counting.
  To switch it on:
    1. ICD/cubesat_icd.h: delete the "// " before ICD_ADDR_INA226_BATT; run
       ICD/tools/gen_icd_py.py.
    2. This file: remove the "# BATT: " prefix from every line that has it:
           sed -i 's/^\\(\\s*\\)# BATT: /\\1/' EPS.py
    3. obc.toml [sensors]: batt_shunt_ohm, batt_max_current_a; [eps]: capacity_mah.

INA226 module (CJMCU-226 style: VCC GND SDA SCL ALE VBUS IN- IN+)
  - Most of these ship with an R100 (0.1 ohm) shunt: full scale 81.92 mV / 0.1 = 0.82 A.
    The satellite can draw more (Pi + camera + coils + radio, ~2 A at low battery), and
    the reading then clips (flag GL_EF_CLIPPED). Swap the shunt for an R010 (0.01 ohm,
    2512 size) or buy an R010 module, and set [sensors] shunt_ohm / max_current_a.
  - The module's VBUS pin is separate and J11 does not carry it: wire VBUS to IN- on
    the module, or the bus voltage reads ~0 (flag GL_EF_VBUS_IMPLAUSIBLE: never taken
    as a flat battery, because the Pi could not be running at that voltage).

Ground: GL_EPS_GET (8 bytes, unchanged) and GL_EPS_GET_STATE (24 bytes, ICD v0.4,
state_bytes()). Wired into OBC/obc_app.py: make_eps() at start-up, update_sun()
with the ADCS sun sensors (slow poll) and the payload light reading (every tick),
poll() every tick, state_bytes() for GET_STATE. Settings: [sensors], [eps] in obc.toml.

Bench check (standalone, from Software/): python3 OBC/EPS/EPS.py --bus 3 [--shunt 0.1 --max-current 0.8]
"""
from __future__ import annotations

import argparse
import copy
import logging
import os
import struct
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

# The ICD folder is the single source of truth: constants and the I2C master come
# from Software/ICD/python (the OBC package's __init__ does the same).
_ICD_PY = Path(__file__).resolve().parents[2] / "ICD" / "python"
if str(_ICD_PY) not in sys.path:
    sys.path.insert(0, str(_ICD_PY))
import icd_defs as I                    # noqa: E402
from icd_link import BusError           # noqa: E402  (one BusError class for every bus backend)

log = logging.getLogger("eps")

# [eps] table of obc.toml. Voltages are for the 2S Li-ion pack, measured on VBUS.
DEFAULTS = {
    "cells": 2,
    "low_batt_mv": 6600,        # 3.30 V/cell: OBC goes SAFE
    "recover_mv": 7000,         # 3.50 V/cell: back to NOMINAL
    "critical_mv": 6200,        # 3.10 V/cell: sync the SD card, BMS cut-off is close
    "full_mv": 8300,            # 4.15 V/cell: charger in constant-voltage = full
    "low_hold_polls": 3,        # low / critical must last this many polls (load spikes)
    "recover_hold_polls": 10,   # and recovery this many
    "implausible_mv": 4500,     # below this the Pi cannot be running: sensor/wiring fault
    "r_int_ohm": 0.15,          # pack + BMS resistance for the load correction (measure it)
    "trend_window_s": 300.0,    # voltage trend window
    "trend_min_s": 120.0,       # need at least this much history for a trend
    "slope_mv_per_min": 1.0,    # |trend| above this counts as rising / falling
    "soc_avg_s": 60.0,          # SOC uses the corrected voltage averaged over this
    "sun_adc_threshold": 2000,  # only for ADCS fw < ICD v0.6 (no page 4): raw count above this = sunlit
    "lux_threshold": 20000.0,   # TSL2591 lux above this (or saturated) = sunlit
    "sun_max_age_s": 30.0,      # sun data older than this is ignored
    "sun_hold_s": 15.0,         # a sun/eclipse change must last this long (tumbling)
    "energy_max_gap_s": 10.0,   # longest poll gap integrated into the energy counter
    # BATT: "capacity_mah": 2000.0,    # pack capacity, for charge counting
    # BATT: "full_current_ma": 50.0,   # |battery current| below this at full_mv = 100 %
    # BATT: "batt_deadband_ma": 20.0,  # |battery current| below this = neither charging nor discharging
}

# Li-ion rest voltage per cell -> SOC % (typical NMC/LCO curve; approximate).
OCV_TABLE = [(3.27, 0), (3.61, 5), (3.69, 10), (3.71, 15), (3.73, 20), (3.75, 25), (3.77, 30),
             (3.79, 35), (3.80, 40), (3.82, 45), (3.84, 50), (3.85, 55), (3.87, 60), (3.91, 65),
             (3.95, 70), (3.98, 75), (4.02, 80), (4.08, 85), (4.11, 90), (4.15, 95), (4.20, 100)]

STATE_NAMES = {I.GL_PS_UNKNOWN: "UNKNOWN", I.GL_PS_ECLIPSE: "ECLIPSE", I.GL_PS_SUN_CHARGING: "SUN_CHARGING",
               I.GL_PS_SUN_FULL: "SUN_FULL", I.GL_PS_SUN_DISCHARGING: "SUN_DISCHARGING"}


def merge(base: dict, over: Optional[dict]) -> dict:
    out = copy.deepcopy(base)
    for k, v in (over or {}).items():
        out[k] = v
    return out


def soc_from_cell_voltage(v_cell: float) -> float:
    t = OCV_TABLE
    if v_cell <= t[0][0]:
        return 0.0
    if v_cell >= t[-1][0]:
        return 100.0
    for (v0, s0), (v1, s1) in zip(t, t[1:]):
        if v_cell <= v1:
            return s0 + (s1 - s0) * (v_cell - v0) / (v1 - v0)
    return 100.0  # not reached


# =====================================================================
# INA226
# =====================================================================

class INA226:
    REG_CONFIG, REG_SHUNT, REG_BUS, REG_POWER, REG_CURRENT, REG_CAL = 0, 1, 2, 3, 4, 5
    REG_MFR, REG_DIE = 0xFE, 0xFF
    MFR_ID, DIE_ID = 0x5449, 0x2260
    # 64-sample average, 1.1 ms bus + 1.1 ms shunt, continuous: a new result every ~141 ms,
    # averaging out the coil PWM and radio bursts.
    CONFIG = 0x4000 | (0b011 << 9) | (0b100 << 6) | (0b100 << 3) | 0b111
    SHUNT_LSB_V = 2.5e-6
    SHUNT_FULL_SCALE_V = 32767 * 2.5e-6     # 81.92 mV
    CLIP_RAW = 32700                        # |shunt register| at or above this = clipped

    def __init__(self, bus: object, addr: int = I.ICD_ADDR_INA226_EPS, shunt_ohm: float = 0.1,
                 max_current_a: float = 0.8):
        if shunt_ohm <= 0 or max_current_a <= 0:
            raise ValueError("shunt_ohm and max_current_a must be > 0")
        self.bus, self.addr, self.shunt = bus, addr, shunt_ohm
        self.cal = round(0.00512 / ((max_current_a / 32768.0) * shunt_ohm))
        if not 1 <= self.cal <= 0x7FFF:     # CAL is 15 bits (bit 15 reserved)
            raise ValueError(f"INA226 calibration {self.cal} out of range: check shunt_ohm/max_current_a")
        self.current_lsb = 0.00512 / (self.cal * shunt_ohm)   # exact for the rounded CAL
        self.full_scale_a = self.SHUNT_FULL_SCALE_V / shunt_ohm
        if max_current_a > self.full_scale_a * 1.001:
            log.warning("INA226 0x%02X: max_current_a %.2f A is above what a %.3f ohm shunt can measure "
                        "(%.2f A): readings clip there", addr, max_current_a, shunt_ohm, self.full_scale_a)

    def _wr(self, reg: int, val: int) -> None:
        self.bus.write(self.addr, bytes([reg, (val >> 8) & 0xFF, val & 0xFF]))

    def _rd(self, reg: int) -> int:
        b = self.bus.write_read(self.addr, bytes([reg]), 2)
        return (b[0] << 8) | b[1]           # INA226 registers are big-endian

    @staticmethod
    def _s16(v: int) -> int:
        return v - 0x10000 if v & 0x8000 else v

    def init(self) -> bool:
        if self._rd(self.REG_MFR) != self.MFR_ID or self._rd(self.REG_DIE) != self.DIE_ID:
            return False
        self._wr(self.REG_CONFIG, self.CONFIG)
        self._wr(self.REG_CAL, self.cal)
        return self._rd(self.REG_CAL) == self.cal     # read back: catches a stuck bus / wrong chip

    def read(self) -> dict:
        # Current comes from the shunt-voltage register (I = V / R), not from the chip's
        # current register: that one is shunt * CAL / 2048 and overflows (wraps negative)
        # when the shunt is near full scale. Power is computed here for the same reason.
        shunt_raw = self._s16(self._rd(self.REG_SHUNT))
        bus_raw = self._rd(self.REG_BUS)
        vbus_mv = bus_raw * 1.25
        cur_ma = shunt_raw * self.SHUNT_LSB_V / self.shunt * 1000.0
        return {
            "vbus_mV": round(vbus_mv, 2),
            "current_mA": round(cur_ma, 3),
            "power_mW": round(vbus_mv * abs(cur_ma) / 1000.0, 1),
            "shunt_mV": round(shunt_raw * self.SHUNT_LSB_V * 1000.0, 4),
            "clipped": abs(shunt_raw) >= self.CLIP_RAW,
        }


class FakeINA226:
    """Simulated INA226 registers (for --sim and tests). Uses the CAL the driver writes,
    clips at the shunt's full scale like the real chip."""

    def __init__(self, vbus_mv: float = 7600.0, current_ma: float = 450.0, shunt_ohm: float = 0.01,
                 max_current_a: float = 3.2):
        self.vbus_mv, self.current_ma, self.shunt_ohm = vbus_mv, current_ma, shunt_ohm
        self.present = True
        self.ptr = 0
        self.regs = {0xFE: 0x5449, 0xFF: 0x2260, 0: 0x4127, 5: 0}

    def write(self, data: bytes) -> None:
        if not self.present:
            raise BusError("INA226 NACK")
        self.ptr = data[0]
        if len(data) >= 3:
            self.regs[self.ptr] = (data[1] << 8) | data[2]

    def _shunt_raw(self) -> int:
        raw = round(self.current_ma / 1000.0 * self.shunt_ohm / 2.5e-6)
        return max(-32768, min(32767, raw))

    def read(self, n: int) -> bytes:
        if not self.present:
            raise BusError("INA226 NACK")
        r, cal = self.ptr, self.regs.get(5, 0)
        if r == 1:
            v = self._shunt_raw() & 0xFFFF
        elif r == 2:
            v = max(0, min(0x7FFF, round(self.vbus_mv / 1.25)))
        elif r == 4:                         # datasheet: current = shunt * CAL / 2048
            v = (int(self._shunt_raw() * cal / 2048)) & 0xFFFF
        elif r == 3:                         # power = |current| * bus / 20000
            cur = abs(int(self._shunt_raw() * cal / 2048))
            v = min(0xFFFF, int(cur * round(self.vbus_mv / 1.25) / 20000))
        else:
            v = self.regs.get(r, 0)
        return bytes([(v >> 8) & 0xFF, v & 0xFF])[:n]


# =====================================================================
# Pyro (unchanged from the base repo)
# =====================================================================

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


# =====================================================================
# EPS monitor
# =====================================================================

class Eps:
    def __init__(self, ina: Optional[INA226], low_mv: Optional[float] = None, recover_mv: Optional[float] = None,
                 cfg: Optional[dict] = None, clock: Callable[[], float] = time.monotonic,
                 sync_fn: Optional[Callable[[], None]] = None):
        c = merge(DEFAULTS, cfg)
        if low_mv is not None:
            c["low_batt_mv"] = low_mv
        if recover_mv is not None:
            c["recover_mv"] = recover_mv
        if not c["critical_mv"] < c["low_batt_mv"] < c["recover_mv"] < c["full_mv"]:
            raise ValueError("EPS thresholds must satisfy critical < low < recover < full")
        self.cfg, self.ina, self.clock = c, ina, clock
        self.sync_fn = sync_fn if sync_fn is not None else getattr(os, "sync", lambda: None)
        self.low_mv, self.recover_mv = c["low_batt_mv"], c["recover_mv"]

        self.ina_ok = False
        self.implausible = False
        self.clipped = False
        self.last: dict = {}
        self.low_battery = False
        self.critical = False
        self._low_n = self._rec_n = self._crit_n = self._crit_rec_n = 0

        self.batt_ina = None                # INA226 #2 (battery current), see attach_battery_ina()
        self.batt_ok = False
        self.ibatt_mA: Optional[float] = None
        # BATT: self.soc_mah: Optional[float] = None

        self._trend: deque = deque()        # (t, corrected mV)
        self._adcs_sun = None               # (t, lit)
        self._tsl_sun = None
        self.sunlit: Optional[bool] = None  # debounced decision
        self._sun_cand: Optional[bool] = None
        self._sun_cand_since = 0.0
        self.state = I.GL_PS_UNKNOWN
        self.state_since = clock()
        self.soc_pct: Optional[float] = None
        self.soc_from_voltage = True

        self.e_load_mwh = 0.0
        self._last_t: Optional[float] = None
        self._reset_stats()

        if ina is not None:
            try:
                self.ina_ok = ina.init()
            except BusError as e:
                log.error("INA226 init failed: %s", e)
            if not self.ina_ok:
                log.error("INA226 not found at 0x%02X", ina.addr)

    # ---------------------------------------------------------- compatibility
    @property
    def ok(self) -> bool:
        """INA226 #1 answering with a plausible bus voltage (the OBC's EPS fault flag)."""
        return self.ina_ok and not self.implausible

    # BATT: def attach_battery_ina(self, ina: INA226) -> None:
    # BATT:     self.batt_ina = ina
    # BATT:     try:
    # BATT:         self.batt_ok = ina.init()
    # BATT:     except BusError as e:
    # BATT:         log.error("battery INA226 init failed: %s", e)
    # BATT:     if not self.batt_ok:
    # BATT:         log.error("battery INA226 not found at 0x%02X", ina.addr)

    # ---------------------------------------------------------- inputs
    def update_sun(self, sun_raw=None, lux=None, adcs_lit=None, adcs_hidden=False) -> None:
        """adcs_lit: ADCS's own sunlit decision from its BPW34 sun sensors (page 4,
        ICD v0.6; it knows which faces have a diode and its lit threshold).
        sun_raw: the six raw sun-sensor values (page 1), used only when the ADCS
        firmware is older and has no page 4. lux: payload TSL2591 last_lux (None =
        no reading, inf = saturated), a second opinion. adcs_hidden (v0.7): ADCS says
        the Sun is only behind a dead or missing diode, so it does not know: its vote
        is dropped (never counted as eclipse). Call whenever one is read."""
        now = self.clock()
        if adcs_hidden:
            self._adcs_sun = None
        elif adcs_lit is not None:
            self._adcs_sun = (now, bool(adcs_lit))
        elif sun_raw:
            self._adcs_sun = (now, max(sun_raw) >= self.cfg["sun_adc_threshold"])
        if lux is not None:
            self._tsl_sun = (now, lux == float("inf") or lux >= self.cfg["lux_threshold"])

    def _sun_vote(self, now: float):
        age = self.cfg["sun_max_age_s"]
        adcs = self._adcs_sun if self._adcs_sun and now - self._adcs_sun[0] <= age else None
        tsl = self._tsl_sun if self._tsl_sun and now - self._tsl_sun[0] <= age else None
        if (adcs and adcs[1]) or (tsl and tsl[1]):
            return True, adcs is not None, tsl is not None
        if adcs is not None:                 # fresh ADCS says dark, TSL not lit
            return False, True, tsl is not None
        return None, False, tsl is not None  # TSL alone cannot prove eclipse

    # ---------------------------------------------------------- poll (once per OBC tick)
    def poll(self) -> dict:
        if self.ina is None:
            return {}
        now = self.clock()
        try:
            if not self.ina_ok:
                self.ina_ok = self.ina.init()
            self.last = self.ina.read() if self.ina_ok else {}
        except BusError as e:
            self.ina_ok = False
            self.last = {}
            log.error("INA226 read failed: %s", e)

        v = self.last.get("vbus_mV")
        i = self.last.get("current_mA")
        clipped = bool(self.last.get("clipped"))
        if clipped != self.clipped:
            self.clipped = clipped
            if clipped:
                log.warning("INA226 shunt at full scale (%.2f A): current under-read, check the shunt",
                            self.ina.full_scale_a)
        implausible = v is not None and v < self.cfg["implausible_mv"]
        if implausible != self.implausible:
            self.implausible = implausible
            if implausible:
                log.error("INA226 bus voltage %.0f mV is impossible while the Pi runs: "
                          "is the module's VBUS pin wired to IN-? Ignoring it for battery decisions", v)
        if implausible:
            v = None

        self.ibatt_mA = None
        # BATT: self.ibatt_mA = self._read_battery()

        self._battery_levels(v)
        dt = None
        if self._last_t is not None:
            dt = min(now - self._last_t, self.cfg["energy_max_gap_s"])
        self._last_t = now
        if v is not None and i is not None:
            self.vmin = v if self.vmin is None else min(self.vmin, v)
            self.vmax = v if self.vmax is None else max(self.vmax, v)
            self.imax = i if self.imax is None else max(self.imax, i)
            if dt is not None and dt > 0:
                self.e_load_mwh += max(0.0, self.last.get("power_mW", 0.0)) * dt / 3600.0
            self._trend.append((now, v + i * self.cfg["r_int_ohm"]))
        while self._trend and now - self._trend[0][0] > self.cfg["trend_window_s"]:
            self._trend.popleft()

        # BATT: if self.ibatt_mA is not None and dt:
        # BATT:     self._count_charge(self.ibatt_mA, dt, v)

        self._update_sun(now)
        self._update_state(now, v)
        self._update_soc(now, v)

        out = dict(self.last)
        out.update(self.summary(now))
        return out

    # BATT: def _read_battery(self) -> Optional[float]:
    # BATT:     if self.batt_ina is None:
    # BATT:         return None
    # BATT:     try:
    # BATT:         if not self.batt_ok:
    # BATT:             self.batt_ok = self.batt_ina.init()
    # BATT:         if not self.batt_ok:
    # BATT:             return None
    # BATT:         r = self.batt_ina.read()
    # BATT:     except BusError as e:
    # BATT:         self.batt_ok = False
    # BATT:         log.error("battery INA226 read failed: %s", e)
    # BATT:         return None
    # BATT:     if r["clipped"]:
    # BATT:         self.clipped = True
    # BATT:     self.last["ibatt_mA"] = r["current_mA"]
    # BATT:     return r["current_mA"]
    # BATT:
    # BATT: def _count_charge(self, ibatt: float, dt: float, v) -> None:
    # BATT:     cap = self.cfg["capacity_mah"]
    # BATT:     if self.soc_mah is None:
    # BATT:         if v is None:
    # BATT:             return
    # BATT:         self.soc_mah = cap * soc_from_cell_voltage((v + ibatt * self.cfg["r_int_ohm"]) / 1000.0
    # BATT:                                                    / self.cfg["cells"]) / 100.0
    # BATT:     self.soc_mah = max(0.0, min(cap, self.soc_mah - ibatt * dt / 3600.0))
    # BATT:     if v is not None and v >= self.cfg["full_mv"] and abs(ibatt) < self.cfg["full_current_ma"]:
    # BATT:         self.soc_mah = cap              # charger in constant voltage, current tapered off: full

    def _battery_levels(self, v) -> None:
        if v is None:
            return
        c = self.cfg
        if not self.low_battery:
            self._low_n = self._low_n + 1 if v < self.low_mv else 0
            if self._low_n >= c["low_hold_polls"]:
                self.low_battery, self._rec_n = True, 0
                log.warning("LOW BATTERY %.0f mV", v)
        else:
            self._rec_n = self._rec_n + 1 if v > self.recover_mv else 0
            if self._rec_n >= c["recover_hold_polls"]:
                self.low_battery, self._low_n = False, 0
                log.warning("battery recovered %.0f mV", v)
        if not self.critical:
            self._crit_n = self._crit_n + 1 if v < c["critical_mv"] else 0
            if self._crit_n >= c["low_hold_polls"]:
                self.critical, self._crit_rec_n = True, 0
                log.error("CRITICAL BATTERY %.0f mV: syncing the SD card", v)
                try:
                    self.sync_fn()
                except Exception as e:  # noqa: BLE001
                    log.error("sync failed: %r", e)
        else:
            self._crit_rec_n = self._crit_rec_n + 1 if v > self.low_mv else 0
            if self._crit_rec_n >= c["recover_hold_polls"]:
                self.critical, self._crit_n = False, 0
                log.warning("battery out of the critical range %.0f mV", v)

    def _update_sun(self, now: float) -> None:
        raw, _, _ = self._sun_vote(now)
        if raw is None:                      # no fresh data: unknown at once
            if self.sunlit is not None:
                self._set_sunlit(None)
            self._sun_cand = None
            return
        if raw != self._sun_cand:
            self._sun_cand, self._sun_cand_since = raw, now
        if self.sunlit is None or (raw != self.sunlit and now - self._sun_cand_since >= self.cfg["sun_hold_s"]):
            self._set_sunlit(raw)

    def _set_sunlit(self, lit: Optional[bool]) -> None:
        self.sunlit = lit
        self._trend.clear()                  # do not mix trends across sun / eclipse

    def _slope_mv_per_min(self) -> Optional[float]:
        pts = list(self._trend)
        if len(pts) < 3 or pts[-1][0] - pts[0][0] < self.cfg["trend_min_s"]:
            return None
        n = len(pts)
        mt = sum(t for t, _ in pts) / n
        mv = sum(x for _, x in pts) / n
        den = sum((t - mt) ** 2 for t, _ in pts)
        if den <= 0:
            return None
        return 60.0 * sum((t - mt) * (x - mv) for t, x in pts) / den

    def _update_state(self, now: float, v) -> None:
        lit = self.sunlit
        if lit is None:
            st = I.GL_PS_UNKNOWN
        elif not lit:
            st = I.GL_PS_ECLIPSE
        elif v is not None and v >= self.cfg["full_mv"]:
            st = I.GL_PS_SUN_FULL
        else:
            slope = self._slope_mv_per_min()
            st = I.GL_PS_SUN_DISCHARGING if slope is not None and slope <= -self.cfg["slope_mv_per_min"] \
                else I.GL_PS_SUN_CHARGING
        # BATT: ib, db = self.ibatt_mA, self.cfg["batt_deadband_ma"]
        # BATT: if ib is not None:            # measured battery current overrides the voltage trend
        # BATT:     if v is not None and v >= self.cfg["full_mv"] and ib < db:
        # BATT:         st = I.GL_PS_SUN_FULL
        # BATT:     elif ib < -db:              # charging: the charger must be getting sun
        # BATT:         st = I.GL_PS_SUN_CHARGING
        # BATT:     elif lit:
        # BATT:         st = I.GL_PS_SUN_DISCHARGING if ib > db else I.GL_PS_SUN_CHARGING
        if st != self.state:
            log.info("power state %s -> %s", STATE_NAMES[self.state], STATE_NAMES[st])
            self.state, self.state_since = st, now

    def _update_soc(self, now: float, v) -> None:
        self.soc_from_voltage = True
        # BATT: if self.soc_mah is not None:
        # BATT:     self.soc_pct = 100.0 * self.soc_mah / self.cfg["capacity_mah"]
        # BATT:     self.soc_from_voltage = False
        # BATT:     return
        recent = [x for t, x in self._trend if now - t <= self.cfg["soc_avg_s"]]
        if not recent:
            if v is None:
                self.soc_pct = None
            return
        self.soc_pct = soc_from_cell_voltage(sum(recent) / len(recent) / 1000.0 / self.cfg["cells"])

    # ---------------------------------------------------------- outputs
    def _reset_stats(self) -> None:
        self.vmin = self.vmax = self.imax = None

    def flags(self) -> int:
        f = 0
        if self.ok:
            f |= I.GL_EF_INA_LOAD_OK
        if self.batt_ina is not None:
            f |= I.GL_EF_INA_BATT_FITTED
            if self.batt_ok:
                f |= I.GL_EF_INA_BATT_OK
        if self.clipped:
            f |= I.GL_EF_CLIPPED
        if self.implausible:
            f |= I.GL_EF_VBUS_IMPLAUSIBLE
        if self.low_battery:
            f |= I.GL_EF_LOW_BATT
        if self.critical:
            f |= I.GL_EF_CRITICAL_BATT
        if self.soc_from_voltage:
            f |= I.GL_EF_SOC_FROM_VOLTAGE
        return f

    def sun_flags(self, now: Optional[float] = None) -> int:
        now = self.clock() if now is None else now
        _, adcs_fresh, tsl_fresh = self._sun_vote(now)
        f = I.GL_SUNF_ADCS_FRESH if adcs_fresh else 0
        if tsl_fresh:
            f |= I.GL_SUNF_TSL_FRESH
        if self.sunlit:
            f |= I.GL_SUNF_LIT
        return f

    def psolar_mw(self) -> Optional[float]:
        """Power the charger puts on the bus = load + charging. Needs INA226 #2."""
        v, i = self.last.get("vbus_mV"), self.last.get("current_mA")
        if self.ibatt_mA is None or v is None or i is None or self.implausible:
            return None
        return max(0.0, v * (i - self.ibatt_mA) / 1000.0)

    def summary(self, now: Optional[float] = None) -> dict:
        now = self.clock() if now is None else now
        ps = self.psolar_mw()
        return {"power_state": STATE_NAMES[self.state], "state_age_s": round(now - self.state_since, 1),
                "sunlit": self.sunlit, "soc_pct": None if self.soc_pct is None else round(self.soc_pct, 1),
                "soc_from_voltage": self.soc_from_voltage, "ibatt_mA": self.ibatt_mA,
                "psolar_mW": None if ps is None else round(ps, 1),
                "low_battery": self.low_battery, "critical": self.critical, "clipped": self.clipped,
                "vbus_implausible": self.implausible, "e_load_mWh": round(self.e_load_mwh, 2),
                "trend_mV_min": (lambda s: None if s is None else round(s, 3))(self._slope_mv_per_min())}

    def state_bytes(self, reset_stats: bool = True) -> bytes:
        """GL_EPS_GET_STATE response (ICD v0.4, GL_EPSS_* layout, 24 bytes)."""
        now = self.clock()

        def u16(x):
            return I.GL_EPSS_U16_NA if x is None else int(max(0, min(0xFFFE, round(x))))

        def i16(x):
            return I.GL_EPSS_I16_NA if x is None else int(max(-0x8000, min(0x7FFE, round(x))))

        v = None if self.implausible else self.last.get("vbus_mV")
        b = bytearray(I.GL_EPSS_LEN)
        struct.pack_into("<H", b, I.GL_EPSS_OFS_VBATT, u16(v))
        struct.pack_into("<h", b, I.GL_EPSS_OFS_ILOAD, i16(self.last.get("current_mA")))
        struct.pack_into("<h", b, I.GL_EPSS_OFS_IBATT, i16(self.ibatt_mA))
        struct.pack_into("<H", b, I.GL_EPSS_OFS_PSOLAR, u16(self.psolar_mw()))
        b[I.GL_EPSS_OFS_SOC] = I.GL_EPSS_U8_NA if self.soc_pct is None else int(max(0, min(100, round(self.soc_pct))))
        b[I.GL_EPSS_OFS_STATE] = self.state
        b[I.GL_EPSS_OFS_FLAGS] = self.flags()
        b[I.GL_EPSS_OFS_SUN] = self.sun_flags(now)
        struct.pack_into("<H", b, I.GL_EPSS_OFS_VMIN, u16(self.vmin))
        struct.pack_into("<H", b, I.GL_EPSS_OFS_VMAX, u16(self.vmax))
        struct.pack_into("<H", b, I.GL_EPSS_OFS_IMAX, u16(self.imax))
        struct.pack_into("<I", b, I.GL_EPSS_OFS_E_LOAD, int(max(0, min(0xFFFFFFFF, self.e_load_mwh))))
        struct.pack_into("<H", b, I.GL_EPSS_OFS_STATE_AGE, int(max(0, min(0xFFFF, now - self.state_since))))
        if reset_stats:
            self._reset_stats()
        return bytes(b)


def decode_state(b: bytes) -> dict:
    """Ground side: GL_EPS_GET_STATE bytes -> dict (for gs_tool.py)."""
    if len(b) != I.GL_EPSS_LEN:
        raise ValueError(f"EPS state is {I.GL_EPSS_LEN} bytes, got {len(b)}")

    def u16(o):
        x = struct.unpack_from("<H", b, o)[0]
        return None if x == I.GL_EPSS_U16_NA else x

    def i16(o):
        x = struct.unpack_from("<h", b, o)[0]
        return None if x == I.GL_EPSS_I16_NA else x

    f, s = b[I.GL_EPSS_OFS_FLAGS], b[I.GL_EPSS_OFS_SUN]
    return {
        "vbatt_mV": u16(I.GL_EPSS_OFS_VBATT), "iload_mA": i16(I.GL_EPSS_OFS_ILOAD),
        "ibatt_mA": i16(I.GL_EPSS_OFS_IBATT), "psolar_mW": u16(I.GL_EPSS_OFS_PSOLAR),
        "soc_pct": None if b[I.GL_EPSS_OFS_SOC] == I.GL_EPSS_U8_NA else b[I.GL_EPSS_OFS_SOC],
        "state": STATE_NAMES.get(b[I.GL_EPSS_OFS_STATE], f"0x{b[I.GL_EPSS_OFS_STATE]:02X}"),
        "flags": {n[6:].lower(): bool(f & getattr(I, n)) for n in dir(I) if n.startswith("GL_EF_")},
        "sun": {n[8:].lower(): bool(s & getattr(I, n)) for n in dir(I) if n.startswith("GL_SUNF_")},
        "vmin_mV": u16(I.GL_EPSS_OFS_VMIN), "vmax_mV": u16(I.GL_EPSS_OFS_VMAX),
        "imax_mA": u16(I.GL_EPSS_OFS_IMAX),
        "e_load_mWh": struct.unpack_from("<I", b, I.GL_EPSS_OFS_E_LOAD)[0],
        "state_age_s": struct.unpack_from("<H", b, I.GL_EPSS_OFS_STATE_AGE)[0],
    }


def make_eps(cfg: dict, sensor_bus, clock: Callable[[], float] = time.monotonic) -> Eps:
    """Build the EPS monitor from the OBC config ([sensors] and [eps] tables)."""
    S, E = cfg.get("sensors", {}), cfg.get("eps", {})
    ina = None
    if sensor_bus is not None and S.get("ina226", True):
        ina = INA226(sensor_bus, S.get("ina226_addr", I.ICD_ADDR_INA226_EPS),
                     S.get("shunt_ohm", 0.1), S.get("max_current_a", 0.8))
    eps = Eps(ina, cfg=E, clock=clock)
    # BATT: addr = getattr(I, "ICD_ADDR_INA226_BATT", None)
    # BATT: if addr is None:
    # BATT:     log.error("ICD_ADDR_INA226_BATT is commented out in cubesat_icd.h: uncomment it and "
    # BATT:               "re-run gen_icd_py.py. Battery INA226 disabled.")
    # BATT: elif sensor_bus is not None:
    # BATT:     eps.attach_battery_ina(INA226(sensor_bus, addr, S.get("batt_shunt_ohm", 0.1),
    # BATT:                                   S.get("batt_max_current_a", 0.8)))
    return eps


# =====================================================================
# Standalone bench check
# =====================================================================

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="EPS bench check: print INA226 readings every second")
    ap.add_argument("--bus", type=int, default=3, help="Pi sensor bus (ICD: i2c-3)")
    ap.add_argument("--shunt", type=float, default=0.1, help="shunt resistor in ohm (R100 = 0.1, R010 = 0.01)")
    ap.add_argument("--max-current", type=float, default=0.8, help="largest expected current in A")
    ap.add_argument("--count", type=int, default=0, help="number of readings (0 = until Ctrl-C)")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="[eps] %(levelname)s %(message)s")
    try:
        from ..Payload.Payload import SMBus2Bus     # noqa: PLC0415  (as part of the OBC package)
    except ImportError:                             # run as a script: python3 OBC/EPS/EPS.py
        _payload_dir = str(Path(__file__).resolve().parents[1] / "Payload")
        if _payload_dir not in sys.path:
            sys.path.insert(0, _payload_dir)
        from Payload import SMBus2Bus               # noqa: PLC0415
    bus = SMBus2Bus(a.bus)
    cfg = {"sensors": {"shunt_ohm": a.shunt, "max_current_a": a.max_current}}
    eps = make_eps(cfg, bus)
    if not eps.ina_ok:
        log.error("no INA226 at 0x%02X on i2c-%d (i2cdetect -y %d)", I.ICD_ADDR_INA226_EPS, a.bus, a.bus)
    n = 0
    try:
        while a.count == 0 or n < a.count:
            r = eps.poll()
            print({k: r.get(k) for k in ("vbus_mV", "current_mA", "power_mW", "shunt_mV", "clipped",
                                         "vbus_implausible", "soc_pct", "power_state")}, flush=True)
            n += 1
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        bus.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
