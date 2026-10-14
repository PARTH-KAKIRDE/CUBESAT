# TTC — Telemetry, Tracking & Command

Radio (communications) module for the 2U CubeSat, LoRa version, Rev A.

## Overview
An STM32F446RET6 (same MCU and footprint as ADCS) drives an RFM98W LoRa module (Semtech SX1278) at 435–438 MHz. It sends telemetry to the ground station (a LilyGO LoRa board running TinyGS), receives commands, and passes data to and from the OBC over I²C (same connector and pins as ADCS) or UART. Power comes from the EPS 3.3 V or 5 V rail (JST-XH, as on ADCS).

The antenna output is either an SMA connector (default) or an LC balun feeding a dipole; fit one of the 0 Ω links R18 or R19.

**Status:** schematic complete. PCB is 84.2 × 84.2 mm, 2-layer, **fully routed** (KiCad DRC: 0 violations, 0 unconnected, schematic parity clean). Outline and four 4 mm mounting holes (75 mm pitch, 4.6 mm from edges) match ADCS and EPS so the boards stack. Design rules are set to JLCPCB standard 2-layer limits (0.127 mm track/space). Not yet checked in 3D for assembly or ordered.

## Layout
- **Floorplan by function.** Power is bottom-left at the J1/J7 inputs. The MCU is centre-left beside the J8 header. The RFM98W radio and its antenna chain sit on the right edge at J3 (SMA) and J9 (dipole). I/O protection parts are directly above J2/J5/J6, and the temperature sensor sits between the MCU and the radio.
- **Spaced for hand soldering and probing.** The median gap to the nearest part is about 2 mm, with about 3 mm around the MCU, radio module and debug pad.
- **Critical paths.** The antenna path is 36 mm long, on the top layer only, with no vias. Each crystal sits next to its MCU pins with its tracks uncrossed, and each decoupling capacitor is at its supply pin.
- **Tracks.** Signals are 0.15 mm, power 0.3 mm, and +3V3 0.2 mm (so it can leave the 0.5 mm-pitch MCU pins). The RF tracks are 0.6 mm.
- **Ground.** GND is poured on both layers, with about 160 stitching vias. Surface-mount GND pads connect solidly to the pour (good for machine assembly, slower to hand-solder).
- **Manufacturing.** There is no top-layer copper under the RFM98W module. The board has 3 fiducials for machine assembly, and teardrops on every pad and via except the fine-pitch U3/U4 pins.

## History
This branch's git history keeps every earlier layout. To get one back, check out that commit's `TTC/TTC_LoRa.kicad_pcb` (or `TTC_auto/TTC_auto.kicad_pcb` for the auto-layout commits).
- `23c6f7e`: first fully routed layout (hand-placed).
- `d20aa38`: the same layout with the crystal re-routed.
- `16187b9`: the same layout with teardrops and antenna-path vias added.
- `f9c4dad`, `deac0fd`, `b9867ab`: auto-placed and auto-routed layouts. The current board is `b9867ab`.

## Contents
- `TTC_LoRa.kicad_sch`: schematic
- `TTC_LoRa.kicad_pcb`: PCB layout (routed)
- `TTC_LoRa.kicad_pro`: project configuration
- `TTC_LoRa.pdf`: schematic PDF export
- `TTC.kicad_sym`, `Library.pretty/`, `sym-lib-table`, `fp-lib-table`: project libraries (TPS7A2033 symbol, Tag-Connect footprint) needed to open the project

## Tool
Designed in KiCad 10. Open `TTC_LoRa.kicad_pro` to view the full project.
