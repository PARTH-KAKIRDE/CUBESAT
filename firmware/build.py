#!/usr/bin/env python3
"""
Firmware build script (no make/CMake needed, works on Windows, Linux, Pi).

  python firmware/build.py test          build + run host unit tests (gcc)
  python firmware/build.py sim           build the simulation library for the OBC tests
  python firmware/build.py check         compile-check all STM32 code against the
                                         real register headers using host gcc
  python firmware/build.py arm [target]  cross-compile: dummy_slave | adcs | ttc | all
                                         needs arm-none-eabi-gcc on PATH (or ARM_GCC=path/to/bin)

Outputs go to code/build/.
"""
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # code/
FW = ROOT / "firmware"
BUILD = ROOT / "build"

COMMON_INC = [ROOT / "icd", FW / "common"]
COMMON_SRC = [ROOT / "icd" / "icd_crc16.c", FW / "common" / "icd_link.c", FW / "common" / "icd_node.c"]
ADCS_LOGIC = [FW / "adcs" / "adcs_app.c", FW / "adcs" / "bdot.c"]
TTC_LOGIC = [FW / "ttc" / "ttc_app.c"]
HOST_SIM = [FW / "host" / "sim_api.c", FW / "host" / "adcs_hw_sim.c", FW / "host" / "ttc_hw_sim.c"]
HOST_INC = COMMON_INC + [FW / "adcs", FW / "ttc", FW / "host"]

STM32_INC = [FW / "stm32", FW / "vendor" / "cmsis" / "core", FW / "vendor" / "cmsis" / "device"]
STM32_BASE = [FW / "stm32" / f for f in ("startup_stm32f446.c", "platform.c", "i2c_slave.c", "log.c")]
TARGETS = {
    "dummy_slave": {
        "src": STM32_BASE + [FW / "dummy_slave" / "main.c"],
        "inc": [],
    },
    "adcs": {
        "src": STM32_BASE + ADCS_LOGIC + [FW / "stm32" / "i2c_master.c", FW / "adcs" / "hw_stm32.c", FW / "adcs" / "main.c"],
        "inc": [FW / "adcs"],
    },
    "ttc": {
        "src": STM32_BASE + TTC_LOGIC + [FW / "stm32" / f for f in ("i2c_master.c", "spi1.c")]
        + [FW / "ttc" / "hw_stm32.c", FW / "ttc" / "sx1278.c", FW / "ttc" / "main.c"],
        "inc": [FW / "ttc"],
    },
}
# Receive-only TT&C for bench work without an antenna (never transmits).
TARGETS["ttc_notx"] = dict(TARGETS["ttc"], defs=["-DTTC_TX_INHIBIT"])

WARN = ["-Wall", "-Wextra", "-Wshadow", "-Wno-unused-parameter"]


def run(cmd):
    print(" ".join(str(c) for c in cmd) if os.environ.get("VERBOSE") else f"  {Path(str(cmd[0])).name} ... {Path(str(cmd[-1])).name}")
    r = subprocess.run([str(c) for c in cmd])
    if r.returncode:
        sys.exit(r.returncode)


def inc(paths):
    return [f"-I{p}" for p in paths]


def host_cc():
    cc = os.environ.get("CC", "gcc")
    if not shutil.which(cc):
        sys.exit(f"host compiler '{cc}' not found")
    return cc


def cmd_test():
    out = BUILD / "host"
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("test_firmware.exe" if os.name == "nt" else "test_firmware")
    srcs = COMMON_SRC + ADCS_LOGIC + TTC_LOGIC + HOST_SIM + [FW / "host" / "test_firmware.c"]
    run([host_cc(), "-std=c11", "-O2", "-g", *WARN, "-Werror", *inc(HOST_INC), *srcs, "-lm", "-o", exe])
    print("running host tests")
    r = subprocess.run([str(exe)])
    sys.exit(r.returncode)


def sim_lib_path():
    name = {"Windows": "cubesat_sim.dll", "Darwin": "libcubesat_sim.dylib"}.get(platform.system(), "libcubesat_sim.so")
    return BUILD / "host" / name


def cmd_sim():
    lib = sim_lib_path()
    lib.parent.mkdir(parents=True, exist_ok=True)
    srcs = COMMON_SRC + ADCS_LOGIC + TTC_LOGIC + HOST_SIM
    extra = [] if os.name == "nt" else ["-fPIC"]
    run([host_cc(), "-std=c11", "-O2", "-shared", *extra, *WARN, "-Werror", *inc(HOST_INC), *srcs, "-lm", "-o", lib])
    print(f"built {lib}")


def cmd_check():
    """Syntax/type check every STM32 file with the host compiler.
    Not a substitute for an ARM build, but it catches wrong register or
    bit names, typos and type errors without the cross-compiler."""
    cc = host_cc()
    vendor_sys = []
    for p in STM32_INC[1:]:
        vendor_sys += ["-isystem", str(p)]
    n = 0
    for name, t in TARGETS.items():
        for src in t["src"]:
            run([cc, "-fsyntax-only", "-std=c11", *WARN, "-Werror", "-DSTM32F446xx", *t.get("defs", []),
                 *inc(COMMON_INC + [FW / "stm32"] + t["inc"]), *vendor_sys, src])
            n += 1
    print(f"checked {n} files OK")


def arm_tool(tool):
    base = os.environ.get("ARM_GCC")
    if not base:  # toolchain unpacked inside the repo (git-ignored)
        local = sorted((ROOT / "tools" / "arm-gcc").glob("*/bin"))
        base = str(local[-1]) if local else None
    exe = f"arm-none-eabi-{tool}" + (".exe" if os.name == "nt" else "")
    if base:
        p = Path(base) / exe
        if p.exists():
            return p
    found = shutil.which(f"arm-none-eabi-{tool}")
    if not found:
        sys.exit("arm-none-eabi-gcc not found. Install it (see firmware/README.md) or set ARM_GCC to its bin dir.")
    return found


def cmd_arm(which):
    names = list(TARGETS) if which == "all" else [which]
    cc, objcopy, size = arm_tool("gcc"), arm_tool("objcopy"), arm_tool("size")
    cpu = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard"]
    for name in names:
        if name not in TARGETS:
            sys.exit(f"unknown target {name}")
        t = TARGETS[name]
        out = BUILD / "arm" / name
        out.mkdir(parents=True, exist_ok=True)
        elf = out / f"{name}.elf"
        print(f"[{name}]")
        run([cc, *cpu, "-std=c11", "-Os", "-g3", "-ffunction-sections", "-fdata-sections",
             *WARN, "-Werror", "-DSTM32F446xx", *t.get("defs", []), *inc(COMMON_INC + STM32_INC + t["inc"]),
             *t["src"], *COMMON_SRC,
             "-T", FW / "stm32" / "stm32f446re.ld", "-nostartfiles", "--specs=nano.specs", "--specs=nosys.specs",
             "-Wl,--gc-sections", f"-Wl,-Map={out / (name + '.map')}", "-lm", "-o", elf])
        run([objcopy, "-O", "binary", elf, out / f"{name}.bin"])
        run([objcopy, "-O", "ihex", elf, out / f"{name}.hex"])
        subprocess.run([str(size), str(elf)])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    c = sys.argv[1]
    if c == "test":
        cmd_test()
    elif c == "sim":
        cmd_sim()
    elif c == "check":
        cmd_check()
    elif c == "arm":
        cmd_arm(sys.argv[2] if len(sys.argv) > 2 else "all")
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
