# ADCS — Attitude Determination and Control System

STM32F446 board that reads the six sun sensors and drives the three magnetorquers (DRV8833 X/Y/Z).

## Contents
- `ADCS.kicad_sch` — Schematic
- `ADCS.kicad_pcb` — PCB layout (84 × 84 mm, 2 layers)
- `ADCS.kicad_pro` — Project configuration
- `fp-lib-table`, `Library.pretty/` — project footprint library (Tag-Connect TC2030 SWD)

## Protection and reliability changes
- Input protection on 5V and 3.3V: PTC fuses (F1 0.75 A, F2 0.5 A), reverse-polarity P-MOSFETs (Q1/Q2 AO3401A), TVS diodes (D1 SMBJ5.0A, D2 SMBJ3.3A)
- ESD diodes (ESD5B5) on the six sun-sensor inputs and both I2C buses (D3–D12)
- Decoupling: 47 µF bulk on 5V, 10 µF + 4.7 µF + 100 nF on 3.3V
- DRV8833 nFAULT lines pulled **up** to 3.3V (R12–R14, 10k); the pin is open-drain
- Current-sense outputs filtered (1k + 100 nF) before the ADC
- I2C pull-ups (4.7k), BOOT0 pull-down (10k)
- Single GND–GNDA tie (R20, 0R)
- Four 4 mm mounting holes, 75 mm spacing

## Checks
KiCad 10.0.6: DRC 0 violations, 0 unconnected pads. Two warnings: the F1/F2 footprints do not match the symbol's footprint filter (footprint is correct).

## Tool
Designed in KiCad 10. Open `ADCS.kicad_pro` to view the full project.
