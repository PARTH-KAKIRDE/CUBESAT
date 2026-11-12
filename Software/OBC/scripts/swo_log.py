#!/usr/bin/env python3
"""
Show the STM32 debug log on the Raspberry Pi.

  ADCS (SWO, pad 6 of J16 -> Pi pin 10 GPIO15 RX):
      python3 OBC/scripts/swo_log.py --port /dev/serial0
  TT&C (UART J6 pin 2 TX -> Pi pin 10 GPIO15 RX):
      python3 OBC/scripts/swo_log.py --port /dev/serial0 --raw

The ADCS firmware sends its log as ITM packets on SWO at 115200 baud: every
character arrives as 0x01 <char>. This script strips the 0x01 headers.
Needs the Pi UART enabled with no login console on it (docs/NO_PROGRAMMER.md).
"""
import argparse
import sys

import serial  # sudo apt install python3-serial


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/serial0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--raw", action="store_true", help="plain UART text (TT&C), no ITM decoding")
    a = ap.parse_args()
    s = serial.Serial(a.port, a.baud, timeout=0.5)
    print(f"reading {a.port} at {a.baud} ({'raw' if a.raw else 'ITM/SWO'}), Ctrl+C to stop", file=sys.stderr)
    expect_data = False
    try:
        while True:
            for b in s.read(256):
                if a.raw:
                    ch = b
                elif expect_data:
                    expect_data = False
                    ch = b
                elif b == 0x01:  # ITM stimulus port 0, 1-byte payload
                    expect_data = True
                    continue
                else:
                    continue  # sync / overflow / other packets
                if ch in (9, 10) or 32 <= ch < 127:
                    sys.stdout.write(chr(ch))
            sys.stdout.flush()
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
