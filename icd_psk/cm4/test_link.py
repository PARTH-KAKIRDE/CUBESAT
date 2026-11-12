#!/usr/bin/env python3
"""ICD link tests against the dummy slave (run on the CM4).

  python3 test_link.py ping                 # one PING + GET_VERSION
  python3 test_link.py badcrc               # test (a): corrupted frames are rejected
  python3 test_link.py unplug               # test (b): unplug, see COMM_FAULT, replug, recover
  python3 test_link.py soak --count 10000   # test (c): zero errors required

Options: --bus N (default 1), --addr 0x32 (default ADCS address).
Every run starts with the CRC self-test.
"""
import argparse
import sys
import time

from icd_link import (ICD, STATUS_NAMES, LinkError, Monitor, Slave, build_cmd,
                      bus_speed_hz, crc_selftest)


def st(code):
    return STATUS_NAMES.get(code, f"0x{code:02X}")


def check_speed(bus):
    hz = bus_speed_hz(bus)
    if hz is None:
        print(f"bus speed: unknown (check dtparam=i2c_arm_baudrate={ICD['I2C_SPEED_HZ']})")
    else:
        flag = "" if hz == ICD["I2C_SPEED_HZ"] else f"  <-- ICD requires {ICD['I2C_SPEED_HZ']}"
        print(f"bus speed: {hz} Hz{flag}")


def cmd_ping(s, a):
    status, _ = s.transact(ICD["CMD_PING"])
    print(f"PING: {st(status)}")
    status, p = s.transact(ICD["CMD_GET_VERSION"])
    if status == ICD["ST_OK"] and len(p) == ICD["GET_VERSION_RESP_LEN"]:
        print(f"GET_VERSION: fw {p[0]}.{p[1]}.{p[2]}, ICD {p[3]}.{p[4]}")
        if (p[3], p[4]) != (ICD["VERSION_MAJOR"], ICD["VERSION_MINOR"]):
            print("  WARNING: slave built against a different icd.h")
    else:
        print(f"GET_VERSION: {st(status)} {p.hex()}")
    return status == ICD["ST_OK"]


def cmd_badcrc(s, a):
    ping = ICD["CMD_PING"]
    good = build_cmd(ping)
    cases = [
        ("flipped CRC byte", good[:-1] + bytes([good[-1] ^ 0xFF]), ICD["ST_BAD_CRC"]),
        ("flipped payload bit", build_cmd(ping, b"\x00")[:2] + b"\x01" + build_cmd(ping, b"\x00")[3:],
         ICD["ST_BAD_CRC"]),
        ("LEN too big", bytes([ping, 25]) + good[2:], ICD["ST_BAD_LENGTH"]),
        ("frame shorter than LEN", bytes([ping, 3]) + good[2:], ICD["ST_BAD_LENGTH"]),
        ("PING with payload", build_cmd(ping, b"\x00"), ICD["ST_BAD_LENGTH"]),
        ("unknown command", build_cmd(0x7E), ICD["ST_UNKNOWN_CMD"]),
    ]
    ok = True
    for name, frame, want in cases:
        try:
            status, _ = s.transact_raw(frame, frame[0])
        except LinkError as e:
            status, err = None, e
        res = "PASS" if status == want else "FAIL"
        ok &= status == want
        print(f"  {res}  {name}: got {st(status) if status is not None else err}, want {st(want)}")
    status, _ = s.transact(ping)          # slave must still work afterwards
    print(f"  {'PASS' if status == ICD['ST_OK'] else 'FAIL'}  PING after bad frames: {st(status)}")
    ok &= status == ICD["ST_OK"]
    print("test (a):", "PASS" if ok else "FAIL")
    return ok


def cmd_unplug(s, a):
    """Poll PING at the ICD rate; expect ok -> COMM_FAULT -> recovered."""
    events = []
    mon = Monitor(f"slave 0x{a.addr:02X}", log=lambda m: (print("  " + m), events.append(m)))
    print("Polling at 1 Hz. Unplug the slave (SDA/SCL or power), wait for COMM_FAULT, plug it back in.")
    worst = 0.0
    saw_fault = False
    t_end = time.monotonic() + a.timeout
    while time.monotonic() < t_end:
        t0 = time.monotonic()
        try:
            status, _ = s.transact(ICD["CMD_PING"])
            if status != ICD["ST_OK"]:
                raise LinkError(st(status))
            was_fault = mon.comm_fault
            mon.ok()
            if was_fault:
                saw_fault = True
                break
        except LinkError as e:
            mon.failed(e)
        dt = time.monotonic() - t0
        worst = max(worst, dt)
        time.sleep(max(0, ICD["POLL_PERIOD_MS"] / 1000 - dt))
    print(f"  slowest transaction: {worst * 1000:.1f} ms")
    # A failed transaction must return well inside one poll period, never hang.
    ok = saw_fault and worst < ICD["POLL_PERIOD_MS"] / 1000
    print("test (b):", "PASS" if ok else f"FAIL ({'no fault+recovery seen' if not saw_fault else 'slow'})")
    return ok


def cmd_soak(s, a):
    errors = {}
    t0 = time.monotonic()
    for i in range(a.count):
        try:
            status, _ = s.transact(ICD["CMD_PING"])
            if status != ICD["ST_OK"]:
                raise LinkError(f"status {st(status)}")
        except LinkError as e:
            key = str(e)
            errors[key] = errors.get(key, 0) + 1
        if (i + 1) % 1000 == 0:
            print(f"  {i + 1}/{a.count}, errors so far: {sum(errors.values())}")
    dt = time.monotonic() - t0
    total = sum(errors.values())
    print(f"{a.count} PINGs in {dt:.1f} s, {total} errors")
    for k, v in sorted(errors.items(), key=lambda kv: -kv[1]):
        print(f"  {v:6d}  {k}")
    if total:
        print("  Dropped frames: try the i2c-gpio overlay (see README, clock stretching).")
    print("test (c):", "PASS" if total == 0 else "FAIL")
    return total == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("test", choices=["ping", "badcrc", "unplug", "soak"])
    ap.add_argument("--bus", type=int, default=1)
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=ICD["ADDR_ADCS"])
    ap.add_argument("--count", type=int, default=10000)
    ap.add_argument("--timeout", type=float, default=120, help="unplug test time limit, s")
    a = ap.parse_args()

    crc_selftest()
    print(f"CRC self-test: PASS (0x{ICD['CRC_CHECK']:04X})")
    check_speed(a.bus)
    s = Slave(a.bus, a.addr)
    try:
        ok = {"ping": cmd_ping, "badcrc": cmd_badcrc,
              "unplug": cmd_unplug, "soak": cmd_soak}[a.test](s, a)
    except LinkError as e:
        print(f"link error: {e}")
        ok = False
    finally:
        s.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
