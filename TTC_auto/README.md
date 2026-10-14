# TTC_auto: auto-placed, auto-routed TTC board

An alternative layout of the TTC (LoRa) board in `../TTC/`. Same schematic and parts, placed and routed again from scratch. Treat it as an experiment to compare against `../TTC/`, not a replacement yet.

## How it was made
1. **Floorplan by function.** Parts are grouped into five blocks, each in its own area of the board:
   - **Power:** bottom-left, next to the J1/J7 inputs.
   - **MCU:** centre-left, beside the J8 header.
   - **RF:** right edge, by the J3 SMA and J9 dipole connectors.
   - **I/O protection:** directly above J2/J5/J6.
   - **Sensor:** between the MCU and the radio.

   Connectors, J8 and the mounting holes are fixed in the same places as `../TTC/`.
2. **Placement.** A simulated-annealing optimiser keeps the antenna chain short, places each crystal next to its MCU pins (pad order matched, so the tracks don't cross) and pulls each decoupling capacitor to its supply pin. Parts are spread out for soldering and debugging: at least about 1.2 mm between small parts, about 3 mm around the MCU, radio module and debug pad, and a soft penalty for any part within 2.5 mm of a neighbour. The MCU and RF blocks use the top strip of the board.
3. **Routing.** Freerouting 2.4.1 routed all nets, with the bottom layer weighted as more costly so it stays mostly ground. The antenna and balun tracks ended up on the top layer with no vias. The only hand fix was shrinking one via to 0.58 mm to meet clearance.
4. **Finishing:**
   - no top-layer copper under the RFM98W module;
   - about 160 GND stitching vias on a 5 mm grid;
   - 3 fiducials for machine assembly;
   - teardrops, except on the fine-pitch U3/U4 pins.

## Status
KiCad DRC: 0 violations, 0 unconnected pads, schematic parity clean.

| | `../TTC/` | `TTC_auto/` |
|---|---|---|
| Median gap to nearest part | 0.95 mm | 2.03 mm |
| Parts closer than 1 mm to a neighbour | 47 | 5 |
| Antenna path | 34.1 mm, 0 vias | 36.3 mm, 0 vias |
| Crystal tracks | 68.2 mm, 4 vias | 52.4 mm, 5 vias |
| Signal vias | 96 | 83 |

## Before using it
- +3V3 is 0.2 mm wide (0.3 mm in `../TTC/`), because 0.3 mm cannot leave the MCU's 0.5 mm-pitch pins.
- Surface-mount GND pads connect solidly to the pour. That is fine for machine assembly but harder to hand-solder.
- It has only been checked with DRC. Review the 3D view and part spacing in KiCad before ordering.

Earlier versions are in git history: the compact version (commit f9c4dad) and the first block-floorplanned version (commit deac0fd).
