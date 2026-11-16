# ADCS Design Record — Evidence-Based Extraction

Extracted 2026-11-16 from the repository as it stands on `main` (c368f2c), the same way as the TT&C record.

## 0. Ground rules

- Evidence only. A value is written down only if a file says it.
- **Status tags:** DESIGNED (in schematic/PCB), IMPLEMENTED (in firmware), SIM-TESTED (passes PC tests), TESTED (measured on hardware), REVISED, REJECTED, PENDING, UNVERIFIED.
- **Source tags:**
  - SCH: `ADCS/ADCS.kicad_sch`
  - PCB: `ADCS/ADCS.kicad_pcb`
  - BOM: `ADCS/ADCS.csv`
  - FW: `Software/ADCS/*`
  - ICD: `Software/ICD/ICD.md` v0.9
  - CHLOG: `Software/CHANGELOG.md`
  - REC: `docs/fyp_record.md`
  - NOTES: `docs/Mission_Notes.md`
  - GIT: commit history
  - MYCALC: my own calculation, labelled and kept separate from project history.
- **Method:** the KiCad files were parsed as text with a script. KiCad was **not** run (no ERC or DRC run, no 3D view). Pin names come from the STM32F446RET6 LQFP-64 pinout, matched to the PCB pad numbers.

## 1. Sources

| Source | What it is |
|---|---|
| SCH / PCB | Title "ADCS PCB". Board text: "ADCS PCB 10/2026 VJTI — Parth Kakirde, Pushkar Dube, Arpit Rajput" |
| BOM (`ADCS.csv`) | 102 lines, KiCad export (reference, value, footprint) |
| `ADCS/ADCS_Gerber.zip` | Gerbers for the board on its own |
| `ADCS/README.md` | Empty |
| FW `Software/ADCS/` | fw 0.5.0, ICD v0.7 (1,414 lines incl. README) |
| REC §6 | Design decisions and review history as recorded by the electronics lead |

**Provenance (CHLOG 2026-11-14):** the ADCS firmware came from a "base repo" (`~/Downloads/code.zip`); "nobody on the team wrote it". It was reviewed line by line, and fw 0.3.0–0.5.0 are fixes and features added after that review. Describe authorship carefully in the report.

## 2. Purpose and role [ICD, FW]

- Stops the satellite tumbling after deployment: **B-dot detumbling** with three magnetorquers.
- Works out the Sun direction and sunlit/eclipse from six photodiodes.
- Serves all of it to the OBC over I2C (slave address **0x32**).
- **Success criterion (REC §3):** detumbling within a couple of orbits. No precision pointing.
- **Pointing law:** none. NOMINAL mode turns the coils off ("pointing law TBD").
- **Autonomy:** boots into DETUMBLE by itself (it never waits for the OBC). It keeps detumbling if the OBC goes silent (lost-master after 30 s).

## 3. Architecture

```
                      +-------------------- ADCS board (85 x 84 mm) --------------------+
 EPS J8  3.3 V ->J14->F2 500 mA-> 3.3V rail -> STM32F446 (U1), 6x MCP6001, sensor modules
 EPS J12/13 5 V->J15->F1 2 A  -> 5V rail   -> 3x DRV8833 VM (U3 X, U4 Y, U5 Z)
                      |                                                                 |
 Pi (I2C master) <-J11-> I2C3 PA8/PC9 (slave 0x32)                                      |
 MMC5983MA module <-J10-> I2C1 PB6/PB7 (master)  0x30                                   |
 MPU-6050 module  <-J12-> I2C1 PB6/PB7 (master)  0x68, INT -> PA15                     |
 BPW34 x6 <-J4..J9-> MCP6001 TIA x6 -> PA0..PA5 (ADC1 ch 0-5)                           |
 STM32 PWM -> DRV8833 AIN1/AIN2 -> AOUT1/AOUT2 -> J1/J2/J3 -> air-core coils X/Y/Z      |
 SWD/SWO <-J16 Tag-Connect TC2030                                                       |
                      +-----------------------------------------------------------------+
```

- **Magnetometer and gyro are off-board breakout modules.** The MMC5983MA sits on J10 (4-pin) and the MPU-6050 on J12 (8-pin, GY-521 style). Neither chip is soldered on the ADCS board.
- REC §6.1: the magnetometer goes on a **flex-cable daughterboard**, away from the coils.
- **The coils are off-board,** on J1–J3 (REC §6.5: air-core coils, Oct 2026).
- **Two grounds:** GND (digital, power) and GNDA (analog: TIAs, VSSA). They are tied at one point by **R15 0 Ω** ("GND to GNDA single-point tie").

## 4. Bill of materials (from SCH/BOM, 103 parts + 4 mounting holes)

| Ref | Value | Package | Function |
|---|---|---|---|
| U1 | STM32F446RETx | LQFP-64 | MCU |
| U3, U4, U5 | DRV8833PWP | HTSSOP-16 + thermal vias | Coil drivers X, Y, Z (A-bridge only) |
| U6–U11 | MCP6001-OT | SOT-23-5 | Sun-sensor transimpedance amps SS1–SS6 |
| R4–R9 | 680 Ω | 0805 | TIA feedback resistors |
| C15–C20 | 22 pF | 0805 | TIA feedback capacitors |
| R1_0805, R1_806, R1_807 | 1 Ω | 0805 | DRV8833 AISEN sense (X, Y, Z) |
| R1, R2, R3 | 100 kΩ | 0805 | nSLEEP pull-downs (X, Z, Y) |
| R12, R13, R14 | 10 kΩ | 0805 | nFAULT pull-ups (Y, X, Z) |
| R10, R11 | 4.7 kΩ | 0805 | Pi link I2C3 pull-ups |
| R16, R17 | 4.7 kΩ | 0805 | Local I2C1 pull-ups |
| R15 | 0 Ω | 0805 | GND–GNDA tie |
| C3/C4, C9/C10, C13/C14 | 10 µF + 100 nF | 0805 | DRV8833 VM decoupling per driver |
| C6, C7, C11 | 0.01 µF | 0805 | DRV8833 VCP (to VM) |
| C5, C8, C12 | 2.2 µF | 0805 | DRV8833 VINT |
| C24–C28 | 100 nF | 0603 | MCU VDD 19/32/48/64, VBAT 1 |
| C29 | 4.7 µF | 0805 | MCU bulk |
| C23 | 4.7 µF | 0805 | VCAP_1 |
| C21, C22 | 1 µF, 10 nF | 0805 | VDDA to GNDA |
| C30–C35 | 100 nF | 0603 | MCP6001 supply decoupling |
| C36, C37 | 10 µF | 1206 | Bulk at 3.3 V (J14) and 5 V (J15) inputs |
| C1 | 100 nF | 0805 | NRST |
| F1 | 2 A | 1206 | 5 V input fuse |
| F2 | 500 mA | 1206 | 3.3 V input fuse |
| J1–J3 | JST-PH 2-pin | | Coils X, Y, Z |
| J4–J9 | JST-PH 2-pin | | Photodiodes SS1–SS6 |
| J10 | JST-PH 4-pin | | Magnetometer module (3.3V, SCL, SDA, GND) |
| J11 | JST-PH 3-pin | | Pi link (SDA, SCL, GND) |
| J12 | JST-PH 8-pin | | MPU-6050 module (3.3V, GND, SCL, SDA, NC, NC, NC, INT) |
| J14 | JST-XH 2-pin | | 3.3 V in |
| J15 | JST-XH 2-pin | | 5 V in |
| J16 | Tag-Connect TC2030 | | SWD (3.3V, SWDIO, NRST, SWCLK, GND, SWO) |
| TP1–TP20 | Test pads 1×1 mm | | See §9 |
| H1–H4 | M4, Ø4.0 mm | | Mounting, 74 × 76 mm pattern |

- No part is marked DNP.
- **No crystal is fitted** (PH0/PH1 and PC14/PC15 are unconnected). The firmware runs from the internal 16 MHz HSI (`Software/firmware/stm32/platform.c`). This matches the REC §12 decision: "HSE crystal (ADCS): not fitted, not needed".
- **No UART header and no status LED.** The log goes out over SWO.

## 5. MCU pin map (PCB nets → STM32F446RET6 LQFP-64)

| Pad | Pin | Net | Use |
|---|---|---|---|
| 2 | PC13 | DRVY_SLEEP | Y driver nSLEEP |
| 3 | PC14 | DRVZ_SLEEP | Z driver nSLEEP |
| 7 | NRST | NRST | C1 100 nF, J16 |
| 12/13 | VSSA/VDDA | GNDA / 3.3V | VDDA straight from 3.3V; C21 1 µF + C22 10 nF to GNDA |
| 14–17 | PA0–PA3 | SS1–SS4 | Sun sensor ADC ch 0–3 |
| 20, 21 | PA4, PA5 | SS5, SS6 | Sun sensor ADC ch 4–5 |
| 22, 23 | PA6, PA7 | DRVY_AIN1/2 | Y coil PWM (TIM3 CH1/CH2) |
| 28 | PB2 | DRVX_SLEEP | X driver nSLEEP (PB2 is also BOOT1; R1 pulls it low) |
| 30 | VCAP_1 | | C23 4.7 µF |
| 34–36 | PB13–PB15 | DRVX/Y/Z_FAULT | nFAULT inputs |
| 37, 38 | PC6, PC7 | DRVX_AIN1/2 | X coil PWM (TIM8 CH1/CH2) |
| 40 | PC9 | RPI_SDA | I2C3 SDA (Pi link) |
| 41 | PA8 | RPI_SCL | I2C3 SCL (Pi link) |
| 42, 43 | PA9, PA10 | DRVZ_AIN1/2 | Z coil PWM (TIM1 CH2/CH3) |
| 46, 49 | PA13, PA14 | SWDIO, SWDCLK | Debug |
| 50 | PA15 | MPU_INT | MPU-6050 interrupt |
| 55 | PB3 | SWO | Trace / log |
| 58, 59 | PB6, PB7 | I2C1_SCL/SDA | Local sensor bus |
| 60 | BOOT0 | GND | Tied directly to GND (no ROM bootloader; programming is SWD only) |

- **Unused:** PC0–PC5, PC8, PC10–PC12, PC15, PH0/PH1, PB0, PB1, PB4, PB5, PB8–PB10, PB12, PA11, PA12, PD2.
- **Timer allocation (FW `hw_stm32.c`):** X = TIM8, Y = TIM3, Z = TIM1. PWM is 16 MHz / 800 = **20 kHz** (`MTQ_PWM_ARR 799`).
- **Spare-pin note (MYCALC):** the AISEN sense nodes go only to test points, not to the MCU (ICD), so coil current cannot be measured in firmware. If it is ever needed, the spare ADC-capable pins PC0–PC5 and PB0/PB1 could take a wire patch.

## 6. Circuits

### 6.1 Power input [DESIGNED]

- **3.3 V:** J14 → F2 500 mA → 3.3V rail, with C36 10 µF bulk. It feeds the MCU, the TIAs and both sensor modules.
- **5 V:** J15 → F1 2 A → 5V rail, with C37 10 µF bulk. It feeds only the DRV8833 VM pins (coil power).
- **No on-board regulator.** Both rails come from the EPS: EPS J8 `ADCS_3V3_POWER` and J12/J13 `ADCS_5V_A/B` (see the EPS record).
- **No reverse-polarity protection** on either input.

### 6.2 Magnetorquer drivers [DESIGNED; IMPLEMENTED]

- **One DRV8833 per axis, A-bridge only.** BOUT1/BOUT2 are left unconnected.
  - REC §6.6: thermal shutdown is per die, so one IC per axis keeps one axis's fault from taking down a second axis.
- **VM = 5 V.** REC §6.3 said "VM → EPS bus". The board uses the EPS 5 V buck output instead.
- **VCP:** 0.01 µF to VM. REVISED from 100 nF in the external review, per the datasheet (REC §6.8).
- **VINT:** 2.2 µF to GND. REC §6.3 says 100 nF; the board has 2.2 µF (both are in the datasheet's typical range — UNVERIFIED which was intended).
- **nSLEEP:** 100 kΩ pull-down, so the drivers stay asleep at power-up until the MCU wakes them.
- **nFAULT:** 10 kΩ pull-up to 3.3 V (added in review, GIT 96a1cd6 "nFAULT").
- **AISEN:** 1 Ω to GND. ICD open issue 2 gives the driver's own current limit as about 200 mV / 1 Ω = **200 mA per coil**. The detumble simulation does not model this cap.
- **Coil connector polarity:** `+duty` drives AOUT1 high (ICD page 1). Sign correction is `MTQ_SIGN` in `board_config.h`, set by compass check at bring-up.

### 6.3 Sun sensors [DESIGNED; IMPLEMENTED]

**Circuit, per channel (×6):**
- Jn pin 1 → MCP6001 −IN (pin 4), and pin 2 → GNDA.
- +IN (pin 3) → GNDA.
- Rf 680 Ω ‖ Cf 22 pF from OUT to −IN.
- OUT → SSn → PA0–PA5.
- This is a **zero-bias (photovoltaic) transimpedance amplifier**: Vout = I_photo × 680 Ω.

**Revision (REC §6.7):** Rf/Cf changed from 47 kΩ / 10 pF to **680 Ω / 22 pF** in Oct 2026. The reason was not recorded. The MYCALC below suggests it was saturation in full sunlight.

**MYCALC (UNVERIFIED, assumptions labelled):**
- BPW34 active area 7.5 mm², and average responsivity over the solar spectrum taken as about 0.35 A/W.
- Sun above the atmosphere is about 136 mW/cm², so about 10 mW lands on the diode, giving I ≈ 3.5 mA.
- With Rf = 680 Ω, Vout ≈ 2.4 V, which is inside the 3.3 V ADC range. That is about 2,980 counts at 12 bits, which agrees with the firmware's default `full = 3000`.
- With the old 47 kΩ the output would have saturated at about 50 µA, i.e. in any daylight.
- Bandwidth: f = 1/(2π·680·22 pF) ≈ 10.6 MHz. That is far above the 1 Hz sampling, so Cf does almost nothing at this Rf.

**Diode orientation (check at harness build, UNVERIFIED):**
- On a single supply the output can only go positive. The photocurrent must therefore flow *out of* the −IN node into the diode, so the **cathode goes to Jn pin 1 (op-amp) and the anode to pin 2 (GNDA)**.
- REC §6.7 says "BPW34 cathodes … AGND-referenced". If the diodes are wired that way, the outputs will sit at 0 V in sunlight. Check one channel with a torch before wiring all six.

**Other details:**
- VDDA (the ADC reference on LQFP-64) is the 3.3 V rail directly, with only C21/C22. REC §6.1 asked for a clean VREF+ (LC filter or REF3033), which was not fitted. Expect some rail noise in the counts.
- NOTES says "connected in reverse bias with TIA"; the design is zero-bias (no bias voltage on +IN).

**Count of sensors — contradiction:**
- **SCH/PCB/FW:** six channels, all fitted.
- **FW fw 0.5.0 / ICD v0.7:** "a sun diode on every face". With no pointing control, a bare face reads as eclipse about 5 % of the time.
- **REC §6.1 and §13:** "reduced from six to two (Nov 2026), four channels unpopulated".
- **The firmware is the later decision.** It also has a 5-face build (`-DSUN_NO_MZ`). Settle this before writing the report.

### 6.4 Sensors (off-board modules) [IMPLEMENTED]

| Sensor | Connector | Address | Notes |
|---|---|---|---|
| MMC5983MA magnetometer | J10 (3.3V, SCL, SDA, GND) | 0x30 | Sampled **only with the coils off** (settle 20 ms) |
| MPU-6050 gyro/accel | J12 (8-pin) + INT on PA15 | 0x68 (AD0 low) | Gyro read every 100 ms; range ±500 °/s (ICD); zero-rate offset measured at boot |

- **I2C1 pull-ups:** R16/R17 4.7 kΩ are fitted (GIT 96a1cd6 "I2C1 pull-ups"). REC §6.4 says none were needed because the modules carry their own. Together they give about 2.4 kΩ, or lower if a module also has 4.7 kΩ. That is fine at 100 kHz.
- **Axis mapping:** placeholders `MAG_MAP`, `GYRO_MAP` and `*_SIGN` = identity in `board_config.h`. Set at the bench (BRINGUP §2).

### 6.5 Pi link [DESIGNED; IMPLEMENTED]

- I2C3 slave at 0x32 on PA8/PC9, with 4.7 kΩ pull-ups R10/R11.
- Header J11: pin 1 SDA, pin 2 SCL, pin 3 GND.
- Pi side: `/dev/i2c-1`, 100 kHz (ICD §1). Clock stretching caveat and bit-bang fallback: ICD §1.

### 6.6 Debug [DESIGNED]

- J16 Tag-Connect TC2030 carries SWDIO, SWCLK, NRST and SWO (PB3).
- The Pi can flash the board by SWD bit-bang (`OBC/scripts/swd_flash.py adcs`). No ST-LINK was available (ICD §12).

## 7. PCB [DESIGNED]

| Item | Value |
|---|---|
| Outline | 85 × 84 mm (panel README: 85.2 × 84.2 incl. edge) |
| Layers | 2 (F.Cu, B.Cu), 1.6 mm FR-4 (KiCad default stackup: 35 µm Cu, 1.51 mm core, εr 4.5) |
| Tracks | 965 segments: 0.2 mm (817), 0.5 mm (122), 0.15 mm (26) |
| 0.5 mm nets | 5V, coil outputs X/Y/Z_AOUT1/2, J15 input, part of GND |
| Vias | 98, all 0.6 / 0.3 mm; smallest drill 0.2 mm (DRV8833 thermal vias) |
| Copper zones | One on F.Cu + B.Cu and one on B.Cu (ground fill) |
| Mounting | 4 × Ø4.0 mm on a 74 × 76 mm pattern (same pattern as EPS and TT&C) |
| Schematic ↔ PCB | All 103 schematic references are on the PCB, and every value matches (parsed check) |

- REC §6.8: a 4-layer board was not pursued (reason not recorded). The uniform 0.5 / 0.2 mm width scheme was chosen for routing simplicity.
- **Open across boards (panel READMEs):** ADCS and TT&C holes are Ø4.0 mm, while EPS is Ø4.3 mm. An M4 rod needs 4.3 mm.
- **Open (panel READMEs):** silkscreen text in the board file is 0.9 mm. It was raised to 1.0 mm only in the panel copies.

## 8. Firmware (fw 0.5.0) [IMPLEMENTED, SIM-TESTED]

| File | Lines | Content |
|---|---|---|
| `adcs_app.c/.h` | 815 / 108 | Modes, control cycle, gyro offset, faults, ICD commands, telemetry (portable, no registers) |
| `bdot.c/.h` | 29 / 19 | B-dot law |
| `hw_stm32.c` | 233 | MPU-6050, MMC5983MA on I2C1; DRV8833 on TIM8/TIM3/TIM1; ADC1 sun channels |
| `board_config.h` | 48 | Axis maps, coil signs, sun-channel → face map, addresses, PWM |
| `main.c` | 34 | Boot and main loop (watchdog 2 s) |

**Control cycle** (default period 1000 ms, settle 20 ms):
1. Coils off.
2. Wait the 20 ms settle.
3. Start one magnetometer measurement, then poll every 2 ms (from 9 ms, give up at 50 ms).
4. Read the sun sensors.
5. B-dot.
6. Coils on with the new duty.

The magnetometer is never sampled with the coils on. The simulator checks this on every sample.

**B-dot:**
- `duty‰ = −(gain/1000) · dB/dt [µT/s]`. Default gain 300,000 (max 10,000,000).
- If any axis would go past ±1000 ‰, all three axes are scaled down together, so the dipole keeps its direction.

**Detumbled:**
- The rate must stay below 2.00 °/s for 60 s.
- The flag clears again above 5.00 °/s (hysteresis).

**Modes:**
- SAFE: drivers asleep, sensors still live.
- DETUMBLE: the default after boot when a magnetometer is present.
- NOMINAL: coils off; MTQ_MANUAL pulses up to 10 s are allowed.

**Faults and autonomy:**
- These go to SAFE:
  - 3 magnetometer failures in a row.
  - A driver nFAULT lasting 3 cycles.
- Dead sensors are retried every 5 s.
- Recovery from a fault-SAFE:
  - It resumes DETUMBLE by itself after 30 s, if the faults have cleared and the OBC is silent.
  - The wait doubles each time the fault returns: 30 s up to 32 min.
  - It resets after 10 clean minutes.

**ICD commands 0x20–0x28:**

| ID | Command |
|---|---|
| 0x20 | SET_BDOT_GAIN |
| 0x21 | SET_DETUMBLE_THRESH |
| 0x22 | SET_BDOT_TIMING |
| 0x23 | GET_CONFIG |
| 0x24 | MTQ_MANUAL |
| 0x25 | SET_MAG_OFFSET |
| 0x26 | SET_GYRO_OFFSET |
| 0x27 | SET_SUN_CONFIG |
| 0x28 | SET_SUN_DISABLED |

**Telemetry pages:**

| Page | Content |
|---|---|
| 0 | Field, gyro, mag age |
| 1 | Raw sun counts, coil duty (commanded, not measured current) |
| 2 | dB/dt, rate, temperature, flags, I2C counters |
| 3 | Gyro offset |
| 4 | Sun direction, sunlit / eclipse / hidden, dead faces |

**Sun algorithm (v0.6–v0.7):**
- Of each opposite pair of faces, only the brighter is used.
- A face counts as sunlit only at or above the `lit` (albedo) threshold.
- Between measurements the Sun direction is propagated with the gyro.
- A dead diode is found after 5 cycles, raises fault SUN_DIODE, and the Sun behind it is reported HIDDEN instead of eclipse.
- Defaults `lit` 1200 / `full` 3000 counts, **TBD (measure)**.

**Version history (CHLOG):**
- fw 0.3.0: gyro zero-rate offset and recovery rules.
- fw 0.4.0: sun direction and eclipse.
- fw 0.5.0: a sun diode on all six faces, gyro tracking.

## 9. Test points (PCB)

| TP | Net |
|---|---|
| TP1, TP2, TP3, TP5 | 5V (one per driver VM plus input) |
| TP4 | 3.3V |
| TP6–TP11 | X/Y/Z AOUT1, AOUT2 |
| TP12–TP17 | SS1–SS6 (TIA outputs) |
| TP18–TP20 | X/Y/Z AISEN (coil current: V/1 Ω) |

- That is 20 in total, matching REC §6.8.
- The AISEN test points are the only way to see coil current: 1 mV = 1 mA.

## 10. Magnetorquers

- **Current design (REC §6.5):** air-core coils, off-board, on J1–J3. **Turns, wire, size, resistance, dipole moment and current are not recorded.**
- **Superseded:** a 15-turn 26 AWG coil on a 5 × 70 mm soft-iron rod, about 0.059 A·m² against a 0.05 A·m² requirement.
- **REJECTED:** PCB-trace coils (537× below the requirement even at 3 oz copper; too large for 2U).
- **Requirement conflict:** NOTES gives `m_target = 0.01 A·m² per axis` (2U, ~2 kg), while REC gives 0.05 A·m². Settle which one applies.
- **MYCALC (UNVERIFIED):** the dipole is m = N·I·A. At the 200 mA driver limit, a coil of area A [m²] needs N = m / (0.2·A) turns.
  - For an 80 × 80 mm air-core loop (A = 0.0064 m²): 0.01 A·m² needs about 8 turns; 0.05 A·m² needs about 39 turns.
  - The coil resistance must also allow 200 mA from 5 V: R ≥ 25 Ω at full duty, or the DRV8833 current limit chops it.

## 11. Testing and validation

| Test | Class | Evidence |
|---|---|---|
| B-dot, gyro offset (still/tumbling, limits), recovery back-off, sun direction on a 324-point grid (≤ 1.2°), albedo rejection, dead diode found in 12 s / tracked ≤ 4.2°, 5-face build | SIM-TESTED | ICD §12, CHLOG (1,354 firmware checks) |
| Link tests (bad frames, unplug, 10,000 PINGs) | SIM-TESTED | 15/15 vs firmware simulation |
| ARM build | NOT RUN | CHLOG "Not done yet": no ARM compiler on the dev PC |
| Watchdog and I2C slave drivers | Compile-checked only | CHLOG |
| Anything on real hardware (flash, sensors, coils, sun channels, link to the Pi) | NOT TESTED | No record |

**Bench values still to set (FW README, BRINGUP §2):**
- Axis maps and signs.
- Coil polarity.
- Magnetometer hard-iron offset.
- Gyro offset.
- Sun channel → face map.
- `full` and `lit`.

## 12. Revision history (GIT, REC)

| Date | Commit / record | Change |
|---|---|---|
| Sep 2026 | REC §13 | Components chosen; RPi 3A+ assumed; iron-rod MTQ; I2C1/I2C2 split; TIA 47 kΩ / 10 pF; VCP 100 nF |
| 2026-09-26 | dbd5947 (Parth Kakirde) | "Added files of ADCS pcb" |
| Sep–Oct 2026 | REC §6.8 | External review: AGND/GND tie, VCP 0.01 µF, nFAULT pull-ups, I2C pull-up check, per-axis DRV8833 confirmed |
| 2026-10-13 | 96a1cd6 (Pushkar Dube) | "Review fixes (nFAULT, VCAP, I2C1 pull-ups, board size, decoupling)" |
| Oct 2026 | REC §13 | TC2030 + SWO on PB3; no HSE; input fuses; 1 Ω AISEN; TIA 680 Ω / 22 pF; Pi link on I2C3; air-core coils on J1–J3 |
| 2026-10-14 | 8dc4beb (vijay) | "Update ADCS and TTC KiCad designs" |
| 2026-10-23 | 504b046 (Parth Kakirde) | "Add final PCB designs" (Final_PCB) |
| 2026-11-14 | CHLOG | Firmware ported; fw 0.3.0–0.5.0 |
| Nov 2026 | REC §13 | Sun sensors "reduced to two" (conflicts with fw 0.5.0, see §6.3) |

## 13. Current status

- **Schematic and PCB:** complete; parsed parity clean. The GND/GNDA tie is now R15 (REC §6.8 still lists it as "open"; the board has it).
- **Manufactured:** UNVERIFIED.
  - ICD says "The PCBs are manufactured".
  - REC says "in manufacturing".
  - The panel Gerbers exist.
  - No bring-up record.
- **Firmware:** implemented, sim-tested only.

## 14. Contradictions and open items

1. **Sun sensors:** six (SCH, PCB, FW 0.5.0) vs two (REC, Nov 2026).
2. **Dipole requirement:** 0.01 A·m² (NOTES) vs 0.05 A·m² (REC). The air-core coil design is not recorded at all.
3. **Coil current cap** of 200 mA (1 Ω AISEN) not checked against the coil design (ICD open issue 2).
4. **I2C1 pull-ups:** REC "none needed" vs R16/R17 fitted.
5. **AGND/GND join:** REC "open" vs R15 0 Ω fitted.
6. **Gyro part:** NOTES "ICM-42688" (one early list) vs MPU-6050 everywhere else.
7. **Magnetometer part:** NOTES "RM3100" (one early list) vs MMC5983MA everywhere else.
8. **Reaction wheels:** NOTES mentions them in an early list; the design has none. Detumble only.
9. **VINT capacitor:** REC 100 nF vs board 2.2 µF.
10. **VDDA:** REC asks for a clean reference (LC or REF3033); the board takes the 3.3 V rail directly.
11. **Photodiode orientation:** a harness check is needed (§6.3).
12. **OBC model:** Pi 4B (ICD) vs CM4 (REC).
13. **Mounting holes:** Ø4.0 mm here vs Ø4.3 mm on EPS.
