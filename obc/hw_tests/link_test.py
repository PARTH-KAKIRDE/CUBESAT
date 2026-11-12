#!/usr/bin/env python3
"""
ICD link acceptance tests (deliverables 4 + 5) against the Nucleo dummy slave
or a real ADCS / TT&C board.

On the Pi:      python3 hw_tests/link_test.py --addr 0x32 --bus 1
On this PC:     python  hw_tests/link_test.py --sim       (firmware simulation)

Tests:
  0) CRC-16 check value (0x29B1) and GET_VERSION
  a) a frame with a corrupted CRC is rejected (status BAD_CRC, never executed)
  b) unplugged slave: transactions fail -> COMM_FAULT; replugged -> recovers
     (interactive on hardware; automatic in --sim)
  c) 10,000 PING transactions with zero errors

If (c) shows errors or many BUSY re-reads on the Pi, switch the link to the
bit-banged i2c-gpio overlay (Raspberry Pi clock-stretching problem); see
docs/RPI_SETUP.md section "Clock stretching".
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from cubesat_obc import icd_defs as I  # noqa: E402
from cubesat_obc.bus import BusError, LinuxI2CBus, SimBus, SimLib  # noqa: E402
from cubesat_obc.frames import build_command, crc16, parse_response, status_name  # noqa: E402
from cubesat_obc.link import LinkError, SlaveError, SlaveLink  # noqa: E402

results = []


def report(name, ok, detail=""):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}")


def test_crc_and_version(link):
    report("crc16 check value", crc16(b"123456789") == 0x29B1, f"(0x{crc16(b'123456789'):04X})")
    p = link.call(I.ICD_CMD_GET_VERSION)
    ok = len(p) == 5 and p[3] == I.ICD_VERSION_MAJOR
    report("GET_VERSION", ok, f"fw {p[0]}.{p[1]}.{p[2]} ICD {p[3]}.{p[4]}")


def test_bad_crc(link):
    bus, addr = link.bus, link.addr
    before = link.call(I.ICD_CMD_GET_STATUS)[0]
    # Ask for a mode the slave is NOT in, so executing it would be visible.
    target = I.ICD_MODE_DETUMBLE if before == I.ICD_MODE_SAFE else I.ICD_MODE_SAFE
    for name, corrupt in (("CRC byte flipped", lambda f: f[:-1] + bytes([f[-1] ^ 0xA5])),
                          ("payload bit flipped", lambda f: f[:2] + bytes([f[2] ^ 0x01]) + f[3:])):
        frame = corrupt(build_command(I.ICD_CMD_SET_MODE, bytes([target])))
        bus.write(addr, frame)
        bus.wait_ms(I.ICD_TURNAROUND_MS)
        r = None
        for _ in range(4):
            r = parse_response(bus.read(addr, I.ICD_RESP_FRAME_LEN))
            if r.status != I.ICD_ST_BUSY:
                break
            bus.wait_ms(I.ICD_BUSY_RETRY_MS)
        report(f"corrupted frame rejected ({name})", r.status == I.ICD_ST_BAD_CRC, f"-> {status_name(r.status)}")
    after = link.call(I.ICD_CMD_GET_STATUS)[0]
    report("corrupted SET_MODE was not executed", after == before, f"(mode {before} -> {after})")


def test_unplug(link, sim: SimLib | None):
    if sim is not None:
        sim.lib.sim_set_present(link.addr, 0)
    else:
        input("\n>>> UNPLUG the slave's SDA wire (or its power), then press Enter ")
    fails = 0
    t0 = time.monotonic()
    for _ in range(3):
        try:
            link.call(I.ICD_CMD_PING)
        except (LinkError, SlaveError):
            fails += 1
    dt = time.monotonic() - t0
    report("unplugged: transactions fail and time out", fails == 3, f"({dt * 1000:.0f} ms for 3)")
    report("unplugged: COMM_FAULT raised", link.stats.comm_fault)
    if sim is not None:
        sim.lib.sim_set_present(link.addr, 1)
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


def test_10k(link, n):
    s0 = (link.stats.busy_retries, link.stats.read_crc_errors, link.stats.resends)
    errors = 0
    t0 = time.monotonic()
    for i in range(n):
        try:
            link.call(I.ICD_CMD_PING)
        except (LinkError, SlaveError) as e:
            errors += 1
            if errors <= 10:
                print(f"   #{i}: {e}")
        if (i + 1) % 1000 == 0:
            print(f"   {i + 1}/{n}  errors={errors}", end="\r")
    dt = time.monotonic() - t0
    busy = link.stats.busy_retries - s0[0]
    crc = link.stats.read_crc_errors - s0[1]
    resend = link.stats.resends - s0[2]
    print()
    report(f"{n} PINGs with zero errors", errors == 0,
           f"errors={errors} busy_rereads={busy} read_crc={crc} resends={resend} "
           f"({1000 * dt / n:.2f} ms/transaction)")
    if errors or crc:
        print("   -> errors seen: try the i2c-gpio overlay (docs/RPI_SETUP.md, 'Clock stretching')")


def dummy_counters(link):
    """If talking to the Nucleo dummy, print its own link counters."""
    try:
        p = link.call(I.ICD_CMD_GET_TELEMETRY, bytes([0]))
    except (LinkError, SlaveError):
        return
    if len(p) == I.DUMMY_TLM0_LEN:
        g = lambda o: int.from_bytes(p[o:o + 4], "little")  # noqa: E731
        print(f"   slave counters: ok={g(I.DUMMY_TLM0_OFS_FRAMES_OK)} crc_err={g(I.DUMMY_TLM0_OFS_CRC_ERRORS)} "
              f"len_err={g(I.DUMMY_TLM0_OFS_LEN_ERRORS)} overwritten={g(I.DUMMY_TLM0_OFS_OVERWRITTEN)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bus", type=int, default=1, help="/dev/i2c-N (default 1)")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=I.ICD_ADDR_ADCS)
    ap.add_argument("--sim", action="store_true", help="run against the PC firmware simulation")
    ap.add_argument("-n", type=int, default=10000)
    ap.add_argument("--skip-unplug", action="store_true")
    a = ap.parse_args()

    sim = None
    if a.sim:
        sim = SimLib()
        sim.init(7)
        bus = SimBus(sim)
    else:
        bus = LinuxI2CBus(a.bus)
    link = SlaveLink(bus, a.addr, f"slave@0x{a.addr:02X}")
    print(f"ICD v{I.ICD_VERSION_MAJOR}.{I.ICD_VERSION_MINOR} link test, "
          f"{'simulation' if sim else f'/dev/i2c-{a.bus}'}, address 0x{a.addr:02X}\n")
    try:
        test_crc_and_version(link)
        test_bad_crc(link)
        if not a.skip_unplug:
            test_unplug(link, sim)
        test_10k(link, a.n)
        dummy_counters(link)
    except (BusError, LinkError, SlaveError) as e:
        report("unexpected link error", False, str(e))
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
