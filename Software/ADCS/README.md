# ADCS firmware (STM32F446, fw 0.5.0, ICD v0.7)

Attitude control board: it stops the satellite tumbling after deployment (B-dot detumbling with three magnetorquers), works out where the Sun is from its sun sensors (and so whether the satellite is in eclipse), and serves all of it to the OBC over I2C (address 0x32). Register-level C, no HAL.

Build, test and flash from `Software/` (the build system and the shared STM32 code are in `../firmware`):

```
python3 firmware/build.py arm adcs        # -> build/arm/adcs/adcs.elf (.bin, .hex)
python3 firmware/build.py test            # host unit tests, ADCS logic in the simulator
python3 OBC/scripts/swd_flash.py adcs     # flash from the Pi (docs/NO_PROGRAMMER.md)
python3 -m OBC tlm adcs                   # read all telemetry pages from the Pi
```

## Files

| File | What |
|---|---|
| `adcs_app.c/.h` | The application: modes, control cycle, gyro offset, faults, ICD commands and telemetry. **Portable**: no registers, only `adcs_hw.h`. |
| `bdot.c/.h` | The B-dot law (pure maths, unit-tested). |
| `adcs_hw.h` | The hardware interface the app uses. Two implementations: `hw_stm32.c` (the board) and `../firmware/host/adcs_hw_sim.c` (a rigid-body simulation for PC tests). |
| `hw_stm32.c` | The board: MPU-6050 gyro and MMC5983MA magnetometer on I2C1, three DRV8833 coil drivers on TIM8/TIM3/TIM1 PWM, six sun-sensor channels (BPW34 photodiodes via MCP6001 transimpedance amplifiers) on ADC1. Pin map checked against `ADCS.kicad_pcb`. |
| `board_config.h` | Bench-check settings: sensor axis map and signs, coil polarity, which sun-sensor channel is on which face, sensor addresses, PWM frequency. |
| `main.c` | Boot and the main loop. |

Shared code used from elsewhere: `../ICD/slave/` (I2C framing, common commands, modes, status flags) and `../firmware/stm32/` (startup, clock, watchdog, I2C slave/master drivers, log).

## How it works

**Main loop** (`main.c`): one pass is `adcs_step()` (poll the ICD link, gyro task, control task, health task), the I2C stuck-bus check, the log drain and a watchdog kick (2 s). Nothing in a pass may block for more than about 10 ms, because the OBC expects an answer within 20 ms. The I2C slave interrupt only copies bytes, and commands are executed in the loop.

**Boot:** self-test of the gyro and magnetometer. With a magnetometer it enters **DETUMBLE by itself** (it never waits for the OBC); without one it goes to SAFE. The 5 s gyro offset measurement starts.

**Control cycle** (every `period_ms`, default 1000 ms), a small state machine in `control_task()`:

```
ACTUATE ──period over──> coils OFF ──settle_ms (20)──> SETTLE: start one magnetometer measurement
   ^                                                         |
   |                                       MEASURE: poll every 2 ms (from 9 ms, give up at 50 ms)
   └── coils ON with the new duty <── B-dot <── read sun sensors <── sample ready
```

The magnetometer is **only ever sampled with the coils off**, so it reads the Earth's field and not the coils' own field. The simulator checks this on every sample (`sim_adcs_dirty_mag_samples() == 0`).

**B-dot** (`bdot.c`): `dB/dt = (B_now − B_prev) / dt` from two clean samples, then `duty = −(gain/1000) · dB/dt` per axis (‰ of full drive). If any axis would exceed ±1000 ‰, all three are scaled down together so the dipole keeps its direction. A tumbling satellite sees the field rotate in its own frame. Opposing that change makes the coils' torque act against the spin.

**Sun sensors** (ICD v0.6, v0.7): BPW34 photodiodes on all **six** faces. They are the ADCS's attitude sensor and have nothing to do with the payload's TSL2591 solar-flux sensor on the Pi. With no pointing control any face can face the Sun, and an earlier 5-face design read as eclipse whenever its bare face did. Once per cycle, coils off, `sun_update()` works out:
- **Sun direction:** each face reads ≈ full × cos(angle to the Sun). Of each opposite pair only the brighter face is used, because the Sun lights only one; the other reading is albedo or noise. Component = ±brighter / full.
- **Sunlit / eclipse:** a face counts as sunlit only at or above `lit`, the **albedo threshold**, so the Earth's glow cannot fake daylight. The OBC's EPS uses this for its eclipse decision.
- **Gyro tracking:** between measurements the last Sun direction is turned with the gyro. That covers a diode failing in orbit:
  - In daylight the six readings must add up to a unit vector. If light is missing, the dark face on the side the tracked Sun favours is suspected. Still dark after 5 cycles while the Sun should light it, it is marked **dead** (fault SUN_DIODE) and its axis is filled in from |s| = 1.
  - With the Sun behind a dead face everything reads dark: ADCS reports **HIDDEN**, meaning unknown, not eclipse, together with the tracked direction.
  - A dead face that lights up again is trusted again. The ground can also switch faces off (`SET_SUN_DISABLED`).
- Page 4 carries all of it. `SET_SUN_CONFIG` sets `full` (measure it within ~10 %) and `lit`.

With no pointing law, ADCS reports the Sun direction in the satellite's own frame. The full orientation (Sun plus magnetic field, compared with where they should be for the time and orbit position) is for the OBC or ground to compute later.

A 5-face build (no −Z diode) still exists: `-DSUN_NO_MZ`, build target `adcs_5face`, and `FW_DEFS="-DSUN_NO_MZ" python3 firmware/build.py test`.

**Rate and DETUMBLED:** the gyro is read every 100 ms, with the zero-rate offset removed. When the rate stays below `exit_rate` (2.00 °/s) for `dwell_s` (60 s), ADCS reports DETUMBLED and the OBC commands NOMINAL. Above `enter_rate` (5.00 °/s) the flag clears again. The gap between the two limits stops it flickering.

**Gyro offset** (ICD v0.5): an MPU-6050 reads a few °/s even at rest, which alone could keep the rate above 2 °/s forever. For the first 5 s after boot ADCS averages the gyro. It keeps the mean only if the board was still: the gyro moved < 0.5 °/s and the raw magnetometer < 0.5 µT on every axis, which also catches a slow steady spin. Telemetry page 3 shows the result (AUTO or REJECTED). In orbit ADCS boots tumbling, so the OBC re-sends the value it saved on the bench (`SET_GYRO_OFFSET`).

**Modes** (rules in `../ICD/ICD.md` section 8):

| Mode | Coils | Notes |
|---|---|---|
| SAFE | drivers asleep | Sensors and telemetry still live. |
| DETUMBLE | B-dot | Default after boot. |
| NOMINAL | off | Pointing law TBD. Manual coil pulses (`MTQ_MANUAL`, ≤ 10 s) allowed for bench tests. |

A commanded mode change switches the coils off immediately.

**Faults and autonomy** (`health_task()`):
- Three magnetometer failures in a row in DETUMBLE → SAFE. A coil driver nFAULT lasting 3 cycles → SAFE.
- Dead sensors are retried every 5 s.
- If ADCS is in SAFE because of a fault, the magnetometer and drivers are fine for 30 s, and the OBC has been silent for 30 s, it resumes DETUMBLE on its own. A dead gyro or sun ADC does not block this, since B-dot needs neither.
- If the fault comes straight back (a shorted driver), each wait doubles (30 s … 32 min). Ten clean minutes, or any command from the OBC, resets it.
- Fault bits are sticky until the OBC has read them with GET_STATUS.

**ICD interface:**
- **Commands:** set gain, set thresholds, set timing, read config, manual coil pulse, set magnetometer offset, set gyro offset, set sun config, disable sun faces (0x20–0x28). The OBC re-sends all settings after every ADCS reset.
- **Telemetry pages:**
  - Page 0: field, gyro rates, magnetometer sample age.
  - Page 1: raw sun-sensor counts, coil duty.
  - Page 2: dB/dt, rate, temperature, flags, I2C counters.
  - Page 3: gyro offset.
  - Page 4: Sun direction, sunlit / eclipse / hidden, lit and dead faces.

## Bench values still to set (docs/BRINGUP.md section 2)

- Axis map and signs in `board_config.h`, from the rotation and compass check.
- Coil polarity (`MTQ_SIGN`), from the compass check.
- Hard-iron magnetometer offset, in `obc.toml` `[adcs] mag_offset`.
- Gyro offset: power ADCS up once untouched and check `tlm adcs 3` shows AUTO.
- Sun sensors: which channel (J4–J9) is on which face, in `board_config.h` (placeholder: SS1..SS6 = +X, −X, +Y, −Y, +Z, −Z). Then `full` (one diode facing the Sun, within ~10 %) and `lit` (above the Earth-albedo glow), in `obc.toml` `[adcs]`.
