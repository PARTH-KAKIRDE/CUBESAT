# CubeSat Documentation

Source material for the project report.

| File | Contents |
|---|---|
| [fyp_record.md](fyp_record.md) | Master project record: decisions, revisions and reasons, as recorded by the electronics lead |
| [Mission_Notes.md](Mission_Notes.md) | Early mission notes: 2U form factor, subsystems, payload, EPS/ADCS theory, PCB stack layout, STM32F446 pin options, component list, orbit choice |
| [Electronics/TTC/TTC_Design_Record.md](Electronics/TTC/TTC_Design_Record.md) | TT&C: architecture, RFM98W/SX1278 config, RF front end, frequencies, protocol, telemetry/telecommand, link budget, PCB, firmware, ground station, test status, history, contradictions |
| [Electronics/ADCS/ADCS_Design_Record.md](Electronics/ADCS/ADCS_Design_Record.md) | ADCS: BOM, pin map, coil drivers, sun-sensor TIAs, sensors, PCB, firmware (B-dot, sun algorithm), test status, history, contradictions |
| [Electronics/EPS/EPS_Design_Record.md](Electronics/EPS/EPS_Design_Record.md) | EPS Rev 1.1: power chain, connectors, BOM, protection, INA226 monitoring, pyro, PCB, power budget, OBC software, TODOs |
| [Electronics/Panels_and_Fabrication.md](Electronics/Panels_and_Fabrication.md) | Board set, JLCPCB / PCB Power panels, design-rule compliance, manufacturing status |
| CubeSat EPS — Power Need and Solar Panel Selection.pdf | Teammate's power budget and solar panel choice (unverified) |
| Solar-panel.pdf, CubeSat_Project_Report_vertical_reflowed.pdf, Latest desgin.jpeg | Earlier report draft, solar panel notes, latest design image |
| Mechanical/ | (to add) |

Software docs live in [`../Software`](../Software): ICD, OBC, ADCS, TTC, CHANGELOG, BRINGUP, WIRING.

## Open items to resolve before writing the report

**System:**
1. OBC model: Pi 4B (ICD, software) vs CM4 (fyp_record) vs 3A+ (Mission_Notes).
2. Mass: 4 kg limit (CDS Rev 14 for 2U) vs ~2 kg ADCS estimate.
3. Whether the boards were ordered, from which panel file, and when they arrived.
4. Mounting holes: Ø4.3 mm (EPS) vs Ø4.0 mm (ADCS, TT&C).
5. Clock: both STM32 boards run from the internal 16 MHz HSI (shared `firmware/stm32/platform.c`), even though TT&C has an 8 MHz HSE crystal fitted.

**TT&C** (record §19–20):
6. Link budget numbers, or real antenna gains for a recompute.
7. Antenna decision (turnstile / fixed / dipole / deployable).
8. Regulatory route: licence-free 433 MHz vs amateur 435–438 MHz.
9. Final PCB missing J8, R20, R21, D9, D10.

**ADCS** (record §14):
10. Sun sensors: six (board, firmware) vs two (fyp_record).
11. Air-core coil design and dipole requirement (0.01 vs 0.05 A·m²); 200 mA driver cap.
12. Photodiode orientation in the harness (cathode to the op-amp input).

**EPS** (record §11):
13. Panel Voc/Isc, for the D1 TVS and the F1 fuse; battery capacity; verified power budget.
14. INA226 module pin order vs J2 (possible SDA/SCL swap), VBUS wire, shunt value.
15. Two burn paths (EPS pyro from the Pi vs TT&C BURN_EN): pick one.
