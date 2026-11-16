# 2U CubeSat Engineering Model — Master Project Record

Purpose of this file: single source of truth for writing the college report on the CubeSat engineering model (EM). It lists the up-to-date design, every revision on record, and what is still open.

## How to read this file (accuracy rules)

- Everything under "Recorded facts" comes from what the project lead (Parth) stated across the project chats, as saved in the project record. Nothing was added from outside sources.
- Where the **reason** for a change was recorded, it is written down. Where it was not recorded, the text says **"reason not recorded"** rather than guessing. Fill these in from the team before the report is final.
- Section 14 ("Verification flags") lists inconsistencies and gaps. Resolve every item there before using this file as a basis for the space-ready design.
- Dates: only "Sep 2026", "Oct 2026" and "Nov 2026" are known for most changes. Exact dates were not recorded.

---

## 1. Project identity and context

- Student capstone (college project): electronics design of a CubeSat **engineering model (EM)**.
- Compressed, **single-semester** timeline.
- Team: four students originally; **Parth is the electronics lead** and primary electronics decision-maker. Five junior team members were later recruited and added.
- The team has **no dedicated software engineer**. This constrains software complexity in every subsystem.
- The project is explicitly an EM prototype. No flight vehicle extension is planned within this capstone. A later space-ready CubeSat is intended to be derived from the EM design and report.
- The report for the college is planned at roughly 100 pages.

## 2. Form factor: from 3U brief to 2U final

| Item | Original brief | Final |
|---|---|---|
| Size | 3U preferred, 10 × 10 × 30 cm | **2U, 10 × 10 × 20 cm** |
| Max mass | 6 kg | Notes keep a "4 kg" MTQ-sizing figure (see flags, section 14) |
| PCB size | not stated | 84 × 84 mm boards |

- **Original brief (as first described):** 3U CubeSat, 10 × 10 × 30 cm, 6 kg maximum, five subsystems (Mechanical, EPS, ADCS, TT&C, Payload), Master–Slave configuration with a Raspberry Pi doing housekeeping tasks and STM32 microcontrollers doing all other tasks.
- **Oct 2026 decision:** 2U declared the final form factor. 3U dropped.
- **Nov 2026:** the project was re-described once as "preferably 3U". Parth then corrected this and confirmed 2U again. **2U is final.**
- Reason for choosing 2U over 3U: not explicitly recorded. Related recorded facts: compressed single-semester scope, no software engineer, and the ADCS magnetorquer analysis concluding that PCB-trace coils were "too big for 2U".
- Effect: the 2U size shaped the ADCS coil design (air-core off-board coils) and the mass/volume budget for all subsystems.

## 3. Mission definition

- **Type:** passive science data collection.
- **Payload goals:** relative solar flux measurement (not calibrated irradiance) and opportunistic Earth imaging with a **Pi Camera**.
- UV sensing was part of the original brief and was **dropped in Nov 2026** (confirmed; see section 9).
- **ADCS success criterion:** detumbling within a couple of orbits. No precision pointing is required.
- **Scope discipline:** EM-first. Features that have no clear EM validation value are deferred or dropped (see section 12).

## 4. System architecture

### 4.1 Five subsystems
Mechanical, EPS (electrical power), ADCS (attitude determination and control), TT&C (telemetry, tracking and command), Payload.

### 4.2 Master–Slave configuration (original brief)
- Raspberry Pi: housekeeping tasks.
- STM32: all other tasks.

### 4.3 Final architecture (as evolved)
- **Master:** Raspberry Pi **CM4** (the OBC). It is **always powered on**. It manages EPS directly over a **control link (I2C and GPIO)**, reads EPS telemetry from the **INA219** over I2C, runs the payload, and handles housekeeping.
- **No dedicated EPS MCU.**
- **Slaves:** STM32F446RET6 (LQFP64) on ADCS and TT&C. They run their own subsystem functions independently of the RPi.
- The RPi's role toward ADCS/TT&C: telemetry collection, fault handling, and executing ground commands (ground commands are routed through the RPi). "Not active" for the RPi applies only to its role in ADCS/TT&C operation, never to its power state.
- **Open concern:** STM32s acting as I2C slaves cannot alert the RPi on their own. This is a problem in an emergency.
- Payload has no dedicated PCB. Its sensors connect directly to the RPi and are **powered through the RPi**.

### 4.4 How the architecture changed
1. **RPi model:** earlier notes said RPi **3A+**; Parth later confirmed **CM4**. Reason for the switch: not recorded.
2. **Inter-subsystem bus became I2C.** Cause: the RPi 3A+ had a single reliable hardware UART. Effect: the STM32 must be an I2C slave toward the RPi.
3. **STM32 I2C constraint.** An STM32 cannot be master and slave on the same I2C peripheral. Effect: the ADCS needed two I2C buses, one for local sensors (STM32 as master) and one for the RPi link (STM32 as slave).
4. **Independence of ADCS/TT&C from the RPi (Oct 2026).** The team is leaning toward the STM32s running independently, with the RPi woken for telemetry, faults and ground commands. This created the emergency-alert concern above.

## 5. Mechanical subsystem

- Only the form factor and PCB size (84 × 84 mm) are recorded. **No other mechanical details were recorded** (structure, materials, panels, deployment, mass budget). These must be added by the team (see section 14).
- The ADCS uses **two** sun-sensor photodiodes (reduced from six in Nov 2026; mounting faces not recorded) and off-board magnetorquer coils connected to the ADCS board by connectors J1–J3.

## 6. ADCS (attitude determination and control)

### 6.1 Bill of materials (finalized)
- MCU: STM32F446RET6 (LQFP64).
- Magnetometer: MMC5983MA **module** (I2C/SPI, 3.3 V, fixed address 0x30), mounted on a **flex-cable daughterboard** to keep it away from the magnetorquers.
- IMU/gyro: MPU-6050 **module** (I2C only, address 0x68 with AD0 tied to GND).
- Sun sensors: **two BPW34** photodiodes (**reduced from six in Nov 2026**; reason not recorded). Each feeds a transimpedance amplifier (TIA) using MCP6001 or TLV271. The board was laid out with six TIA channels on STM32 ADC pins PA0–PA5 (ADC1/2 channels 0–5); **four channels are left unpopulated**. Which two channels are used: not recorded.
- Magnetorquer (MTQ) drivers: **three DRV8833** H-bridge ICs, **one per axis**, channel B unused.
- VREF+ must be a clean 3.3 V (LC filter or REF3033).
- Reference design voltage inputs: 3.3 V and 5 V connectors.

### 6.2 MPU-6050 pin assignment
SCL → PB6, SDA → PB7 (I2C1, AF4); INT → PA15 (EXTI15); FSYNC, AD0, CLKIN → GND; CPOUT → its own 100 nF to GND; REGOUT → its own 100 nF to GND (**not shorted** with CPOUT); VLOGIC → NC or 3.3 V; XCL/XDA → NC.

### 6.3 DRV8833 per-axis connections
- VM → EPS bus, with 100 nF + 10 µF decoupling.
- VINT → 100 nF to GND.
- **VCP → 0.01 µF / 16 V to VM** (corrected, see 6.8).
- AIN1/AIN2 → STM32 PWM GPIO.
- ~SLEEP → STM32 GPIO plus 100 kΩ pull-down to GND (defaults to sleep at power-up).
- ~FAULT (nFAULT) → STM32 GPIO input, with a **10 kΩ pull-up** (added in review; initially an internal pull-up was planned).
- AOUT1/AOUT2 → MTQ coil via JST-PH or Picoblade connector.
- AISEN → **1 Ω** sense resistor to GND (range 0.2–1 Ω was considered; 1 Ω kept in the Oct 2026 review). Junction optionally to STM32 ADC for current monitoring.
- BIN1/BIN2 → GND; BISEN → GND; BOUT1/BOUT2 → NC.

### 6.4 I2C bus allocation
- **Current:** I2C1 (PB6/PB7) is the **local bus**: STM32 is master to MPU-6050 (0x68) and MMC5983MA (0x30). The **RPi link is on I2C3 (PA8/PC9)** with the STM32 as slave to the RPi master.
- **Earlier version:** an I2C1/I2C2 split. The I2C2 pin assignment and the reason for moving the RPi link to I2C3 were **not recorded**.
- The MPU-6050 and MMC5983MA modules already carry on-board I2C pull-up resistors, so **no extra board-level I2C1 pull-ups** are fitted.

### 6.5 Magnetorquer (MTQ) design and its revision
- **Earlier design (superseded):** 15-turn single-layer coil, 26 AWG, wound on a 5 mm diameter × 70 mm soft-iron rod; identical on all three axes; ~0.059 A·m² dipole against a 0.05 A·m² requirement (~18 % margin).
- **PCB-trace coils: definitively rejected.** Cause: even at 3 oz copper the dipole moment was **537× below** the requirement. Oct 2026 added: also too large for 2U.
- **Current design (Oct 2026):** the ADCS board drives **air-core** coils located off-board and connected on J1–J3.
- Air-core coil parameters (turns, wire, dimensions, dipole moment, current) are **not recorded**. The power budget is pending MTQ current-draw characterization.
- **Time multiplexing is mandatory:** coils must be de-energized and the field allowed to settle before the magnetometer is sampled. The flex-cable daughterboard is the layout mitigation.

### 6.6 Why three separate DRV8833 ICs
Intentional choice, confirmed in the external review. DRV8833 thermal shutdown is per die, so sharing one chip across two axes would cascade a thermal fault to a second axis. Using one IC per axis (A-channel only) isolates the fault.

### 6.7 Sun-sensor analog design and its revision
- BPW34 cathodes, MCP6001 VSS and the TIA non-inverting bias points are **AGND-referenced**; the TIA feedback resistor and capacitor are **not** grounded.
- **Revision (Oct 2026):** TIA values changed from **Rf 47 kΩ / Cf 10 pF** to **Rf 680 Ω / Cf 22 pF**. Reason not recorded.
- **Review correction:** the TIA non-inverting terminals were wrongly connected to GND and were corrected to **AGND**.
- BPW34 chosen over COTS sun sensors under the EM-first principle (simplicity).
- **Revision (Nov 2026):** sun sensors reduced from six to **two**. Reason not recorded. Effect: two sensors cover only part of the sky, so sun-direction estimates are limited. This does not affect the detumbling goal, which relies on the magnetometer and gyro.

### 6.8 ADCS PCB and review history
- KiCad. 84 × 84 mm, **2 layers**. A 4-layer board was **not pursued**. AGND and GND are routed as separate tracks alongside a digital ground fill.
- Schematic complete, ERC passing. Layout done: routing, ground fill, mounting holes, test points; DRC clean.
- **Trace widths:** 0.5 mm for VM, AOUT1/AOUT2 (×3 axes) and the 5 V input; 0.2 mm uniform for all other nets. A uniform scheme was preferred over a theoretically optimal one for routing simplicity.
- **Test points (20):** VM ×3, 3.3 V ×1, 5 V ×1, AOUT ×2 per driver (×6), TIA outputs ×6, AISEN ×3. The AGND/GND join and MPU-6050 INT test points were skipped by choice. The list above totals 20 (an earlier note said 17). With two sun sensors, four of the six TIA test points sit on unpopulated channels.
- **External PCB review findings:**
  1. **AGND and GND are not connected.** Open. A single-point join (near power entry) still has to be added.
  2. **VCP capacitor** was 100 nF; corrected to **0.01 µF / 16 V** per the DRV8833 datasheet.
  3. Decoupling capacitors rechecked across the board.
  4. I2C pull-ups: not needed (modules carry them).
  5. 10 kΩ pull-up added on nFAULT of all three DRV8833s.
  6. Three DRV8833 ICs confirmed as intentional (6.6).
- **Oct 2026 schematic review decisions:**
  - Tag-Connect **TC2030 SWD header** on the ADCS board; SWO wired to PB3.
  - **No HSE crystal** (internal oscillator is used; crystal not needed).
  - **Fuses** added on the ADCS power inputs (3.3 V and 5 V connectors).
  - 1 Ω AISEN resistors kept.
  - TIA values changed (6.7).
  - RPi link on I2C3 (6.4).
- **Manufacturing status:** the PCBs are **already in manufacturing**, so no new board-level connections can be added. Only **wire patches to existing pins** are possible. This directly affects the open AGND/GND join (see section 14).
- A 1,098-line ADCS handover markdown document was produced at the design-phase boundary.

## 7. TT&C (telemetry, tracking and command)

- **Status:** finalized for the EM.
- **Frequency: 433 MHz (final).** The architecture diagram's "437 MHz" label is wrong and must be corrected.
- **RF chain:** RFM98W (SX1278) module at 433 MHz → self-fabricated **LC bandpass filter** (NP0/C0G capacitors, high-Q RF SMD inductors; exact values deferred to filter design) → **balun** → female right-angle **SMA edge connector** → RG-316 cable → COTS **turnstile antenna** (EnduroSat or ISIS).
- **Balun: required (Nov 2026).** Exact position in the chain, part number and reason: not recorded. The position shown above follows the architecture diagram (filter → balun → antenna); confirm it.
- The single LC filter sits on the **shared Tx/Rx path** (acknowledged Nov 2026).
- MCU: STM32F446RET6 (LQFP64), linked to the RFM98W over SPI.
- **Dropped:** external PA and LNA, because an explicit **link budget** showed adequate internal margin at both EM test distances and LEO. The external **RF switch** was dropped as a consequence (no external branches).
- RFM98W **module** chosen over a discrete RF chain (EM simplicity). The module contains most of the TT&C chain. The transceiver, power amplifier, low noise amplifier and RF switch blocks in the architecture diagram **represent functions inside the RFM98W**, not separate parts, and stay in the diagram on that basis.
- **BOOT0 pull-down is non-negotiable.** No SWD header on the TT&C board; debugging is by UART print statements.
- **Ground station:** LilyGO TTGO LoRa32 (USB) with a whip antenna.
- **Open item:** design of the 433 MHz LC bandpass filter topology and component values (QUCS or an RF filter calculator), to be done in the TT&C phase.

## 8. EPS (electrical power system)

- Designed in KiCad by a **teammate**. J10 AUX_POWER_OUT is a spare protected power output.
- **Chain:** Solar array → reverse-polarity protection → MPPT → battery charger → battery + BPC → bus → buck converters → fuses → load switches → loads.
- The bus is a **common copper rail at battery voltage (2S Li-ion)**. Prototype BOM is set; architecture finalized.
- The RPi CM4 manages EPS directly over a control link (I2C and GPIO). There is no EPS MCU.
- **INA219 included (Nov 2026)** for telemetry (voltage and current monitoring, read by the CM4 over I2C). It is the "Parameter Monitor" block in the architecture diagram. I2C address and shunt value: not recorded.
- **Deferred to the flight model:** RBF pin, deployment switches, battery heater, EMI filtering, hardware load shedding.
- **Open items:** the power budget cannot be finalized until MTQ current draw is characterized. A teammate compiled a consolidated EPS report (power budget, battery sizing, solar panel selection) that **Parth has not yet verified**.

## 9. Payload

- **No dedicated PCB.** Sensors connect directly to the RPi (camera port and I2C) and are **powered through the RPi**.
- **Camera: Pi Camera** on the CM4's camera (CSI) port. **Changed from a USB webcam in Nov 2026.** Camera model and reason for the change: not recorded.
- **Solar flux:** **TSL2591** over I2C. Chosen for simplicity. Documented as a relative indicator only, not calibrated irradiance.
- **UV sensor (VEML6075): dropped, Nov 2026 (confirmed).** Reason not recorded. Effect: the payload is now solar flux plus imaging.
- **Magnetometer and IMU are shared with ADCS** (no separate payload units). The magnetometer sits on the ADCS board only.

## 10. Tools and standards

- EDA: KiCad. RF simulation: QUCS (planned).
- Connectors: JST-PH (2 mm) for PCB sensor connections; JST-SH (1 mm) considered; jumper wires for direct RPi connections.
- Antenna sources: EnduroSat, ISIS. Ground station: LilyGO TTGO LoRa32.
- Key ICs and parts: RFM98W/SX1278, DRV8833, MMC5983MA, MPU-6050, MCP6001/TLV271, TSL2591, BPW34, STM32F446RET6, INA219, Pi Camera, balun (part not recorded).
- External reviewer used for the ADCS PCB.

## 11. Design principles (why decisions were made)

1. **EM-first:** only what is testable and demonstrable in one semester.
2. **Module over bare IC** (MPU-6050 module, RFM98W module) to avoid decoupling and RF complexity.
3. **Dual-use sensors** (magnetometer, IMU shared with payload) to cut redundant hardware.
4. **I2C topology constraint** (RPi UART limit, STM32 master/slave limit) drove the I2C1/I2C3 design.
5. **MTQ/magnetometer time multiplexing** is mandatory.
6. **PCB trace coils are not viable.**
7. **Link budget before adding components** (PA/LNA eliminated only after calculation).
8. **AGND/GND discipline** with a single-point join near power entry.
9. **Simplified uniform schemes** over theoretically optimal ones when the EM benefits (e.g., uniform 0.2 mm traces).
10. **Honest trade-off assessment** over optimistic framing. Imprecise or speculative claims are corrected against the schematic.
11. Work proceeds iteratively per subsystem: component selection → schematic → PCB layout → bring-up planning.

## 12. Items dropped or deferred (and why)

| Item | Status | Reason recorded |
|---|---|---|
| External PA and LNA | Dropped | Link budget shows adequate margin |
| RF switch | Dropped | No PA/LNA branches |
| GPS | Dropped/deferred | EM-first scope |
| Reaction wheels, RCS | Dropped/deferred | EM-first scope; passive science, detumble only |
| PCB-trace magnetorquer | Rejected | 537× below requirement; too large for 2U |
| Iron-core MTQ | Superseded | Replaced by air-core coils (Oct 2026); reason not recorded |
| UV sensor VEML6075 | Dropped Nov 2026 (confirmed) | Reason not recorded |
| 4 of 6 sun sensors (BPW34) | Dropped Nov 2026; channels left unpopulated | Reason not recorded |
| USB webcam | Replaced by Pi Camera, Nov 2026 | Reason not recorded |
| 4-layer ADCS PCB | Not pursued | Reason not recorded |
| HSE crystal (ADCS) | Not fitted | Not needed |
| Flight-model EPS features (RBF, deployment switches, heater, EMI filter, hardware load shedding) | Deferred | EM-first scope (INA219 was moved out of this list and is included for telemetry) |
| TT&C SWD header | Excluded | Debug by UART prints |

## 13. Chronology of revisions

1. **Early/Sep 2026:** component selection and architecture for ADCS, TT&C, EPS, payload; RPi 3A+ assumed; MTQ designed as iron-rod coils; I2C1/I2C2 split; TIA 47 kΩ / 10 pF; VCP 100 nF; MPU-6050 as module; trace-coil idea rejected; PA/LNA removed after link budget; 1,098-line ADCS handover document written.
2. **Sep–Oct 2026:** ADCS schematic complete, PCB layout done; external review (AGND/GND gap, VCP correction, nFAULT pull-ups, I2C pull-up check, per-axis DRV8833 confirmation); test points placed.
3. **Oct 2026 schematic review:** TC2030 SWD header and SWO on PB3; no HSE crystal; fuses on power inputs; 1 Ω AISEN; TIA changed to 680 Ω / 22 pF; I2C3 (PA8/PC9) for the RPi link; 2U declared final; air-core MTQ coils on J1–J3; STM32-independence architecture proposed (with emergency-alert concern); RPi stays powered.
4. **Oct–Nov 2026:** PCBs sent to manufacturing; RPi confirmed as CM4; teammate's EPS report received (unverified); five junior members recruited.
5. **Nov 2026:** UV sensor dropped; form factor re-confirmed as 2U; this master record compiled.
6. **Nov 2026, architecture diagram review:** UV drop confirmed; USB webcam replaced by Pi Camera (payload is now TSL2591 plus Pi Camera, powered through the RPi); INA219 included for EPS telemetry; EPS–OBC control link confirmed; TT&C fixed at 433 MHz; balun added to the RF chain; RFM98W's internal PA/LNA/RF switch kept in the diagram as module functions; shared Tx/Rx filter acknowledged; sun sensors reduced from six to two.

## 14. Verification flags (resolve before report and before the space-ready design)

**A. Gaps — information was never recorded**
1. Mechanical subsystem: structure, materials, mass budget, deployment, panels.
2. Air-core magnetorquer parameters and the dipole requirement it must meet.
3. Reasons for: the RPi 3A+ → CM4 switch, the TIA value change, the I2C2 → I2C3 move, dropping UV, choosing air-core over iron-core, choosing 2U over 3U, reducing sun sensors from six to two, and switching from a USB webcam to a Pi Camera.
4. Link budget numbers (EM distances and LEO), battery capacity, solar panel selection, and power budget.
5. Orbit, mission lifetime, launch provider and deployer.
6. Balun: position in the RF chain, part number, and reason it is required.
7. Sun sensors: which two ADC channels are populated and which faces the two sensors are mounted on.
8. Pi Camera model; INA219 I2C address and shunt resistor value.

**B. Inconsistencies in the project notes**
1. **Mass limit (resolved):** the notes carry "2U / 4 kg" for MTQ sizing, and the original brief said 6 kg (for 3U). Both match CubeSat Design Specification Rev 14 (2 kg per U: 4 kg for 2U, 6 kg for 3U). An earlier version of this flag quoted the older Rev 13 limits by mistake. The limit the launch provider applies still needs confirming before the flight mass budget is built.
2. **AGND/GND join is not connected**, yet the PCBs are already in manufacturing and no new connections can be added. A wire patch is the only route; the exact patch points have not been defined.
3. **Architecture diagram (`cubesat_arc.jpeg`) is out of date** and must be redrawn: change 437 MHz to 433 MHz; remove the UV sensor; relabel "Optical Camera" as Pi Camera; label "Parameter Monitor" as INA219; add the EPS–OBC control (data) link; show payload power coming through the RPi; show two sun sensors; add a legend (green = power, blue = data); draw a satellite outline with the ground station outside it; fix spellings ("Tranceiver" → "Transceiver", "Magnetic Torquer" → "Magnetorquer", "Base Station" → "Ground Station"); confirm "RPi-CM4+" matches the exact CM4 model.
4. **Test-point count:** an earlier note said 17; the latest list totals 20.
5. An older line in the records says ADCS, Payload and TT&C boards each carry an STM32, but the payload has no PCB.

**C. Assistant observations (not project decisions; verify independently)**
1. The RPi CM4 and a commercial LoRa module on 433 MHz are fine for an EM; for a space-ready build, radiation tolerance, thermal limits, and radio frequency licensing and coordination will all need separate treatment.
2. The independent-STM32 architecture needs a defined fault-reporting mechanism (for example a dedicated interrupt line to the RPi), because I2C slaves cannot initiate messages. The pin and board changes this implies cannot be made on the current boards.
