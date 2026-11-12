"""EPS.py: INA226 driver, battery levels, sun/eclipse, power state, SOC, energy,
GET_STATE bytes, and the commented-out INA226 #2 code switched on (on a copy)."""
import importlib.util
import logging
import re
import shutil
import subprocess
import sys
import types
from pathlib import Path

import pytest

from OBC import EPS as E

SRC = Path(E.__file__)                     # Software/OBC/EPS.py
SW = SRC.parent.parent                     # Software/


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    sys.modules[name] = m
    spec.loader.exec_module(m)
    return m


I = E.I


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


class Bus:
    def __init__(self, devs):
        self.devs = devs

    def write(self, addr, data):
        if addr not in self.devs:
            raise E.BusError("NACK")
        self.devs[addr].write(bytes(data))

    def write_read(self, addr, data, n):
        if addr not in self.devs:
            raise E.BusError("NACK")
        self.devs[addr].write(bytes(data))
        return self.devs[addr].read(n)


def rig(mod=E, vbus=7600, cur=400, shunt=0.1, maxa=0.8, cfg=None, extra=None):
    clk = Clock()
    fake = mod.FakeINA226(vbus, cur, shunt_ohm=shunt)
    devs = {0x40: fake}
    devs.update(extra or {})
    bus = Bus(devs)
    c = {"sensors": {"shunt_ohm": shunt, "max_current_a": maxa}, "eps": cfg or {}}
    syncs = []
    eps = mod.make_eps(c, bus, clock=clk)
    eps.sync_fn = lambda: syncs.append(clk.t)
    return eps, fake, clk, syncs


def step(eps, clk, n=1, dt=1.0):
    r = None
    for _ in range(n):
        r = eps.poll()
        clk.t += dt
    return r


# ---------------------------------------------------------------- INA226 driver
@pytest.mark.parametrize("shunt,maxa,cur", [(0.1, 0.8, 400), (0.1, 0.8, -250), (0.01, 3.2, 2100), (0.01, 8.0, -3000)])
def test_ina_scaling(shunt, maxa, cur):
    eps, fake, clk, _ = rig(vbus=7412.5, cur=cur, shunt=shunt, maxa=maxa)
    r = eps.poll()
    assert eps.ina_ok and eps.ok
    assert r["vbus_mV"] == pytest.approx(7412.5, abs=1.25)
    assert r["current_mA"] == pytest.approx(cur, abs=max(0.5, abs(cur) * 0.002))
    assert r["shunt_mV"] == pytest.approx(cur * shunt, abs=0.003)
    assert r["power_mW"] == pytest.approx(abs(cur) * 7412.5 / 1000, rel=0.01)
    assert not r["clipped"]
    assert fake.regs[0] == E.INA226.CONFIG == 0x4727


def test_ina_clipping_r100():
    eps, fake, clk, _ = rig(cur=1500, shunt=0.1, maxa=0.8)
    r = eps.poll()
    assert r["clipped"] and eps.clipped
    assert r["current_mA"] == pytest.approx(819.2, abs=1)        # stuck at full scale, not 1500
    assert eps.flags() & I.GL_EF_CLIPPED
    fake.current_ma = 500
    assert not eps.poll()["clipped"] and not eps.flags() & I.GL_EF_CLIPPED


def test_ina_cal_limits_and_warning(caplog):
    with pytest.raises(ValueError):
        E.INA226(Bus({}), 0x40, 0.0001, 0.001)                     # CAL > 15 bits
    with pytest.raises(ValueError):
        E.INA226(Bus({}), 0x40, 0, 1)
    caplog.set_level(logging.WARNING)
    E.INA226(Bus({}), 0x40, 0.1, 3.2)
    assert "clip" in caplog.text


def test_ina_wrong_chip_and_missing():
    fake = E.FakeINA226()
    fake.regs[0xFF] = 0x1234                                       # not an INA226 die id
    eps = E.Eps(E.INA226(Bus({0x40: fake}), 0x40, 0.01, 3.2))
    assert not eps.ok
    eps2 = E.Eps(E.INA226(Bus({}), 0x40, 0.01, 3.2))
    assert eps2.poll() == {} or not eps2.ok
    assert not eps2.ok and not eps2.flags() & I.GL_EF_INA_LOAD_OK


def test_compat_positional_and_no_ina():
    fake = E.FakeINA226(7600, 450)                                  # base repo's call
    eps = E.Eps(E.INA226(Bus({0x40: fake}), 0x40, 0.01, 3.2), 6600, 7000)
    r = eps.poll()
    assert eps.ok and 7000 < r["vbus_mV"] < 8000 and not eps.low_battery
    assert E.Eps(None, 6600, 7000).poll() == {}
    with pytest.raises(ValueError):
        E.Eps(None, 7000, 6600)                                     # low above recover


def test_unplug_and_recover():
    eps, fake, clk, _ = rig()
    assert eps.ok
    fake.present = False
    assert step(eps, clk).get("vbus_mV") is None and not eps.ok
    fake.present = True
    assert step(eps, clk)["vbus_mV"] > 7000 and eps.ok


# ---------------------------------------------------------------- battery thresholds
def test_low_battery_hold_hysteresis_and_spikes():
    eps, fake, clk, _ = rig(cfg={"low_hold_polls": 3, "recover_hold_polls": 10})
    step(eps, clk, 2)
    fake.vbus_mv = 6400
    step(eps, clk, 2)
    assert not eps.low_battery                                      # 2 polls: a load spike
    fake.vbus_mv = 7600
    step(eps, clk, 1)
    fake.vbus_mv = 6400
    step(eps, clk, 2)
    assert not eps.low_battery                                      # counter restarted
    step(eps, clk, 1)
    assert eps.low_battery and eps.flags() & I.GL_EF_LOW_BATT
    fake.vbus_mv = 6800                                             # hysteresis band
    step(eps, clk, 30)
    assert eps.low_battery
    fake.vbus_mv = 7200
    step(eps, clk, 9)
    assert eps.low_battery
    step(eps, clk, 1)
    assert not eps.low_battery


def test_critical_syncs_once_and_clears():
    eps, fake, clk, syncs = rig()
    fake.vbus_mv = 6000
    step(eps, clk, 10)
    assert eps.critical and eps.low_battery and len(syncs) == 1
    assert eps.flags() & I.GL_EF_CRITICAL_BATT
    fake.vbus_mv = 6700                                             # above low: critical clears after hold
    step(eps, clk, 10)
    assert not eps.critical and eps.low_battery


def test_implausible_vbus_is_a_fault_not_a_flat_battery(caplog):
    eps, fake, clk, syncs = rig(vbus=0)                             # VBUS pin not wired
    step(eps, clk, 20)
    assert eps.implausible and not eps.ok and not eps.low_battery and not eps.critical and not syncs
    assert "VBUS pin" in caplog.text
    d = E.decode_state(eps.state_bytes())
    assert d["vbatt_mV"] is None and d["flags"]["vbus_implausible"] and not d["flags"]["ina_load_ok"]
    fake.vbus_mv = 7600
    step(eps, clk)
    assert eps.ok and not eps.implausible


# ---------------------------------------------------------------- sun / state
def test_sun_sources_and_debounce():
    eps, fake, clk, _ = rig(cfg={"sun_hold_s": 15, "sun_max_age_s": 30})
    step(eps, clk)
    assert eps.state == I.GL_PS_UNKNOWN and eps.sunlit is None
    eps.update_sun(lux=0.0)                                         # TSL dark alone proves nothing
    step(eps, clk)
    assert eps.sunlit is None and eps.sun_flags() & I.GL_SUNF_TSL_FRESH
    eps.update_sun(lux=float("inf"))                                # saturated TSL = sun
    step(eps, clk)
    assert eps.sunlit is True and eps.state == I.GL_PS_SUN_CHARGING
    eps.update_sun(sun_raw=[100, 50, 3000, 0, 0, 0])
    eps.update_sun(lux=10.0)
    step(eps, clk)
    assert eps.sunlit is True
    for _ in range(10):                                             # short dark flicker < hold
        eps.update_sun(sun_raw=[100] * 6)
        step(eps, clk)
    eps.update_sun(sun_raw=[3000] + [0] * 5)
    step(eps, clk)
    assert eps.sunlit is True
    for _ in range(16):                                             # dark for > hold
        eps.update_sun(sun_raw=[10] * 6)
        step(eps, clk)
    assert eps.sunlit is False and eps.state == I.GL_PS_ECLIPSE
    clk.t += 60                                                     # everything stale
    step(eps, clk)
    assert eps.sunlit is None and eps.state == I.GL_PS_UNKNOWN and eps.sun_flags() == 0


def test_lux_none_ignored_and_threshold():
    eps, fake, clk, _ = rig(cfg={"lux_threshold": 20000})
    eps.update_sun(lux=None)
    step(eps, clk)
    assert eps.sun_flags() == 0
    eps.update_sun(lux=19999.0)
    step(eps, clk)
    assert eps.sunlit is None
    eps.update_sun(lux=20000.0)
    step(eps, clk)
    assert eps.sunlit is True


def lit(eps):
    eps.update_sun(sun_raw=[3500, 0, 0, 0, 0, 0])


def test_full_charging_and_sunlit_discharging():
    eps, fake, clk, _ = rig(cfg={"trend_min_s": 120, "slope_mv_per_min": 1.0, "full_mv": 8300})
    fake.vbus_mv = 7600
    for k in range(200):                                            # rising 3 mV/min
        lit(eps)
        fake.vbus_mv = 7600 + 3 * k / 60
        step(eps, clk)
    assert eps.state == I.GL_PS_SUN_CHARGING and eps.summary()["trend_mV_min"] == pytest.approx(3, abs=0.3)
    for k in range(200):                                            # falling 4 mV/min in sun
        lit(eps)
        fake.vbus_mv = 7610 - 4 * k / 60
        step(eps, clk)
    assert eps.state == I.GL_PS_SUN_DISCHARGING
    lit(eps)
    fake.vbus_mv = 8350
    step(eps, clk)
    assert eps.state == I.GL_PS_SUN_FULL
    assert eps.summary()["state_age_s"] == pytest.approx(1.0)


def test_load_steps_do_not_fake_discharging():
    # Battery rest voltage constant at 7600 mV; the load jumps 300 <-> 1200 mA every 20 s.
    r = 0.15
    eps, fake, clk, _ = rig(shunt=0.01, maxa=3.2, cfg={"r_int_ohm": r, "trend_min_s": 120})
    for k in range(300):
        lit(eps)
        fake.current_ma = 1200 if (k // 20) % 2 else 300
        fake.vbus_mv = 7600 - fake.current_ma * r                   # what the bus really shows
        step(eps, clk)
    assert eps.state == I.GL_PS_SUN_CHARGING
    assert abs(eps.summary()["trend_mV_min"]) < 0.5


def test_soc_from_voltage_with_load_correction():
    eps, fake, clk, _ = rig(shunt=0.01, maxa=3.2, cfg={"r_int_ohm": 0.2, "cells": 2})
    fake.current_ma = 1000
    fake.vbus_mv = 7600 - 1000 * 0.2                                # rest 7600 = 3.80 V/cell
    step(eps, clk, 70)
    assert eps.soc_pct == pytest.approx(40.0, abs=1.0) and eps.soc_from_voltage
    assert E.soc_from_cell_voltage(4.25) == 100 and E.soc_from_cell_voltage(3.0) == 0
    assert E.soc_from_cell_voltage(3.845) == pytest.approx(52.5)


def test_energy_counter_and_gap_cap():
    eps, fake, clk, _ = rig(vbus=8000, cur=500, shunt=0.01, maxa=3.2)  # 4 W
    step(eps, clk, 3601)
    assert eps.e_load_mwh == pytest.approx(4000, rel=0.01)
    clk.t += 3600                                                  # a 1 h stall counts as 10 s at most
    step(eps, clk, 1)
    assert eps.e_load_mwh == pytest.approx(4000 + 4000 * 10 / 3600, rel=0.01)


# ---------------------------------------------------------------- GET_STATE bytes
def test_state_bytes_layout_reset_and_na():
    eps, fake, clk, _ = rig(vbus=7600, cur=400)
    lit(eps)
    step(eps, clk, 5)
    fake.vbus_mv, fake.current_ma = 7000, 700
    step(eps, clk)
    b = eps.state_bytes()
    assert len(b) == I.GL_EPSS_LEN == 24
    d = E.decode_state(b)
    assert d["vbatt_mV"] == pytest.approx(7000, abs=2) and d["iload_mA"] == pytest.approx(700, abs=1)
    assert d["ibatt_mA"] is None and d["psolar_mW"] is None                     # INA226 #2 not fitted
    assert d["vmin_mV"] == pytest.approx(7000, abs=2) and d["vmax_mV"] == pytest.approx(7600, abs=2)
    assert d["imax_mA"] == pytest.approx(700, abs=1)
    assert d["state"] == "SUN_CHARGING" and d["sun"]["lit"] and d["sun"]["adcs_fresh"]
    assert d["flags"]["ina_load_ok"] and not d["flags"]["ina_batt_fitted"] and d["flags"]["soc_from_voltage"]
    assert 0 <= d["soc_pct"] <= 100
    d2 = E.decode_state(eps.state_bytes())                                     # min/max restarted
    assert d2["vmin_mV"] is None and d2["vmax_mV"] is None and d2["imax_mA"] is None
    step(eps, clk)
    d3 = E.decode_state(eps.state_bytes(reset_stats=False))
    assert d3["vmin_mV"] == pytest.approx(7000, abs=2)


def test_state_bytes_clamps():
    eps, fake, clk, _ = rig(shunt=0.01, maxa=8.0)
    fake.current_ma = -5000
    step(eps, clk)
    eps.e_load_mwh = 1e12
    eps.state_since = clk.t - 1e6
    d = E.decode_state(eps.state_bytes())
    assert d["iload_mA"] == pytest.approx(-5000, abs=2)
    assert d["e_load_mWh"] == 0xFFFFFFFF and d["state_age_s"] == 0xFFFF
    with pytest.raises(ValueError):
        E.decode_state(b"\0" * 23)


def test_no_batt_lines_active_in_plain_file():
    txt = SRC.read_text()
    assert "self.batt_ina = ina" not in [l.strip() for l in txt.splitlines() if not l.strip().startswith("#")]
    assert not hasattr(I, "ICD_ADDR_INA226_BATT")
    assert txt.count("# BATT: ") >= 40


# ---------------------------------------------------------------- INA226 #2 switched on
@pytest.fixture(scope="module")
def B(tmp_path_factory):
    """Software/ tree with the ICD line uncommented + regenerated and EPS.py run
    through the exact sed command from its docstring."""
    root = tmp_path_factory.mktemp("batt")
    shutil.copytree(SW / "ICD", root / "ICD")
    (root / "OBC").mkdir()
    shutil.copy(SW / "OBC" / "Payload.py", root / "OBC" / "Payload.py")
    shutil.copy(SRC, root / "OBC" / "EPS.py")
    h = root / "ICD" / "cubesat_icd.h"
    t = h.read_text()
    assert t.count("// #define ICD_ADDR_INA226_BATT") == 1
    h.write_text(t.replace("// #define ICD_ADDR_INA226_BATT", "#define ICD_ADDR_INA226_BATT"))
    subprocess.run([sys.executable, "-B", str(root / "ICD" / "tools" / "gen_icd_py.py")], check=True,
                   capture_output=True)
    doc = SRC.read_text()
    m = re.search(r"sed -i '(.*?)' EPS\.py", doc)
    sed_expr = m.group(1).replace("\\\\", "\\")                   # docstring escapes -> real text
    subprocess.run(["sed", "-i", sed_expr, "EPS.py"], cwd=root / "OBC", check=True)
    out = (root / "OBC" / "EPS.py").read_text()
    assert not [l for l in out.splitlines() if l.lstrip().startswith("# BATT: ")]
    compile(out, "EPS.py", "exec")
    saved = sys.modules.pop("icd_defs", None)          # use the regenerated ICD, not the cached one
    path_before = list(sys.path)
    try:
        mod = load(root / "OBC" / "EPS.py", "EPS_batt")
    finally:
        sys.path[:] = path_before
        if saved is not None:
            sys.modules["icd_defs"] = saved
    assert mod.I.ICD_ADDR_INA226_BATT == 0x41
    return mod


def batt_rig(B, vload=7600, iload=500, ibatt=0.0, cfg=None):
    batt = B.FakeINA226(vload, ibatt, shunt_ohm=0.1)
    eps, fake, clk, syncs = rig(B, vbus=vload, cur=iload, shunt=0.1, maxa=0.8, cfg=cfg, extra={0x41: batt})
    return eps, fake, batt, clk


def test_batt_attached_and_flags(B):
    eps, fake, batt, clk = batt_rig(B)
    assert eps.batt_ina is not None and eps.batt_ok
    step(eps, clk)
    f = eps.flags()
    assert f & B.I.GL_EF_INA_BATT_FITTED and f & B.I.GL_EF_INA_BATT_OK
    batt.present = False
    step(eps, clk)
    assert not eps.flags() & B.I.GL_EF_INA_BATT_OK and eps.ibatt_mA is None
    batt.present = True
    step(eps, clk)
    assert eps.flags() & B.I.GL_EF_INA_BATT_OK and eps.ibatt_mA is not None


def test_batt_states_from_current(B):
    eps, fake, batt, clk = batt_rig(B, iload=500, cfg={"batt_deadband_ma": 20})
    batt.current_ma = -300                                          # charging, no sun data at all
    r = step(eps, clk)
    assert eps.state == B.I.GL_PS_SUN_CHARGING and r["ibatt_mA"] == pytest.approx(-300, abs=1)
    assert eps.psolar_mw() == pytest.approx(7600 * 800 / 1000, rel=0.01)  # load 500 + charge 300
    eps.update_sun(sun_raw=[3000] + [0] * 5)
    batt.current_ma = 200                                           # sunlit but discharging
    step(eps, clk)
    assert eps.state == B.I.GL_PS_SUN_DISCHARGING
    fake.vbus_mv = batt.vbus_mv = 8350
    batt.current_ma = 5                                             # CV, tapered
    step(eps, clk)
    assert eps.state == B.I.GL_PS_SUN_FULL
    for _ in range(20):
        eps.update_sun(sun_raw=[0] * 6)
        fake.vbus_mv = 7500
        batt.current_ma = 500
        step(eps, clk)
    assert eps.state == B.I.GL_PS_ECLIPSE
    d = B.decode_state(eps.state_bytes())
    assert d["ibatt_mA"] == pytest.approx(500, abs=1) and d["psolar_mW"] == pytest.approx(0, abs=5)
    assert not d["flags"]["soc_from_voltage"]


def test_batt_charge_counting(B):
    eps, fake, batt, clk = batt_rig(B, vload=7600, iload=0, ibatt=0,
                                    cfg={"capacity_mah": 2000, "r_int_ohm": 0.0, "full_current_ma": 50})
    step(eps, clk)
    assert eps.soc_pct == pytest.approx(40, abs=1)                  # seeded from 3.80 V/cell
    batt.current_ma = 400                                           # discharge 400 mA for 30 min = 200 mAh
    step(eps, clk, 1800)
    assert eps.soc_pct == pytest.approx(40 - 10, abs=0.5)
    batt.current_ma = -800                                          # charge 800 mA for 15 min = +200 mAh
    step(eps, clk, 900)
    assert eps.soc_pct == pytest.approx(40, abs=0.5)
    fake.vbus_mv = 8350
    batt.current_ma = 10
    step(eps, clk)
    assert eps.soc_pct == pytest.approx(100.0)                      # full re-anchors the count
    batt.current_ma = 4000                                          # cannot go below 0
    step(eps, clk, 3 * 3600)
    assert eps.soc_pct == 0.0


def test_batt_icd_constant_missing_is_reported(B, caplog, monkeypatch):
    monkeypatch.delattr(B.I, "ICD_ADDR_INA226_BATT")
    eps, fake, clk, _ = rig(B, extra={0x41: B.FakeINA226()})
    assert eps.batt_ina is None and "commented out" in caplog.text


# ---------------------------------------------------------------- bench CLI
def test_cli_with_fake_smbus2(monkeypatch, capsys):
    fake = E.FakeINA226(7800, 300, shunt_ohm=0.1)

    class Msg:
        def __init__(self, kind, addr, data):
            self.kind, self.addr, self.data = kind, addr, data

        def __iter__(self):
            return iter(self.data)

    class i2c_msg:
        @staticmethod
        def write(addr, data):
            return Msg("w", addr, bytes(data))

        @staticmethod
        def read(addr, n):
            return Msg("r", addr, bytearray(n))

    class SMBus:
        def __init__(self, n):
            pass

        def i2c_rdwr(self, *msgs):
            for m in msgs:
                if m.addr != 0x40:
                    raise OSError(121, "Remote I/O error")
                if m.kind == "w":
                    fake.write(m.data)
                else:
                    m.data[:] = fake.read(len(m.data))

        def close(self):
            pass

    mod = types.ModuleType("smbus2")
    mod.SMBus, mod.i2c_msg = SMBus, i2c_msg
    monkeypatch.setitem(sys.modules, "smbus2", mod)
    monkeypatch.setattr(E.time, "sleep", lambda s: None)
    monkeypatch.syspath_prepend(str(SRC.parent))
    assert E.main(["--bus", "3", "--count", "2", "--shunt", "0.1", "--max-current", "0.8"]) == 0
    out = capsys.readouterr().out.strip().splitlines()
    assert len(out) == 2 and "'vbus_mV': 7800" in out[0] and "'clipped': False" in out[0]
