# CubeSat Documentation

Source material for the project report.

| File | Contents |
|---|---|
| [Mission_Notes.md](Mission_Notes.md) | Early mission notes: 2U form factor, subsystems, payload, EPS/ADCS theory, PCB stack layout, STM32F446 pin options, component list, orbit choice |
| [Electronics/TTC/TTC_Design_Record.md](Electronics/TTC/TTC_Design_Record.md) | Evidence-based TT&C record: architecture, RFM98W/SX1278 config, RF front end, frequencies, protocol, telemetry/telecommand, link budget, PCB, firmware, ground station, test status, design history, contradictions |
| Electronics/ADCS/ | (to add) |
| Electronics/EPS/ | (to add) |
| Mechanical/ | (to add) |

Software docs live in [`../Software`](../Software) (ICD, OBC, ADCS, TTC, CHANGELOG).

## Open items to resolve before writing the report

From the TT&C record (§19–20):

1. Original link-budget numbers, or real antenna gains for a recompute.
2. Antenna decision (turnstile / fixed / dipole / deployable).
3. OBC model: RPi CM4 vs Pi 4B (Mission_Notes also mentions RPi 3A+).
4. Whether the boards were ordered, and from which file.
5. Datasheets for the exact RFM98W variant.
6. Regulatory route: licence-free 433 MHz vs amateur 435–438 MHz.
7. Mass: 4 kg (Mission_Notes) vs ~2 kg (ADCS estimate) vs 6 kg (original brief).
