#!/usr/bin/env python3
"""
Program the ADCS / TT&C STM32 straight from the Raspberry Pi GPIO pins (SWD
bit-bang with OpenOCD). No ST-LINK, Nucleo or Tag-Connect cable needed: solder
thin wires to the Tag-Connect pads (J16 on ADCS, J4 on TT&C).

  Pad (both boards)      Pi header
  1  3.3V                pin 17 (3V3)  - ONLY if the board has no other power
  2  SWDIO               pin 13 (GPIO27)
  3  NRST                pin 15 (GPIO22)
  4  SWCLK               pin 22 (GPIO25)
  5  GND                 pin 20 (GND)
  6  SWO                 pin 10 (GPIO15 RX) - optional, ADCS log (swo_log.py)

Usage (on the Pi, from Software/):
  python3 OBC/scripts/swd_flash.py probe            check the chip answers (IDCODE)
  python3 OBC/scripts/swd_flash.py adcs             flash build/arm/adcs/adcs.elf
  python3 OBC/scripts/swd_flash.py ttc
  python3 OBC/scripts/swd_flash.py path/to/file.elf
  python3 OBC/scripts/swd_flash.py erase            mass erase (if the chip is stuck)
Needs:  sudo apt install openocd gpiod
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CODE = Path(__file__).resolve().parents[2]
SWCLK, SWDIO, NRST = 25, 27, 22


def gpio_chip() -> int:
    """Find the gpiochip that owns the 40-pin header (GPIO25 line)."""
    for tool in (["gpiofind", "GPIO25"], ["gpioinfo", "GPIO25"]):  # libgpiod v1 / v2
        if shutil.which(tool[0]):
            r = subprocess.run(tool, capture_output=True, text=True)
            out = r.stdout.strip()
            if r.returncode == 0 and out.startswith("gpiochip"):
                return int(out.split()[0].replace("gpiochip", "").rstrip(":"))
    return 0


def config() -> str:
    c = gpio_chip()
    return f"""
adapter driver linuxgpiod
adapter gpio swclk {SWCLK} -chip {c}
adapter gpio swdio {SWDIO} -chip {c}
adapter gpio srst {NRST} -chip {c} -open-drain
transport select swd
reset_config srst_only srst_open_drain connect_assert_srst
source [find target/stm32f4x.cfg]
"""


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    if not shutil.which("openocd"):
        print("openocd not found: sudo apt install openocd gpiod")
        return 1
    what = sys.argv[1]
    if what == "probe":
        cmds = ["init", "targets", "reset halt", "flash probe 0", "reset run", "shutdown"]
    elif what == "erase":
        cmds = ["init", "reset halt", "stm32f4x mass_erase 0", "shutdown"]
    else:
        elf = CODE / "build" / "arm" / what / f"{what}.elf" if what in ("adcs", "ttc", "dummy_slave") else Path(what)
        if not elf.exists():
            print(f"{elf} not found (build it with: python3 firmware/build.py arm {what})")
            return 1
        cmds = [f"program {elf.as_posix()} verify reset exit"]
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as f:
        f.write(config())
        cfg = f.name
    args = ["openocd", "-f", cfg]
    for c in cmds:
        args += ["-c", c]
    print(" ".join(args))
    return subprocess.run(args).returncode


if __name__ == "__main__":
    sys.exit(main())
