# CLAUDE.md — CubeSat software

A 2U CubeSat **ground prototype**:
- **OBC:** Raspberry Pi 4B running Python. It is the I2C master.
- **ADCS:** STM32F446 slave at 0x32. Magnetorquers and B-dot detumbling.
- **TT&C:** STM32F446 slave at 0x33. SX1278 LoRa radio.
- **EPS:** no MCU; the Pi drives the INA226 and the pyro GPIO directly.

The PCBs (KiCad, in `../ADCS`, `../TTC`, `../EPS`) are already manufactured. **The software must fit the hardware as built**; no board changes, only wire patches.

All software lives in this `code/` directory. Keep it here.

## Rules

- **`icd/cubesat_icd.h` and `icd/ground_link.h` are the single source of truth.** They hold addresses, command IDs, status codes, modes, flag bits and telemetry offsets.
  - Never redefine these locally.
  - After editing a header, run `python tools/gen_icd_py.py` (it regenerates `obc/cubesat_obc/icd_defs.py`; never edit that file by hand). Then update `icd/ICD.md`.
  - Bump `ICD_VERSION_MINOR` for compatible changes, `MAJOR` for breaking ones. Changes go through Parth.
  - Headers contain only `#define NAME <integer>` lines; the generator stops with an error on any other `#define`.
  - Ground commands are authenticated (HMAC + counter, `ICD.md` section 11). The key file is a secret: never commit it.
- **No floats or structs on the wire.** Pack field by field with `icd_pack.h` (C) or `struct` (Python), little-endian.
- **Firmware app logic stays portable.** `adcs_app.c`, `bdot.c` and `ttc_app.c` touch hardware only through `adcs_hw.h` / `ttc_hw.h`. Register code goes in `hw_stm32.c` / `firmware/stm32/`.
- **Keep the I2C slave interrupt to byte copying.** The main loop must not block for more than about 10 ms, or the Pi's BUSY retries (20 ms total) run out.
- **Run the tests after any change** (below). The simulation runs the real firmware logic, so it is the main safety net until the hardware is up.

## Commands (run from `code/`)

```
python tools/gen_icd_py.py [--check]
python firmware/build.py check      # STM32 files vs real CMSIS headers (host gcc, -Werror)
python firmware/build.py test       # C unit tests via the simulated bus
python firmware/build.py sim        # build/host/cubesat_sim.dll (the OBC tests also rebuild it if stale)
python firmware/build.py arm all    # -> build/arm/<target>/*.elf|.bin|.hex
cd obc && python -m pytest -q
python obc/hw_tests/link_test.py --sim         # on the Pi: --bus 1 --addr 0x32
python -m cubesat_obc --sim run --ticks 20     # from obc/
python ground/gs_tool.py --sim cmd obc hk
python ground/gs_tool.py keygen               # ground-command key (once; copy it to the Pi)
```

## Hardware facts (from the KiCad files; verify against them before changing pins)

- **ADCS:**
  - OBC link: I2C3 PA8/PC9 (J11). Sensor bus: I2C1 PB6/PB7, MPU-6050 at 0x68 (J12), MMC5983MA at 0x30 (J10).
  - Coils: X = TIM8 PC6/PC7, Y = TIM3 PA6/PA7, Z = TIM1 PA9/PA10. nSLEEP on PB2/PC13/PC14, nFAULT on PB13-15.
  - Sun sensors: PA0-5, ADC1 channels 0-5.
  - No crystal. **AISEN goes to ground through a 1 Ω sense resistor (DRV8833 current limit, about 200 mA) and is not connected to the STM32**, so coil current cannot be measured.
  - Logs go out on SWO/ITM; there is no free UART.
- **TT&C:**
  - OBC link: I2C3 PA8/PC9 (J5). UART log: USART1 PA9/PA10 (J6). Radio: SPI1 + NSS PA4, RESET PC4.
  - TMP102 at 0x48 on I2C1. Deploy pins: DEPLOY_SW PC0, RBF_IN PC1, BURN_EN PC2. LED on PC11.
  - **Has** SWD on J4.
- **EPS:**
  - INA226 module only (0x40). J6 = +5V / GND / SDA / SCL / PYRO_FIRE.
  - Pyro is gated by the J26 plug plus a Pi GPIO (GPIO17). Board TODOs are in `../EPS/CubeSat_EPS/EPS_Rev1.1_Changes.md`.
- **Pi:** link on i2c-1 (GPIO2/3). Sensors on i2c-3 (`dtoverlay=i2c3,pins_4_5`). Fallback for clock-stretching errors: i2c-gpio bus 11 on GPIO23/24.
- **Bench values not yet verified** (see docs/BRINGUP.md):
  - axis map and coil polarity (`firmware/adcs/board_config.h`)
  - deploy pin polarity (`firmware/ttc/ttc_app.h`)
  - INA226 shunt value (`obc/config/obc.toml`)
  - LoRa frequency: 436.5 MHz is a placeholder.

## Current hardware (2026-11-05)

The user has only the Pi 4B and the soldered boards: no ST-LINK, Nucleo, Tag-Connect cable or antenna. So:
- The Pi programs the STM32s by SWD bit-bang (`obc/scripts/swd_flash.py`, GPIO25/27/22).
- ADCS logs on SWO as NRZ 115200, read on the Pi UART with `obc/scripts/swo_log.py`.
- Use the `ttc_notx` build (never transmits) until an antenna is fitted.

See `docs/NO_PROGRAMMER.md`.

## Environment notes

- Windows PC with MSYS2 gcc 15 and Python 3.13 (pytest installed). No make or CMake: `build.py` replaces them.
- ARM toolchain: xPack arm-none-eabi-gcc 13.3.1, unpacked in `tools/arm-gcc/` (git-ignored). `build.py` finds it automatically; `ARM_GCC=<bin dir>` overrides.
- Pi access details are in `RASPBERRY_PI_GUIDE.md` (git-ignored; it contains the password). Last seen at 10.101.243.47, but it was not reachable on 2026-11-05.
