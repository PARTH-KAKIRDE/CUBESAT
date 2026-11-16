# EPS Design Record — Evidence-Based Extraction

Extracted 2026-11-16 from the repository as it stands on `main` (c368f2c), the same way as the TT&C record.

## 0. Ground rules

- Evidence only.
- **Status tags:** DESIGNED, IMPLEMENTED, SIM-TESTED, TESTED, REVISED, REJECTED, PENDING, UNVERIFIED.
- **Source tags:**
  - SCH: `EPS/CubeSat_EPS/CubeSat_EPS.kicad_sch`
  - PCB: `EPS/CubeSat_EPS/CubeSat_EPS.kicad_pcb`
  - REV: `EPS/CubeSat_EPS/EPS_Rev1.1_Changes.md`
  - SW: `Software/OBC/EPS/EPS.py` and `Software/OBC/config/obc.toml`
  - ICD: `Software/ICD/ICD.md` v0.9
  - REC: `docs/fyp_record.md`
  - PWR: `docs/CubeSat EPS — Power Need and Solar Panel Selection.pdf`
  - NOTES: `docs/Mission_Notes.md`
  - GIT: commit history
  - MYCALC: my own calculation, labelled.
- **Method:** the KiCad files were parsed as text. KiCad was not run. REV reports ERC 0 / DRC 0 for Rev 1.1; I did not re-run them.

## 1. Sources

| Source | What it is |
|---|---|
| SCH / PCB | "CubeSat EPS", **Rev 1.1**, VJTI, "Electrical Power System". Board text: "EPS PCB 10/2026 VJTI — Abhijit Dalal, Krish Sitafale" |
| REV | Rev 1.0 → 1.1 review fixes and a TODO list (KiCad 10 project) |
| `EPS/CubeSat_EPS/CubeSat_EPS.pdf` | Schematic PDF |
| `EPS/CubeSat_EPS.zip` | Zipped copy of the project (not compared) |
| Bundled libraries | `815-2.pretty` (INA226, MPPT, XL4015 footprints), `MP1584.pretty`, `MP1584.kicad_sym`, `yaaj_dcdc_stepdown_lm2596.kicad_sym` |
| SW | OBC-side EPS driver (the EPS has no MCU) |
| PWR | Power need and solar panel selection (Oct 10, 2026). This is the teammate's report that REC says is **not yet verified** |

## 2. Purpose and architecture

- Generates, stores and distributes power for the 2U EM.
- **The EPS has no MCU** (ICD, SW, REC). The OBC (Pi) reads the INA226 over I2C and drives the pyro gate from a GPIO.
- NOTES still lists a "Custom STM32-managed EPS board"; that idea was REJECTED (REC §4.3: "No dedicated EPS MCU").

**Power chain, as built (PCB nets):**
```
SOLAR_IN J1 -> F1 (TODO) -> Q1 IRF4905 reverse-polarity P-FET -> MPPT_IN+ (C1 100uF/50V, C2 100nF, D1 TVS TODO)
           -> U3 CN3722 2S MPPT charger module -> VBUS
Battery 2S J4 (BATT+, BATT_MID, BATT-) <-> J3 BMS module -> VBUS      (battery sits on the bus)
VBUS -> J2 INA226 module shunt (IN+ = VBUS, IN- = VBUS_DIST) -> VBUS_DIST
VBUS_DIST -> F2 5A   -> U1 XL4015 buck  -> F5 5A -> +5V
VBUS_DIST -> F3 2.5A -> U2 MP1584 buck  -> F6 2A -> +3V3
VBUS_DIST -> F4 (TODO, max 2A)          -> J10 AUX_POWER_OUT
```

- **The bus is the battery voltage, 2S Li-ion: about 6.2–8.4 V** (SW thresholds).
- The charger and the BMS switch between sun and battery on their own (SW docstring). The pack and BMS are off-board modules.
- **The 3.3 V buck runs from VBUS_DIST, not from 5 V.** The two rails are independent.
- **Load switching:** none on the board. Loads are behind fuses only. REC §8 lists "load switches" in the chain and "hardware load shedding" as deferred to the flight model.

## 3. Outputs and connectors

| Ref | Name | Pins (net) | Feeds |
|---|---|---|---|
| J1 | SOLAR_IN (JST-VH 2) | 1 solar+, 2 GND | Solar array |
| J4 | BATTERY_PACK (JST-VH 3) | BATT+, BATT_MID, BATT− | 2S pack (balance tap) |
| J3 | BMS_MODULE (JST-VH 5) | BATT+, BATT_MID, BATT−, VBUS, GND | BMS module |
| J2 | INA226_MODULE (8-pin module footprint) | 1 VBUS, 2 VBUS_DIST, 3 NC, 4 NC, 5 SDA, 6 SCL, 7 GND, 8 +3V3 | INA226 module (0x40) |
| J6 | OBC_INTERFACE (JST-VH 5) | 1 +5V, 2 GND, 3 SDA, 4 SCL, 5 PYRO_FIRE | Raspberry Pi (power, sensor I2C, pyro GPIO) |
| J8 | ADCS_3V3_POWER (JST-VH 2) | +3V3, GND | ADCS J14 |
| J9 | TTC_POWER (JST-VH 2) | +3V3, GND | TT&C J1 (3.3 V path) |
| J12, J13 | ADCS_5V_A / ADCS_5V_B (JST-VH 2) | +5V, GND | ADCS J15 (coil drivers) |
| J10 | AUX_POWER_OUT (JST-VH 2) | VBUS_DIST via F4, GND | Spare, at bus voltage |
| J26 | PYRO_ARM (JST-VH 2) | +5V → PYRO_ARMED | Remove-before-flight shorting plug |
| J25 | Pyro (screw terminal 2) | DRAIN, PYRO_ARMED | Burn element |
| TP1–TP4 | Test points | VBUS, +5V, +3V3, GND | |

- The I2C pull-ups for the Pi sensor bus are on the EPS: R2/R3 4.7 kΩ to +3V3.
- ICD: the INA226 is on the Pi **sensor** bus `/dev/i2c-3` (GPIO4/5), separate from the STM32 link bus.

## 4. Bill of materials (SCH, 47 parts incl. 4 mounting holes)

| Ref | Value | Package | Function |
|---|---|---|---|
| U3 | CN3722 2S MPPT module | Module footprint | Solar MPPT charger |
| U1 | XL4015 buck module | Module footprint | 5 V rail (set to 5.0 V before load) |
| U2 | MP1584 buck module | 20 × 15 mm module | 3.3 V rail (set to 3.3 V before load) |
| J2 | INA226 module | 8-pin | Bus voltage and load current |
| Q1 | IRF4905 | TO-220 | Reverse-polarity P-FET (solar input) |
| D5 | 12 V zener 1SMA5927 | SMA | Q1 gate–source clamp |
| R1 | 100 kΩ | 0805 | Q1 gate pull-down |
| D1 | **TVS TODO** | SOD-123 | Solar input clamp |
| C1 | 100 µF 50 V electrolytic | 6.3 × 7.7 | Solar input bulk |
| C2, C4, C6 | 100 nF 50 V | 0805 | HF decoupling on MPPT_IN+, +5V, +3V3 |
| C3 | 100 µF 10 V | 1206 | +5V bulk |
| C5 | 100 µF 6.3 V | 0805 | +3V3 bulk |
| Q3 | IRLZ44N | TO-220 | Pyro low-side switch |
| R15 | 100 Ω | 0805 | Pyro gate series |
| R10 | 10 kΩ | 0805 | Pyro gate pull-down |
| D6 | SS34 | SMA | Flyback across the pyro load (DRAIN → +5V) |
| D8 + R4 | Red LED + 220 Ω | 0805 | +5V power indicator |
| R2, R3 | 4.7 kΩ | 0805 | I2C pull-ups |
| F1 | **TODO 1.5 × Isc** | 1206 | Solar input |
| F2 | 5 A | 1206 | XL4015 input |
| F3 | 2.5 A | 1206 | MP1584 input |
| F4 | **TODO max 2 A** | 1206 | AUX output |
| F5 | 5 A | 1206 | +5V output |
| F6 | 2 A | 1206 | +3V3 output (module continuous rating) |
| H1–H4 | M4, Ø4.3 mm | | 74 × 76 mm pattern |

Schematic ↔ PCB: all references match and all values match (parsed check).

## 5. Circuits

### 5.1 Solar input and reverse polarity [DESIGNED, REVISED in Rev 1.1]

- **Q1 IRF4905 (P-FET, G-D-S):** the drain is on the solar side (after F1) and the source on MPPT_IN+. The gate is pulled to GND by R1 100 kΩ.
- **D5 12 V zener** between the source (cathode) and the gate (anode) clamps Vgs. Rev 1.1 moved its cathode from the drain to the source "so it actually clamps Vgs".
- **C1:** Rev 1.0 had 100 µF in 0805, typically 6.3 V-rated and below the panel voltage. Rev 1.1 changed it to a 100 µF 50 V electrolytic.
- **D1 TVS — TODO.** The part must have standoff V_WM > panel Voc **and** clamp V_C < 28 V (the CN3722 maximum, per REV). The footprint is SOD-123, which limits it to a small (~200–400 W) TVS.

### 5.2 MPPT charger [DESIGNED; check on hardware]

- **CN3722 2S module.** The float voltage must be checked at **8.4 V** (REV TODO), along with the reverse leakage from the battery in eclipse.
- **Input limit, conflict:** REV gives the CN3722 maximum as 28 V; PWR says "rated about 24 V". Use the module's datasheet.

### 5.3 Battery and BMS [DESIGNED]

- 2S Li-ion with a balance tap, through an off-board BMS module (J3/J4).
- **Capacity: not recorded** (REC §14 flag 4; SW `capacity_mah` is commented out).
- **SW thresholds on VBUS** (`obc.toml [eps]`):

| Setting | Value | Meaning |
|---|---|---|
| low_batt_mv | 6600 (3.30 V/cell) | OBC goes SAFE |
| recover_mv | 7000 | Back to NOMINAL |
| critical_mv | 6200 | Sync the SD card (BMS cut-off near) |
| full_mv | 8300 | Charger in CV = full |
| r_int_ohm | 0.15 | Pack + BMS resistance, to measure at bring-up |

### 5.4 Power monitoring [DESIGNED; IMPLEMENTED in SW]

- The INA226 module's shunt is in series between VBUS and VBUS_DIST, so it **sees only the load current** (the whole satellite). It sees no solar or battery current.
- SW works the power state out from other inputs:
  - sun/eclipse from the ADCS sun sensors and the payload TSL2591;
  - charging from the voltage trend corrected by I_load × R_int;
  - SOC from a Li-ion rest-voltage curve (rough).
- **Shunt:** most modules ship with R100 (0.1 Ω), which gives only 81.92 mV / 0.1 Ω = **0.82 A full scale**. The satellite can draw about 2 A at low battery, so swap the shunt for R010–R020 and set `[sensors] shunt_ohm / max_current_a` (REV TODO, SW, ICD open issue 3).
- **VBUS pin:** J2 pin 3 is not connected. The module's VBUS pin must be wired to IN− on the module itself, or the bus voltage reads about 0 V (SW flag `GL_EF_VBUS_IMPLAUSIBLE`).
- **Pin order check (MYCALC, UNVERIFIED — check before soldering):**
  - SW describes the module as `VCC GND SDA SCL ALE VBUS IN- IN+`.
  - Mapping that order onto J2 pins 8 → 1 gives pin 8 = VCC ✔, pin 7 = GND ✔, **pin 6 = SDA and pin 5 = SCL**.
  - The PCB has **pin 5 = SDA and pin 6 = SCL**. If the module really uses that pin order, SDA and SCL are swapped, and the fix is a crossed pair of wires to the module.
  - REV already lists "compare its pin order with J2" as a TODO.
- **Second INA226 (0x41) for battery current:** PENDING, not fitted. It would be spliced into the battery cable before J4, with no PCB change (SW, ICD).
- **Part, conflict:** REC §8 says INA219; SCH, SW and ICD say INA226. NOTES also says INA219.

### 5.5 Regulators [DESIGNED]

- **U1 XL4015** (5 V): F2 5 A in, F5 5 A out.
  - REV TODO: check 5 A against the OBC's needs ("a Pi CM4 can draw about 3 A") and the module's real capability.
- **U2 MP1584** (3.3 V): F3 2.5 A in, F6 2 A out.
  - Rev 1.1 fixed **swapped pin labels**: Rev 1.0 had IN+ wired to GND, which would have shorted.
  - REV TODO: check the module's IN+/IN−/OUT+/OUT− against the silkscreen before soldering.
- **Both modules are adjustable.** Set U1 to 5.0 V and U2 to 3.3 V with **no load** before plugging anything in (SCH notes).

### 5.6 Pyro (antenna or deployment burn) [DESIGNED; check on hardware]

**Circuit:**
- Pi GPIO → J6 pin 5 `PYRO_FIRE` → R15 100 Ω → Q3 IRLZ44N gate, with R10 10 kΩ pull-down.
- The load sits between `PYRO_ARMED` and `DRAIN` on J25. `PYRO_ARMED` is +5V through the **J26 arm plug**.
- D6 SS34 is the flyback diode.

**Safety (Rev 1.1):**
- Without the J26 shorting plug the pyro cannot fire (remove-before-flight).
- The net was renamed from `Ras-pie_input` to `PYRO_FIRE`.

**GPIO choice:**
- SCH notes: drive `PYRO_FIRE` from a Pi GPIO in **9–27** (boot pull-down), not 0–8 (boot pull-up).
- SW names GPIO17.

**Gate drive — TODO:** the IRLZ44N is specified at Vgs 4–5 V, but the Pi drives 3.3 V. Test full enhancement at the real burn current, or add a 5 V gate driver (REV).

**Tracks:** `/DRAIN` was widened to 1.2 mm in Rev 1.1.

**Relation to TT&C, conflict:** TT&C fw v0.9 also has its own burn-wire output (BURN_EN PC2, to an "external MOSFET"). Two burn paths now exist on paper (EPS pyro from the Pi, and TT&C BURN_EN). Decide which one drives the antenna release.

### 5.7 Indicator [DESIGNED]

- D8 red LED from +5V through R4 220 Ω.
- MYCALC: about (5 − 2) / 220 ≈ 14 mA, or about 70 mW all the time. Consider removing it or raising R4 for power-budget runs.

## 6. PCB [DESIGNED]

| Item | Value |
|---|---|
| Outline | 85 × 84 mm, 2 layers, 1.6 mm FR-4 (KiCad default stackup) |
| Tracks | 126 segments. Widths: 0.2 (38), 0.5 (1), 0.7 (14), 1.0 (23), 1.2 (7), 1.5 (19), 1.6 (9), 1.8 (9), 2.3 (1), 2.5 (5) mm |
| Power net widths | Solar in 2.5 mm; Q1 drain / XL4015 in / VBUS 1.8 mm; VBUS_DIST 1.0–1.6 mm; +5V 1.5 mm; XL4015 out 2.5 mm; MP1584 out 2.3 mm; battery 1.5 mm; PYRO_ARMED 1.5 mm; DRAIN 1.2 mm; AUX 1.0 mm |
| Vias | 15 (0.6/0.3 × 9, 0.8/0.4 × 5, 1.0/0.5 × 1); smallest drill 0.8 mm (module pins) |
| Zones | 4 on F.Cu, 1 on B.Cu |
| Mounting | 4 × Ø4.3 mm (M4) on 74 × 76 mm, matching ADCS and TT&C (Rev 1.1 snapped H2/H4 by 25–50 µm) |
| Board text | "5V Buck" (U1), "3V3 Buck" (U2; it read "5V Buck" in Rev 1.0) |

**Rev 1.1 routing fixes:**
- The J10 AUX track went from 0.2 mm to 1.0 mm ("would have burned before F4 blew").
- The pyro DRAIN track went to 1.2 mm.

**MYCALC (IPC-2221, 1 oz, external, 10 °C rise, approximate):** 1.0 mm carries about 2.3 A, 1.5 mm about 3.1 A, 2.5 mm about 4.5 A. Check the 1.5 mm +5V track against the 5 A F5 fuse; at 5 A it would run about 20 °C or more above ambient.

## 7. Power budget (PWR — teammate report, UNVERIFIED per REC)

| Situation | Power | Current at 7.4 V |
|---|---|---|
| Normal running | ~6 W | ~0.9 A |
| Worst case | ~8.5 W | ~1.3 A |

**Solar array target:**
- About 10 W just to run the satellite, 20 W to run and charge.
- Vmp 10.5–18 V (above 8.4 V full battery, with headroom), Voc ≤ 22 V, Isc ≤ 3 A.
- Series first, then parallel strings (each with its own Schottky).
- Never use a "24 V" 60-cell panel (Voc ≈ 37 V).
- Easiest buy: one 18 V / 20 W panel. A demo with only charging: 2 × 6 V / 3 W in series.

**Not documented:**
- The breakdown behind the 6 W / 8.5 W figures.
- Battery capacity.
- Orbit-average generation (eclipse fraction, cell efficiency, panel area).
- MTQ current draw. REC: "the power budget cannot be finalized until MTQ current draw is characterized"; the ADCS coils are capped at 200 mA each by the drivers.

**NOTES formulas for orbit sizing (not yet applied):**
- I_s = I₀ cos θᵢ
- A = P_reqd / (I₀ cos θᵢ · η)

**Mission diode note (NOTES):** a Schottky on the solar input is enough for the prototype. The board uses a P-FET (Q1) instead, which has a lower drop.

## 8. Software side (OBC) [IMPLEMENTED, SIM-TESTED]

- `OBC/EPS/EPS.py`:
  - INA226 at 0x40.
  - 64-sample averaging, 1.1 ms conversions, a new result about every 141 ms.
  - `CAL = 0.00512 / (I_max/32768 · R_shunt)`.
  - Clip detection.
  - Power-state estimation (§5.4).
  - SAFE below `low_batt_mv`.
- **Ground commands:** `GL_EPS_GET` (8 B) and `GL_EPS_GET_STATE` (24 B, ICD v0.4): battery V, load I, battery I, solar power, SOC, state, flags, sun flags, min/max, energy.
- **Bench check:** `python3 OBC/EPS/EPS.py --bus 3 [--shunt 0.1 --max-current 0.8]`.
- **Tests:** `OBC/tests/test_eps.py` (PC). No hardware run is recorded.
- **Stale reference:** the SW docstring calls the INA226 header "J11" and the hardware "EPS Rev 1.0". The board has J2 and is Rev 1.1.

## 9. Revision history

| Date | Commit / record | Change |
|---|---|---|
| 2026-09-17 | e8a2bcf, 303590c (abhijitdalal26) | EPS KiCad project and README added |
| Oct 2026 (PWR) | Teammate report | Power need and solar panel selection |
| 2026-10-12/13 | 922c820, d93a0fe, 0d56fb1 (abhijitdalal26) | Files moved to `EPS/CubeSat_EPS/`; dimensions; warning fix |
| 2026-10-14 | fd52e61 (vijay) | "Update EPS PCB" |
| 15/16 Oct 2026 | REV | Rev 1.0 |
| by 2026-10-23 | 504b046 (Parth Kakirde) | **Rev 1.1** in "Add final PCB designs". It fixed MP1584 pin labels, added the pyro arm plug J26, C1 → 50 V electrolytic, Q1 pin order and D5 clamp, capacitor ratings, D6 SS34, F6 2 A, wider AUX and pyro tracks, hole snap, PWR_FLAGs and bundled libraries. ERC 4 → 0 errors, DRC 0 |
| Nov 2026 | REC §13 | INA219 "included" for telemetry (the design uses an INA226) |

## 10. Testing and validation

| Item | Class | Evidence |
|---|---|---|
| ERC / DRC Rev 1.1 | Tool check | REV: ERC 0 errors (3 library-mismatch warnings), DRC 0 |
| EPS.py logic (power state, clipping, thresholds, GET_STATE) | SIM-TESTED | `OBC/tests/test_eps.py` |
| Module voltages, MPPT float, INA226 reading, pyro firing, thermal | NOT TESTED | No record |

## 11. Open items (REV TODO + extraction)

1. **D1 TVS part:** needs the panel Voc.
2. **F1:** about 1.5 × panel Isc.
3. **F4:** sized to the AUX load (track good to about 2 A).
4. **F5 5 A:** check against the OBC draw and the XL4015.
5. **MP1584 module pinout** vs silkscreen.
6. **INA226 module:** pin order vs J2 (possible SDA/SCL swap, §5.4), VBUS wire, shunt swap to R010–R020.
7. **CN3722:** 8.4 V float, eclipse leakage, input maximum (28 V vs 24 V).
8. **Buck set-points** before any load.
9. **Q3 at 3.3 V gate drive.**
10. **Pyro GPIO:** 9–27 (SW: GPIO17).
11. **Mounting holes:** Ø4.3 mm here vs Ø4.0 mm on ADCS/TT&C. Agree on one size.
12. **Battery capacity and verified power budget** (PWR unverified).
13. **Two burn paths** (EPS pyro vs TT&C BURN_EN): pick one.
14. **INA219 vs INA226 naming** in REC and NOTES.
15. **OBC model:** Pi 4B (ICD, SW) vs CM4 (REC). This affects the 5 V current budget.
16. **Cosmetic (REV):** C1/C2 value text overlaps the D1 label in the schematic.
