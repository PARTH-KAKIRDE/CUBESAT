# STM32 firmware (ADCS, TT&C, dummy slave)

Register-level C on the CMSIS headers (`vendor/cmsis`, Arm/ST Apache-2.0). It doesn't use the HAL or CubeMX, and `build.py` is the whole build system.

```
common/      icd_link (I2C framing, ISR <-> main loop), icd_node (common commands, modes, flags)
stm32/       startup, linker script, clock/tick/watchdog, I2C3 slave, I2C1 master, SPI1, log
adcs/        adcs_app (modes, B-dot, faults, telemetry), bdot, hw_stm32 (sensors, PWM, ADC), main
ttc/         ttc_app (queues, beacons, burn wire), sx1278 (LoRa), hw_stm32, main
dummy_slave/ Nucleo-F446RE test slave (deliverable 4)
host/        PC simulation of the hardware + unit tests + DLL for the OBC tests
```

The `*_app.c` files are portable. They reach hardware only through `adcs_hw.h` / `ttc_hw.h`, so the same code runs on the STM32 and in the PC simulation.

## Commands

```
python firmware/build.py test        # host unit tests (needs gcc)
python firmware/build.py sim         # build build/host/cubesat_sim.dll for the OBC tests
python firmware/build.py check       # compile-check every STM32 file against the real headers
python firmware/build.py arm all     # real firmware -> build/arm/<target>/<target>.elf/.bin/.hex
```

## ARM compiler

Install one of these:

- **Windows:** `winget install Arm.GnuArmEmbeddedToolchain`, or download the xPack arm-none-eabi-gcc zip. Then set `ARM_GCC=<path>\bin`.
- **Raspberry Pi / Linux:** `sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi`

## Flashing

| Board | How |
|---|---|
| Nucleo | Copy `dummy_slave.bin` onto the NOD_F446RE USB drive. |
| ADCS / TT&C | ST-LINK on the Tag-Connect: `openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program build/arm/adcs/adcs.elf verify reset exit"`, or STM32CubeProgrammer. |

## Design notes

- **Clock:** HSI 16 MHz, no PLL. It's enough for everything here and avoids clock-tree bugs.
- **I2C slave interrupt:** priority 0. It only copies bytes. The main loop must never block for more than about 10 ms, so the Pi's 5 ms + 3 × 5 ms retry window always gets an answer. Slow sensor resets happen only at boot.
- **Watchdog:** the IWDG is set to 2 s (4 s on TT&C) and kicked once per main-loop pass.
- **Bench checks:** sensor axes and coil polarity are set in `adcs/board_config.h` (see docs/BRINGUP.md). Deploy pin polarity is set in `ttc/ttc_app.h`.
