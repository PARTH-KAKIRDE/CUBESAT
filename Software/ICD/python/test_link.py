#!/usr/bin/env python3
"""ICD link acceptance tests (deliverables 4 and 5), run on the OBC.

  python3 test_link.py                          all tests against /dev/i2c-1, ADCS address
  python3 test_link.py --addr 0x33              TT&C (or the dummy slave built for 0x33)
  python3 test_link.py --bus 11                 bit-banged bus (clock-stretching fallback)
  python3 test_link.py --sim-lib PATH           against the PC firmware simulation
                                                (Software/build/host/libcubesat_sim.so)

Tests:
  0) CRC self-test, bus speed, GET_VERSION (fails on an ICD major mismatch, warns on minor)
  a) bad frames are rejected with the right status and never executed:
     flipped CRC byte, flipped payload bit, LEN > 24, frame shorter than LEN,
     PING with a payload, unknown command; then a corrupted SET_MODE must not
     change the mode
  b) unplugged slave: transactions fail quickly -> COMM_FAULT; replugged -> recovers
     (interactive on hardware, automatic with --sim-lib)
  c) N PINGs (default 10,000) with zero errors, with an error histogram

If (c) shows errors or many read-CRC errors, switch the link to the bit-banged
i2c-gpio bus (Raspberry Pi clock-stretching problem; see ICD.md section 1).
"""
from __future__ import annotations

import argparse
import ctypes
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import icd_defs as I  # noqa: E402
from icd_link import (BusError, I2CDev, LinkError, SlaveError, SlaveLink, build_command,  # noqa: E402
                      bus_speed_hz, crc16, crc_selftest, status_name)

results = []


def report(name, ok, detail=""):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}")


def skip(name, why):
    print(f"[SKIP] {name} ({why})")


class SimBus:
    """The firmware simulation (built by Software/firmware/build.py sim). Time moves only in wait_ms."""

    def __init__(self, path: str, seed: int = 7):
        self.lib = L = ctypes.CDLL(path)
        u8p = ctypes.POINTER(ctypes.c_uint8)
        L.sim_init.argtypes = [ctypes.c_uint32]
        L.sim_run_ms.argtypes = [ctypes.c_uint32]
        L.sim_i2c_write.argtypes = [ctypes.c_uint8, u8p, ctypes.c_int]
        L.sim_i2c_read.argtypes = [ctypes.c_uint8, u8p, ctypes.c_int]
        L.sim_set_present.argtypes = [ctypes.c_uint8, ctypes.c_int]
        L.sim_init(seed)
        L.sim_run_ms(100)

    def write(self, addr, data):
        buf = (ctypes.c_uint8 * max(1, len(data))).from_buffer_copy(bytes(data) or b"\0")
        if self.lib.sim_i2c_write(addr, buf, len(data)) != 0:
            raise BusError(f"sim 0x{addr:02X} write NACK")

    def read(self, addr, n):
        buf = (ctypes.c_uint8 * n)()
        if self.lib.sim_i2c_read(addr, buf, n) != 0:
            raise BusError(f"sim 0x{addr:02X} read NACK")
        return bytes(buf)

    def wait_ms(self, ms):
        if ms > 0:
            self.lib.sim_run_ms(int(round(ms)))

    def set_present(self, addr, present):
        self.lib.sim_set_present(addr, int(present))


# ------------------------------------------------------------------ 0) basics

def test_basics(link, bus_num, sim):
    crc_selftest()
    report("CRC-16 check value", True, f"(0x{crc16(b'123456789'):04X})")
    if sim is None:
        hz = bus_speed_hz(bus_num)
        if hz is None:
            print(f"   bus speed: unknown (set dtparam=i2c_arm_baudrate={I.ICD_I2C_SPEED_HZ})")
        else:
            report("bus speed", hz == I.ICD_I2C_SPEED_HZ, f"({hz} Hz, ICD {I.ICD_I2C_SPEED_HZ})")
    p = link.call(I.ICD_CMD_GET_VERSION)
    ok = len(p) == I.ICD_GET_VERSION_RESP_LEN and p[3] == I.ICD_VERSION_MAJOR
    report("GET_VERSION", ok, f"fw {p[0]}.{p[1]}.{p[2]}, slave ICD {p[3]}.{p[4]}, "
                              f"this tool ICD {I.ICD_VERSION_MAJOR}.{I.ICD_VERSION_MINOR}")
    if ok and p[4] != I.ICD_VERSION_MINOR:
        print("   WARNING: slave built against a different ICD minor version (compatible, but update it)")


# ------------------------------------------------------------------ a) bad frames

def test_bad_frames(link):
    ping = I.ICD_CMD_PING
    good = build_command(ping)
    with_byte = build_command(ping, b"\x00")
    cases = [
        ("flipped CRC byte", good[:-1] + bytes([good[-1] ^ 0xFF]), I.ICD_ST_BAD_CRC),
        ("flipped payload bit", with_byte[:2] + b"\x01" + with_byte[3:], I.ICD_ST_BAD_CRC),
        ("LEN > 24", bytes([ping, I.ICD_MAX_PAYLOAD + 1]) + good[2:], I.ICD_ST_BAD_LENGTH),
        ("frame shorter than LEN", bytes([ping, 3]) + good[2:], I.ICD_ST_BAD_LENGTH),
        ("PING with a payload", with_byte, I.ICD_ST_BAD_LENGTH),
        ("unknown command", build_command(0x7E), I.ICD_ST_UNKNOWN_CMD),
    ]
    for name, frame, want in cases:
        try:
            got = link.transact_raw(frame, frame[0]).status
            detail = f"-> {status_name(got)}, want {status_name(want)}"
        except (LinkError, BusError) as e:
            got, detail = None, f"-> {e}"
        report(f"bad frame rejected: {name}", got == want, detail)

    # A corrupted SET_MODE must not be executed (needs GET_STATUS on the slave).
    try:
        before = link.call(I.ICD_CMD_GET_STATUS)[I.ICD_STATUS_OFS_MODE]
    except SlaveError as e:
        skip("corrupted SET_MODE not executed", f"slave has no GET_STATUS: {e}")
    else:
        target = I.ICD_MODE_DETUMBLE if before == I.ICD_MODE_SAFE else I.ICD_MODE_SAFE
        f = build_command(I.ICD_CMD_SET_MODE, bytes([target]))
        for name, bad in (("CRC byte", f[:-1] + bytes([f[-1] ^ 0xA5])),
                          ("payload bit", f[:2] + bytes([f[2] ^ 0x01]) + f[3:])):
            link.transact_raw(bad, I.ICD_CMD_SET_MODE)
        after = link.call(I.ICD_CMD_GET_STATUS)[I.ICD_STATUS_OFS_MODE]
        report("corrupted SET_MODE not executed", after == before, f"(mode {before} -> {after})")
    report("slave still answers after bad frames", link.transact(ping).ok)


# ------------------------------------------------------------------ b) unplug

def test_unplug(link, sim):
    if sim is not None:
        sim.set_present(link.addr, False)
    else:
        input("\n>>> UNPLUG the slave's SDA wire (or its power), then press Enter ")
    fails, worst = 0, 0.0
    for _ in range(I.ICD_COMM_FAULT_THRESHOLD):
        t0 = time.monotonic()
        try:
            link.call(I.ICD_CMD_PING)
        except (LinkError, SlaveError):
            fails += 1
        worst = max(worst, time.monotonic() - t0)
    report("unplugged: transactions fail", fails == I.ICD_COMM_FAULT_THRESHOLD)
    # A failed transaction must return well inside one poll period, never hang (v0.1).
    report("unplugged: failures return quickly", worst < I.ICD_POLL_PERIOD_MS / 1000,
           f"(slowest {worst * 1000:.1f} ms, limit {I.ICD_POLL_PERIOD_MS} ms)")
    report("unplugged: COMM_FAULT raised", link.stats.comm_fault)
    if sim is not None:
        sim.set_present(link.addr, True)
    else:
        input(">>> PLUG it back in, then press Enter ")
    ok = False
    for _ in range(20):
        try:
            link.call(I.ICD_CMD_PING)
            ok = True
            break
        except (LinkError, SlaveError):
            link.bus.wait_ms(100)
    report("replugged: link recovers", ok and not link.stats.comm_fault)


# ------------------------------------------------------------------ c) soak

def test_soak(link, n):
    s = link.stats
    base = (s.busy_retries, s.read_crc_errors, s.resends)
    errors: dict = {}
    t0 = time.monotonic()
    for i in range(n):
        try:
            link.call(I.ICD_CMD_PING)
        except (LinkError, SlaveError) as e:
            errors[str(e)] = errors.get(str(e), 0) + 1
        if (i + 1) % 1000 == 0:
            print(f"   {i + 1}/{n}  errors={sum(errors.values())}", end="\r")
    print()
    dt = time.monotonic() - t0
    total = sum(errors.values())
    report(f"{n} PINGs with zero errors", total == 0,
           f"errors={total} busy_rereads={s.busy_retries - base[0]} read_crc={s.read_crc_errors - base[1]} "
           f"resends={s.resends - base[2]} ({1000 * dt / max(1, n):.2f} ms/transaction)")
    for msg, count in sorted(errors.items(), key=lambda kv: -kv[1]):
        print(f"   {count:6d}  {msg}")
    if total or s.read_crc_errors - base[1]:
        print("   -> errors seen: try the bit-banged i2c-gpio bus (ICD.md section 1, clock stretching)")


def dummy_counters(link):
    """The dummy slave's own link counters (telemetry page 0), if it is one.
    Identified by its firmware version: TT&C page 0 is also 24 bytes long."""
    try:
        v = link.call(I.ICD_CMD_GET_VERSION)
        if tuple(v[:3]) != (I.DUMMY_FW_MAJOR, I.DUMMY_FW_MINOR, I.DUMMY_FW_PATCH):
            return
        p = link.call(I.ICD_CMD_GET_TELEMETRY, bytes([0]))
    except (LinkError, SlaveError):
        return
    if len(p) == I.DUMMY_TLM0_LEN:
        g = lambda o: int.from_bytes(p[o:o + 4], "little")  # noqa: E731
        print(f"   slave counters: ok={g(I.DUMMY_TLM0_OFS_FRAMES_OK)} crc_err={g(I.DUMMY_TLM0_OFS_CRC_ERRORS)} "
              f"len_err={g(I.DUMMY_TLM0_OFS_LEN_ERRORS)} overwritten={g(I.DUMMY_TLM0_OFS_OVERWRITTEN)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bus", type=int, default=1, help="/dev/i2c-N (default 1)")
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=I.ICD_ADDR_ADCS)
    ap.add_argument("--sim-lib", help="path to libcubesat_sim.so / cubesat_sim.dll instead of hardware")
    ap.add_argument("-n", "--count", type=int, default=10000, help="PINGs for test (c)")
    ap.add_argument("--skip-unplug", action="store_true")
    a = ap.parse_args()

    sim = SimBus(a.sim_lib) if a.sim_lib else None
    bus = sim or I2CDev(a.bus)
    link = SlaveLink(bus, a.addr, f"slave@0x{a.addr:02X}")
    print(f"ICD v{I.ICD_VERSION_MAJOR}.{I.ICD_VERSION_MINOR} link test, "
          f"{'simulation' if sim else f'/dev/i2c-{a.bus}'}, address 0x{a.addr:02X}\n")
    try:
        test_basics(link, a.bus, sim)
        test_bad_frames(link)
        if not a.skip_unplug:
            test_unplug(link, sim)
        test_soak(link, a.count)
        dummy_counters(link)
    except (BusError, LinkError, SlaveError, AssertionError) as e:
        report("unexpected link error", False, str(e))
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
