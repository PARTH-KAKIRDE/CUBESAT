"""Payload.py: TSL2591, VEML6075, Pi camera, magnetometer logging, log files, standalone mode.
Fake chips at register level and fake camera commands; no hardware needed."""
import json
import os
import sys
import time
import types
from pathlib import Path

import pytest

from OBC import Payload as P

I = P.I


# ---------------------------------------------------------------- fakes
class FakeTSL:
    """Register-level TSL2591. AVALID sets after `integrations_needed` STATUS reads once enabled."""
    def __init__(self, full=1200, ir=300, ident=0x50):
        self.full, self.ir, self.ident = full, ir, ident
        self.enable = 0
        self.control = None
        self.status_reads = 0
        self.ptr = 0
        self.data_reads = []          # n of each read from 0x14
        self.present = True

    def write(self, data):
        if not self.present:
            raise P.BusError("NACK")
        assert data[0] & 0xE0 == 0xA0, "command bit + normal transaction"
        self.ptr = data[0] & 0x1F
        if len(data) == 2:
            if self.ptr == 0x00:
                if not (self.enable & 0x02) and data[1] & 0x02:
                    self.status_reads = 0          # AEN newly asserted
                self.enable = data[1]
            elif self.ptr == 0x01:
                self.control = data[1]

    def read(self, n):
        if not self.present:
            raise P.BusError("NACK")
        if self.ptr == 0x12:
            return bytes([self.ident])
        if self.ptr == 0x13:
            self.status_reads += 1
            return bytes([0x01 if (self.enable & 0x03) == 0x03 and self.status_reads > 1 else 0x00])
        if self.ptr == 0x14:
            self.data_reads.append(n)
            return bytes([self.full & 0xFF, self.full >> 8, self.ir & 0xFF, self.ir >> 8])[:n]
        return bytes(n)


class FakeVEML:
    def __init__(self, uva=500, uvb=600, c1=50, c2=40, ident=0x26):
        self.regs = {0x07: uva, 0x09: uvb, 0x0A: c1, 0x0B: c2, 0x0C: 0x0400 | ident}
        self.conf = None
        self.ptr = 0

    def write(self, data):
        self.ptr = data[0]
        if len(data) == 3 and data[0] == 0x00:
            self.conf = data[1] | (data[2] << 8)

    def read(self, n):
        v = self.regs.get(self.ptr, 0)
        return bytes([v & 0xFF, v >> 8])[:n]


class Bus:
    def __init__(self, devs):
        self.devs = devs
        self.resets = 0
        self.log = []

    def write(self, addr, data):
        if addr not in self.devs:
            raise P.BusError(f"0x{addr:02X} NACK")
        self.log.append(("w", addr, bytes(data)))
        self.devs[addr].write(bytes(data))

    def write_read(self, addr, data, n):
        if addr not in self.devs:
            raise P.BusError(f"0x{addr:02X} NACK")
        self.log.append(("wr", addr, bytes(data), n))
        self.devs[addr].write(bytes(data))
        return self.devs[addr].read(n)

    def reset(self):
        self.resets += 1


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def lines(d):
    out = []
    for f in sorted((Path(d) / "payload").glob("payload_*.jsonl")):
        for ln in f.read_text().splitlines():
            out.append(json.loads(ln))
    return out


def adafruit_lux(c0, c1, atime, again):
    # Formula copied from adafruit_circuitpython_tsl2591 1.4.8 lux property
    cpl = (atime * again) / 408.0
    lux1 = (c0 - (1.64 * c1)) / cpl
    lux2 = ((0.59 * c0) - (0.86 * c1)) / cpl
    return max(lux1, lux2)


def no_cam(extra=None):
    c = {"camera": {"enabled": False}}
    c.update(extra or {})
    return c


# ---------------------------------------------------------------- TSL2591
def test_tsl_not_ready_then_ok_one_transaction(tmp_path):
    tsl = FakeTSL(1200, 300)
    clk = Clock()
    p = P.Payload(no_cam(), Bus({0x29: tsl}), tmp_path, clk)
    r1 = p.tick()["tsl2591"]
    assert r1 == {"status": "NOT_READY"}            # first status read: AVALID not yet set
    assert tsl.control == 0x00                       # low gain, 100 ms
    assert tsl.enable == 0x03
    clk.t += 10
    r2 = p.tick()["tsl2591"]
    assert r2["status"] == "OK" and r2["full"] == 1200 and r2["ir"] == 300 and r2["vis"] == 900
    assert tsl.data_reads == [4]                      # both channels in ONE 4-byte read
    assert r2["lux"] == pytest.approx(adafruit_lux(1200, 300, 100, 1.0), abs=1e-3)
    assert p.last_lux == r2["lux"]


@pytest.mark.parametrize("full,ir,sat", [(36863, 36863, True), (36863, 20000, True), (36862, 36000, False),
                                         (40000, 100, True), (100, 36863, True)])
def test_tsl_saturation_100ms(tmp_path, full, ir, sat):
    clk = Clock()
    p = P.Payload(no_cam(), Bus({0x29: FakeTSL(full, ir)}), tmp_path, clk)
    p.tick(); clk.t += 10
    r = p.tick()["tsl2591"]
    assert r["status"] == ("SATURATED" if sat else "OK")
    if sat:
        assert r["lux"] is None and p.last_lux == float("inf")
        assert r["full"] == full                      # raw counts still logged


def test_tsl_longer_integration_limit_65535(tmp_path):
    clk = Clock()
    cfg = no_cam({"tsl2591": {"integration_ms": 200, "gain": "med"}})
    tsl = FakeTSL(40000, 1000)
    p = P.Payload(cfg, Bus({0x29: tsl}), tmp_path, clk)
    p.tick(); clk.t += 10
    r = p.tick()["tsl2591"]
    assert tsl.control == 0x10 | 0x01
    assert r["status"] == "OK"
    assert r["lux"] == pytest.approx(adafruit_lux(40000, 1000, 200, 25.0), abs=1e-3)
    tsl.full = 65535
    clk.t += 10
    assert p.tick()["tsl2591"]["status"] == "SATURATED"


def test_tsl_dark_and_negative_lux_clamped(tmp_path):
    clk = Clock()
    tsl = FakeTSL(0, 0)
    p = P.Payload(no_cam(), Bus({0x29: tsl}), tmp_path, clk)
    p.tick(); clk.t += 10
    assert p.tick()["tsl2591"]["lux"] == 0.0
    tsl.full, tsl.ir = 1000, 900                      # mostly IR: formula goes negative
    clk.t += 10
    assert p.tick()["tsl2591"]["lux"] == 0.0


def test_tsl_unplug_recover_and_bus_reset(tmp_path):
    clk = Clock()
    tsl = FakeTSL()
    bus = Bus({0x29: tsl})
    p = P.Payload(no_cam(), bus, tmp_path, clk)
    p.tick(); clk.t += 10
    assert p.tick()["tsl2591"]["status"] == "OK"
    tsl.present = False
    for i in range(5):
        clk.t += 10
        assert p.tick()["tsl2591"]["status"].startswith("ERR:")
    assert bus.resets == 1
    tsl.present = True
    clk.t += 10
    assert p.tick()["tsl2591"]["status"] in ("OK", "NOT_READY")   # re-init, never a bogus value
    clk.t += 10
    assert p.tick()["tsl2591"]["status"] == "OK"


def test_tsl_bad_id(tmp_path):
    clk = Clock()
    p = P.Payload(no_cam(), Bus({0x29: FakeTSL(ident=0x11)}), tmp_path, clk)
    assert p.tick()["tsl2591"]["status"] == "ERR:BusError"
    assert p.last_lux is None


def test_tsl_missing_and_bad_config(tmp_path):
    clk = Clock()
    p = P.Payload(no_cam(), Bus({}), tmp_path, clk)
    assert p.tick()["tsl2591"]["status"].startswith("ERR")
    with pytest.raises(ValueError):
        P.TSL2591(Bus({}), 0x29, "huge", 100)
    with pytest.raises(ValueError):
        P.TSL2591(Bus({}), 0x29, "low", 150)


def test_sample_period_and_no_burst(tmp_path):
    clk = Clock()
    p = P.Payload(no_cam(), Bus({0x29: FakeTSL()}), tmp_path, clk)
    n = 0
    for _ in range(35):                               # 35 s of 1 s ticks
        if "tsl2591" in p.tick():
            n += 1
        clk.t += 1
    assert n == 4                                      # t=0,10,20,30
    clk.t += 1000                                      # a long stall: one sample, not 100
    got = sum("tsl2591" in p.tick() for _ in range(1))
    clk.t += 1
    assert got == 1 and "tsl2591" not in p.tick()


# ---------------------------------------------------------------- VEML6075
def test_veml_disabled_while_commented_out(tmp_path, caplog):
    assert not hasattr(I, "ICD_ADDR_VEML6075")        # commented out in the ICD
    p = P.Payload(no_cam({"veml6075": {"enabled": True}}), Bus({0x29: FakeTSL()}), tmp_path, Clock())
    assert p.uv is None and "commented out" in caplog.text
    assert "veml6075" not in p.tick()


def test_veml_enabled(tmp_path, monkeypatch):
    monkeypatch.setattr(I, "ICD_ADDR_VEML6075", 0x10, raising=False)
    clk = Clock()
    v = FakeVEML(uva=500, uvb=600, c1=50, c2=40)
    p = P.Payload(no_cam({"veml6075": {"enabled": True}, "tsl2591": {"enabled": False}}),
                  Bus({0x10: v}), tmp_path, clk)
    assert p.tick()["veml6075"] == {"status": "NOT_READY"}
    assert v.conf == 0x0010                            # IT=100 ms, powered, continuous
    clk.t += 10
    r = p.tick()["veml6075"]
    a = 500 - 2.22 * 50 - 1.33 * 40
    b = 600 - 2.95 * 50 - 1.74 * 40
    assert r["status"] == "OK"
    assert r["uvi"] == pytest.approx((a * 0.001461 + b * 0.002591) / 2, abs=1e-3)
    v.regs[0x07] = 65535
    clk.t += 10
    r = p.tick()["veml6075"]
    assert r["status"] == "SATURATED" and r["uvi"] is None and r["uva"] == 65535


def test_veml_it_scaling_and_bad_id(tmp_path, monkeypatch):
    monkeypatch.setattr(I, "ICD_ADDR_VEML6075", 0x10, raising=False)
    clk = Clock()
    v = FakeVEML(1000, 1000, 0, 0)
    p = P.Payload(no_cam({"veml6075": {"enabled": True, "integration_ms": 200}, "tsl2591": {"enabled": False}}),
                  Bus({0x10: v}), tmp_path, clk)
    p.tick(); clk.t += 10
    r = p.tick()["veml6075"]
    assert v.conf == 0x0020
    assert r["uvi"] == pytest.approx((1000 * 0.001461 + 1000 * 0.002591) / 2 * 0.5, abs=1e-3)
    bad = P.Payload(no_cam({"veml6075": {"enabled": True}, "tsl2591": {"enabled": False}}),
                    Bus({0x10: FakeVEML(ident=0x99)}), tmp_path / "b", Clock())
    assert bad.tick()["veml6075"]["status"] == "ERR:BusError"


# ---------------------------------------------------------------- magnetometer
def sens(uptime, age, mag=(10.0, -20.0, 30.0), stale=False):
    return {"uptime_ms": uptime, "mag_uT": list(mag) if age is not None else None,
            "mag_age_ms": age, "mag_stale": stale, "gyro_dps": [0, 0, 0]}


def test_mag_fresh_dedupe_stale_invalid_wrap():
    m = P.MagLogger()
    assert m.update(None) is None
    assert m.update(sens(5000, None)) is None                    # never valid
    r = m.update(sens(5000, 200))
    assert r == {"adcs_sample_ms": 4800, "age_ms": 200, "uT": [10.0, -20.0, 30.0]}
    assert m.update(sens(5900, 1100)) is None                    # same sample (4800) again
    assert m.update(sens(7000, 1600, stale=True)) is None        # stale: never presented as fresh
    assert m.update(sens(7000, 200))["adcs_sample_ms"] == 6800
    assert m.update(sens(100, 300))["adcs_sample_ms"] == (100 - 300) & 0xFFFFFFFF   # ADCS clock wrap
    assert m.update(sens(50, 10))["adcs_sample_ms"] == 40         # ADCS rebooted: still logged
    assert m.update({"uptime_ms": 1}) is None                     # missing keys: skipped


def test_mag_in_payload_records(tmp_path):
    clk = Clock()
    p = P.Payload(no_cam({"tsl2591": {"enabled": False}}), None, tmp_path, clk)
    assert p.tick(adcs_sensors=sens(1000, 100))["mag"]["adcs_sample_ms"] == 900
    clk.t += 1
    assert p.tick(adcs_sensors=sens(1500, 600)) == {}             # duplicate -> nothing written
    clk.t += 1
    assert "mag" in p.tick(adcs_sensors=sens(2000, 100))
    p.close()
    assert [r["mag"]["adcs_sample_ms"] for r in lines(tmp_path)] == [900, 1900]
    q = P.Payload(no_cam({"tsl2591": {"enabled": False}, "mag": {"enabled": False}}), None, tmp_path / "x", clk)
    assert q.tick(adcs_sensors=sens(1000, 100)) == {}


# ---------------------------------------------------------------- camera
def cam_script(tmp_path, body):
    s = tmp_path / "fakecam.py"
    s.write_text("import sys,time\nargs=sys.argv[1:]\nout=args[args.index('-o')+1]\n" + body)
    sh = tmp_path / "fakecam"
    sh.write_text(f"#!/bin/sh\nexec {sys.executable} -B {s} \"$@\"\n")
    sh.chmod(0o755)
    return str(sh)


GOOD = "open(out,'wb').write(b'\\xff\\xd8' + b'x'*1000 + b'\\xff\\xd9')\n"


def run_until(p, pred, timeout=10, **kw):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = p.tick(**kw)
        if pred(r):
            return r
        time.sleep(0.02)
    raise AssertionError("timeout")


def cam_cfg(cmd, **over):
    c = {"enabled": True, "command": cmd, "period_s": 0, "timeout_s": 3}
    c.update(over)
    return {"camera": c, "tsl2591": {"enabled": False}}


def test_camera_capture_atomic_and_nonblocking(tmp_path):
    cmd = cam_script(tmp_path, "time.sleep(0.5)\n" + GOOD)
    p = P.Payload(cam_cfg(cmd), None, tmp_path, time.monotonic)
    assert p.request_capture(I.ICD_MODE_NOMINAL) == "started"
    t = time.monotonic(); p.tick(); assert time.monotonic() - t < 0.1   # tick never waits for the camera
    assert p.request_capture(I.ICD_MODE_NOMINAL) == "busy"
    r = run_until(p, lambda r: "camera" in r)["camera"]
    assert r["image"].startswith("img_000000_") and r["image"].endswith("Z.jpg")
    imgs = list((tmp_path / "images").iterdir())
    assert [i.name for i in imgs if i.name.endswith(".jpg")] == [r["image"]]
    assert not (tmp_path / "images" / "capture_tmp.jpg").exists()
    assert p.images == 1
    assert p.request_capture(None) == "started"
    r2 = run_until(p, lambda r: "camera" in r)["camera"]
    assert r2["image"].startswith("img_000001_")
    p.close()


def test_camera_refused_in_safe_and_low_battery(tmp_path):
    cmd = cam_script(tmp_path, GOOD)
    p = P.Payload(cam_cfg(cmd), None, tmp_path, time.monotonic)
    assert p.request_capture(I.ICD_MODE_SAFE) == "refused"
    assert p.request_capture(I.ICD_MODE_NOMINAL, low_battery=True) == "refused"
    assert P.Payload({"camera": {"enabled": False}}, None, tmp_path / "d").request_capture() == "disabled"


@pytest.mark.parametrize("body,why", [
    ("time.sleep(60)\n", "timeout"),
    ("sys.exit(3)\n", "exit 3"),
    ("open(out,'wb').write(b'')\n", "not a JPEG"),
    ("open(out,'wb').write(b'GIF89a')\n", "not a JPEG"),
    ("pass\n", "No such file"),
])
def test_camera_failures(tmp_path, body, why):
    cmd = cam_script(tmp_path, body)
    p = P.Payload(cam_cfg(cmd, timeout_s=1.5), None, tmp_path, time.monotonic)
    assert p.request_capture() == "started"
    r = run_until(p, lambda r: "camera" in r)["camera"]
    assert r["image"] == "CAM_FAIL" and why in r["error"]
    assert not (tmp_path / "images" / "capture_tmp.jpg").exists()
    assert not any(n.endswith(".jpg") for n in os.listdir(tmp_path / "images"))
    assert not p.cam.busy and p.images == 0


def test_camera_timeout_kills_process_group(tmp_path):
    marker = tmp_path / "child_alive"
    body = ("import subprocess\n"
            f"subprocess.Popen([sys.executable,'-c','import time;time.sleep(3);open(r\"{marker}\",\"w\")'])\n"
            "time.sleep(60)\n")
    p = P.Payload(cam_cfg(cam_script(tmp_path, body), timeout_s=0.8), None, tmp_path, time.monotonic)
    p.request_capture()
    r = run_until(p, lambda r: "camera" in r)["camera"]
    assert "timeout" in r["error"]
    time.sleep(3.5)
    assert not marker.exists(), "grandchild survived the kill"


def test_camera_noisy_stderr_no_deadlock(tmp_path):
    body = "sys.stderr.write('libcamera says hi\\n'*20000); sys.stderr.flush()\n" + GOOD
    p = P.Payload(cam_cfg(cam_script(tmp_path, body)), None, tmp_path, time.monotonic)
    p.request_capture()
    r = run_until(p, lambda r: "camera" in r)["camera"]
    assert r["image"] != "CAM_FAIL", r


def test_camera_missing_command(tmp_path, monkeypatch):
    monkeypatch.setattr(P.shutil, "which", lambda n: None)
    p = P.Payload({"camera": {"enabled": True, "period_s": 0}, "tsl2591": {"enabled": False}}, None, tmp_path)
    assert p.request_capture() == "error"
    assert p.tick() == {}


def test_camera_periodic_and_safe_pause(tmp_path):
    cmd = cam_script(tmp_path, GOOD)
    clk = Clock()
    cfg = cam_cfg(cmd, period_s=60)
    p = P.Payload(cfg, None, tmp_path, clk)
    def go(mode, secs):
        n = 0
        for _ in range(secs):
            p.tick(mode)
            clk.t += 1
            # let the child finish in real time
            if p.cam.busy:
                end = time.monotonic() + 5
                while p.cam.busy and time.monotonic() < end:
                    r = p.tick(mode)
                    n += "camera" in r and r["camera"]["image"] != "CAM_FAIL"
                    time.sleep(0.02)
        return n
    assert go(I.ICD_MODE_NOMINAL, 130) == 3          # t=5, 65, 125
    assert go(I.ICD_MODE_SAFE, 200) == 0             # none in SAFE
    assert go(I.ICD_MODE_NOMINAL, 2) == 1            # resumes at once after SAFE
    p.close()


def test_seq_continues_and_old_names_and_prune(tmp_path):
    d = tmp_path / "images"; d.mkdir()
    (d / "img_000007.jpg").write_bytes(b"\xff\xd8" + b"a" * 600_000)          # old payload.py name
    (d / "img_000008_20261101T000000Z.jpg").write_bytes(b"\xff\xd8" + b"b" * 600_000)
    (d / "capture_tmp.jpg").write_bytes(b"junk")                                # power cut leftover
    (d / "notes.txt").write_text("keep me")
    cmd = cam_script(tmp_path, GOOD)
    p = P.Payload(cam_cfg(cmd, max_images_mb=1.5, reserve_mb=0.5, min_free_mb=0), None, tmp_path, time.monotonic)
    assert not (d / "capture_tmp.jpg").exists()
    assert p.cam.seq == 9
    p.request_capture()
    r = run_until(p, lambda r: "camera" in r)["camera"]
    assert r["image"].startswith("img_000009_")
    names = sorted(os.listdir(d))
    assert "img_000007.jpg" not in names            # oldest pruned to stay under the limit
    assert "img_000008_20261101T000000Z.jpg" in names and "notes.txt" in names


# ---------------------------------------------------------------- log file
def test_log_valid_json_and_half_line_repair(tmp_path, monkeypatch):
    clk = Clock()
    pdir = tmp_path / "payload"; pdir.mkdir()
    day = P.utc_now().strftime("%Y-%m-%d")
    (pdir / f"payload_{day}.jsonl").write_text('{"ok": 1}\n{"half": ')
    p = P.Payload(no_cam(), Bus({0x29: FakeTSL()}), tmp_path, clk)
    for _ in range(3):
        p.tick(I.ICD_MODE_NOMINAL); clk.t += 10
    p.close()
    raw = (pdir / f"payload_{day}.jsonl").read_text().splitlines()
    assert raw[0] == '{"ok": 1}' and raw[1] == '{"half": '
    recs = [json.loads(x) for x in raw[2:]]
    assert len(recs) == 3 and all(r["obc_mode"] == I.ICD_MODE_NOMINAL and r["t"].endswith("+00:00") for r in recs)


def test_fsync_period(tmp_path, monkeypatch):
    calls = []
    real = os.fsync
    monkeypatch.setattr(P.os, "fsync", lambda fd: (calls.append(fd), real(fd)))
    clk = Clock()
    p = P.Payload(no_cam({"sample_period_s": 1.0}), Bus({0x29: FakeTSL()}), tmp_path, clk)
    for _ in range(30):
        p.tick(); clk.t += 1
    assert 2 <= len(calls) <= 4                       # every 10 s, not every line
    p.close()
    assert len(calls) >= 3


def test_day_rollover(tmp_path, monkeypatch):
    from datetime import datetime, timezone
    clk = Clock()
    days = iter([datetime(2026, 11, 12, 23, 59, 59, tzinfo=timezone.utc)] * 3 +
                [datetime(2026, 11, 13, 0, 0, 1, tzinfo=timezone.utc)] * 10)
    monkeypatch.setattr(P, "utc_now", lambda: next(days))
    p = P.Payload(no_cam({"tsl2591": {"enabled": False}}), None, tmp_path, clk)
    p.tick(adcs_sensors=sens(1000, 10)); clk.t += 1
    p.tick(adcs_sensors=sens(2000, 10)); clk.t += 1
    p.close()
    files = sorted(x.name for x in (tmp_path / "payload").iterdir())
    assert files == ["payload_2026-11-12.jsonl", "payload_2026-11-13.jsonl"]


def test_write_error_does_not_raise(tmp_path, monkeypatch):
    clk = Clock()
    p = P.Payload(no_cam(), Bus({0x29: FakeTSL()}), tmp_path, clk)
    monkeypatch.setattr(p, "_open", lambda: (_ for _ in ()).throw(OSError(28, "No space left")))
    p.tick()                                           # must not raise


def test_tick_never_raises_on_garbage(tmp_path):
    p = P.Payload(no_cam(), None, tmp_path, Clock())
    p.tick(adcs_sensors={"uptime_ms": "x", "mag_uT": [1, 2, 3], "mag_age_ms": 5, "mag_stale": False})


# ---------------------------------------------------------------- standalone
def test_standalone_main_with_fake_smbus2(tmp_path, monkeypatch):
    tsl = FakeTSL()
    class Msg:
        def __init__(self, kind, addr, data):
            self.kind, self.addr, self.data = kind, addr, data
        def __iter__(self):
            return iter(self.data)
    class i2c_msg:
        @staticmethod
        def write(addr, data): return Msg("w", addr, bytes(data))
        @staticmethod
        def read(addr, n): return Msg("r", addr, bytearray(n))
    class SMBus:
        opened = 0
        def __init__(self, n): SMBus.opened += 1; self.n = n
        def i2c_rdwr(self, *msgs):
            for m in msgs:
                if m.addr != 0x29:
                    raise OSError(121, "Remote I/O error")
                if m.kind == "w":
                    tsl.write(m.data)
                else:
                    m.data[:] = tsl.read(len(m.data))
        def close(self): pass
    mod = types.ModuleType("smbus2"); mod.SMBus, mod.i2c_msg = SMBus, i2c_msg
    monkeypatch.setitem(sys.modules, "smbus2", mod)
    assert P.main(["--bus", "3", "--data-dir", str(tmp_path), "--no-camera", "--ticks", "12"]) == 0
    recs = lines(tmp_path)
    assert [r["tsl2591"]["status"] for r in recs] == ["NOT_READY", "OK"]
    assert all("mag" not in r for r in recs)
    b = P.SMBus2Bus(3); b.reset(); assert SMBus.opened >= 3
