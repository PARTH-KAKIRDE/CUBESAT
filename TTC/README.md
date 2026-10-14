# TTC — Telemetry, Tracking & Command

Radio (communications) module for the 2U CubeSat, LoRa version, Rev A.

## Overview
An STM32F446RET6 (same MCU and footprint as ADCS) drives an RFM98W LoRa module (Semtech SX1278) at 435–438 MHz. It sends telemetry to the ground station (a LilyGO LoRa board running TinyGS), receives commands, and passes data to and from the OBC over I²C (same connector and pins as ADCS) or UART. Power comes from the EPS 3.3 V or 5 V rail (JST-XH, as on ADCS).

The antenna output is either an SMA connector (default) or an LC balun feeding a dipole; fit one of the 0 Ω links R18 or R19.

**Status:** schematic complete. PCB is 84.2 × 84.2 mm, 2-layer, **fully routed** (DRC: 0 violations, 0 unconnected, schematic parity clean; ERC clean). Outline and four 4 mm mounting holes (75 mm pitch, 4.6 mm from edges) match ADCS and EPS so the boards stack. Tracks: 0.15 mm signal, 0.3 mm power, 0.6 mm RF; GND poured on both layers with stitching vias. Design rules set to JLCPCB standard 2-layer limits (0.127 mm track/space).

## Review changes (branch `rayon/ttc-v2`)
- 8 MHz crystal Y1 rotated 180° so PH0/PH1 no longer cross: crystal tracks 37.1 → 28.5 mm.
- Teardrops on 470 pad/via joints (vibration robustness).
- 9 extra GND vias along the antenna and balun tracks.
- DRC "missing courtyard" check switched on (no issues found).
- KiCad DRC after the changes: 0 violations, 0 unconnected, schematic parity clean.

An automatically placed and routed alternative of this board is in `../TTC_auto/`.

## Contents
- `TTC_LoRa.kicad_sch`: schematic
- `TTC_LoRa.kicad_pcb`: PCB layout (routed)
- `TTC_LoRa.kicad_pro`: project configuration
- `TTC_LoRa.pdf`: schematic PDF export
- `TTC.kicad_sym`, `Library.pretty/`, `sym-lib-table`, `fp-lib-table`: project libraries (TPS7A2033 symbol, Tag-Connect footprint) needed to open the project

## Tool
Designed in KiCad 10. Open `TTC_LoRa.kicad_pro` to view the full project.
