# STM32 firmware: build system, shared STM32 code, dummy slave, PC simulation

The ADCS and TT&C applications live in their own folders, [`../ADCS`](../ADCS/README.md) and [`../TTC`](../TTC/README.md); both are built from here with the shared code below.

Register-level C on the CMSIS headers (`vendor/cmsis`, Arm/ST Apache-2.0). It doesn't use the HAL or CubeMX, and `build.py` is the whole build system.

Ported from the base repo (`~/Downloads/code/firmware`) on 2026-11-14 and built against `../ICD` instead of the base repo's ICD v0.2 copy (v0.2 → v0.4 only added constants, so the port needed no code change). Since then, after a full review:

- **ADCS fw 0.5.0 (ICD v0.7):** a sun diode on all six faces; brighter-of-pair (albedo on the far face ignored); gyro tracking of the Sun with dead-diode detection and HIDDEN instead of a false eclipse; `SET_SUN_DISABLED`. Build target `adcs_5face` and `FW_DEFS="-DSUN_NO_MZ"` cover the 5-face variant.
- **ADCS fw 0.4.0 (ICD v0.6):** sun direction and sunlit/eclipse from the BPW34 sun sensors, page 4, `SET_SUN_CONFIG`.
- **ADCS fw 0.3.0 (ICD v0.5):**
  - Gyro zero-rate offset: measured for 5 s after every boot and kept only if the board was still (gyro and magnetometer steady). `SET_GYRO_OFFSET` (0x26) overrides it, page 3 reports it, and page 0 gyro has it removed. Without it a few °/s of MPU-6050 offset kept the rate above the 2 °/s limit, so DETUMBLED never came.
  - Automatic recovery from fault-SAFE waits only for the magnetometer and coil drivers; a dead gyro or sun-sensor ADC no longer keeps ADCS in SAFE.
  - A fault that returns soon after an automatic recovery (a shorted coil driver) doubles the wait each time, 30 s up to 32 min.
  - A commanded mode change switches the coils off at once.
- **Shared STM32 code (both boards; TT&C fw 0.2.1):**
  - `stm32/platform.c`: the watchdog is started before it is configured (ST's order). The old code waited on IWDG status flags that only clear once the watchdog clock runs, so it could hang at boot.
  - `stm32/i2c_slave.c` and `../ICD/slave/icd_link.c`: a second end-of-transfer after a read can no longer re-queue and re-run the previous command.
  - `stm32/i2c_master.c`: per-wait timeout 5 → 2 ms, so failed sensor transactions cannot hold the main loop past the OBC's reply window.

- **TT&C fw 0.4.2 (ICD v0.9):** moved to `../TTC`; see [../TTC/README.md](../TTC/README.md) and the CHANGELOG (automatic deployment, emergency commands, long packets, review fixes).

```
../ADCS/    ADCS application: adcs_app (modes, B-dot, gyro offset, faults, telemetry), bdot, hw_stm32, main
../TTC/     TT&C application: ttc_app (radio, queues, beacons, deploy, emergency), ttc_nv, hmac_sha256, sx1278, hw_stm32, main
stm32/       startup, linker script, clock/tick/watchdog, I2C3 slave, I2C1 master, SPI1, log
dummy_slave/ Nucleo-F446RE test slave (deliverable 4)
host/        PC simulation of the hardware + unit tests + shared library for the OBC tests
vendor/      CMSIS core and STM32F446 device headers
```

The ICD parts come from `../ICD` and are never copied here:
- `cubesat_icd.h`, `ground_link.h`, `icd_pack.h`, `icd_crc16.c/.h`;
- `slave/icd_link` (I2C framing, ISR <-> main loop) and `slave/icd_node` (common commands, modes, flags), which the base repo kept in `firmware/common/`.

The `*_app.c` files are portable. They reach hardware only through `adcs_hw.h` / `ttc_hw.h`, so the same code runs on the STM32 and in the PC simulation.

## Commands (from `Software/`)

```
python3 firmware/build.py test        # host unit tests (needs gcc): 1354 checks
FW_DEFS="-DSUN_NO_MZ" python3 firmware/build.py test    # same, 5-face variant (no -Z diode): 1353
python3 firmware/build.py sim         # build/host/libcubesat_sim.so for the OBC and ICD link tests
python3 firmware/build.py check       # compile-check every STM32 file against the real headers
python3 firmware/build.py arm all     # real firmware -> build/arm/<target>/<target>.elf/.bin/.hex
                                      # targets: adcs, adcs_5face, ttc, ttc_notx, dummy_slave
```

Outputs go to `Software/build/` (git-ignored). After `sim`, the OBC tests find the library on their own (`python3 -B -m pytest -q OBC/tests`), and so does the link test (`python3 ICD/python/test_link.py --sim-lib build/host/libcubesat_sim.so`).

## ARM compiler

Install one of these:

- **Raspberry Pi / Linux:** `sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi`
- **Windows:** `winget install Arm.GnuArmEmbeddedToolchain`, or download the xPack arm-none-eabi-gcc zip. Then set `ARM_GCC=<path>\bin`.
- **Unpacked toolchain:** put it in `Software/tools/arm-gcc/<name>/bin` (git-ignored) and `build.py` finds it.

## Flashing

| Board | How |
|---|---|
| Nucleo | Copy `dummy_slave.bin` onto the NOD_F446RE USB drive. |
| ADCS / TT&C | From the Pi without a programmer: `python3 OBC/scripts/swd_flash.py adcs` (or `ttc`). With an ST-LINK on the Tag-Connect: `openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program build/arm/adcs/adcs.elf verify reset exit"`, or STM32CubeProgrammer. |

## Design notes

- **Clock:** HSI 16 MHz, no PLL. It's enough for everything here and avoids clock-tree bugs.
- **I2C slave interrupt:** priority 0. It only copies bytes. The main loop must never block for more than about 10 ms, so the Pi's 5 ms + 3 × 5 ms retry window always gets an answer. Slow sensor resets happen only at boot.
- **Watchdog:** the IWDG is set to 2 s (4 s on TT&C) and kicked once per main-loop pass.
- **Bench checks:** sensor axes and coil polarity are set in `../ADCS/board_config.h`. Deploy pin polarity is set in `../TTC/ttc_app.h`. The bring-up procedure is in [../docs/BRINGUP.md](../docs/BRINGUP.md); flashing from the Pi without a programmer is in [../docs/NO_PROGRAMMER.md](../docs/NO_PROGRAMMER.md).
