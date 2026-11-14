# Changelog

## 2026-11-14

### Layout
- **OBC split by subsystem:** `OBC/EPS/EPS.py`, `OBC/Payload/Payload.py`, `OBC/comms/` (`bus.py`, `slaves.py`, `ground.py`). The commands changed to `python3 OBC/EPS/EPS.py` and `python3 OBC/Payload/Payload.py`; `python3 -m OBC` is unchanged.
- **Firmware ported** from the base repo into `firmware/`: build system, shared STM32 code, TT&C, Nucleo dummy slave, PC simulation and its tests. Everything builds against `ICD/` (the base repo's `firmware/common/` was identical to `ICD/slave/` and is not copied).
- **ADCS has its own folder,** `ADCS/`, like `OBC/` and `ICD/`.
- **`docs/` rewritten** for this repo from the base repo's docs: `RPI_SETUP.md`, `WIRING.md`, `BRINGUP.md`, `NO_PROGRAMMER.md`. Personal paths and the Pi's IP address were left out, and the SWO baud rate is corrected (115200, not 2 MHz).
- **New:** `README.md` (overview, all build and test commands) and `ADCS/README.md` (how ADCS works).
- The 22 OBC simulation tests that used to be skipped now run (`python3 firmware/build.py sim`).

The ADCS and TT&C firmware came from the base repo (`~/Downloads/code.zip`); nobody on the team wrote it. It was reviewed line by line, and the fixes below came out of that review.

### Shared STM32 code (both boards)
- **Watchdog:** started before it is configured (ST's order). The old order waited on IWDG flags that only clear once the watchdog clock runs, so it could hang at boot.
- **I2C slave:** a second end-of-transfer after a read (STOP after the master's NACK) could re-queue the previous command and run it twice. Fixed in two places: `stm32/i2c_slave.c` tracks the transfer direction, and `ICD/slave/icd_link.c` clears the receive buffer when a read starts.
- **I2C master:** the per-wait timeout went from 5 to 2 ms, so failed sensor reads cannot hold the main loop past the OBC's 20 ms reply window.
- TT&C fw 0.2.1 (shared fixes only; TT&C's own code is not reviewed yet).

### ADCS
- **fw 0.3.0, ICD v0.5: gyro zero-rate offset.** MPU-6050 modules read a few °/s at rest, which could keep the rate above the 2 °/s detumbled limit forever.
  - ADCS measures the offset for 5 s after every boot and keeps it only if the board was still (gyro and raw magnetometer steady).
  - `SET_GYRO_OFFSET` (0x26) overrides it, and telemetry page 3 reports it.
  - The OBC saves the bench value (`adcs_gyro_offset.json`) and re-sends it when ADCS boots tumbling in orbit.
- **fw 0.3.0, recovery:** automatic recovery from fault-SAFE waits only for the magnetometer and coil drivers (a dead gyro or sun ADC no longer blocks it). A fault that returns right after recovery doubles the wait, from 30 s up to 32 min. A commanded mode change switches the coils off at once.
- **fw 0.4.0, ICD v0.6: sun sensors.** ADCS turns its BPW34 readings into a Sun direction (body frame) and a sunlit/eclipse flag, on telemetry page 4. `SET_SUN_CONFIG` (0x27) sets full scale and the lit (albedo) threshold. The EPS eclipse decision uses this flag (older firmware: raw values).
- **fw 0.5.0, ICD v0.7: a sun diode on every face.** With no pointing control a bare face reads as eclipse whenever it faces the Sun (about 5 % of directions).
  - Of each opposite pair only the brighter face is used, so albedo on the far face is ignored.
  - Gyro tracking of the Sun backs up a diode that fails in orbit. Missing light identifies the dead diode (fault `SUN_DIODE`), and the Sun behind it is reported HIDDEN (unknown, not eclipse).
  - `SET_SUN_DISABLED` (0x28) lets the ground switch faces off.
  - A 5-face build remains (`-DSUN_NO_MZ`, target `adcs_5face`).
- The sun sensors (BPW34, attitude) and the payload's TSL2591 (solar flux, science) are separate systems.

### Tests (all on PC)

| Test | Before | Now |
|---|---|---|
| Firmware checks | 91 | 1354 (1353 in the 5-face build) |
| STM32 compile check | 34 files | 43 files |
| OBC tests | 102 (22 skipped) | 114 (none skipped with the simulation built) |
| ICD tests | 47 | 52 |
| Link test | — | 15/15 against both simulated boards |

Each fix was also checked by putting the bug back and confirming a test fails.

### Not done yet
- **ARM builds:** not run (no ARM compiler on the development PC).
- **Untested driver code:** the drivers that only run on the chip (watchdog, I2C slave) are compile-checked only. Check them on the first flash.
- **Bench values** (`docs/BRINGUP.md`):
  - sun-sensor face mapping, full scale and lit threshold;
  - gyro offset;
  - axis map and coil polarity;
  - magnetometer hard-iron offset.
- **TT&C review:** still to do.
- **Ground tool:** `gs_tool.py` and the LilyGO bridge are still in the base repo.
