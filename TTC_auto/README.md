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
2. **Placement.** A simulated-annealing optimiser keeps the antenna chain short, places each crystal next to its MCU pins (pad order matched, so the tracks don't cross) and pulls each decoupling capacitor to its supply pin. It leaves about 0.8 mm between small parts and 2 mm around the MCU and radio module.
3. **Routing.** Freerouting 2.4.1 did the bulk of it. Hand fixes:
   - the antenna and balun tracks were kept on the top layer with no vias;
   - R16 was moved so U4's supply pin could connect.
4. **Finishing:**
   - no top-layer copper under the RFM98W module;
   - a ground via at each local ground patch;
   - 175 stitching vias;
   - 3 fiducials for machine assembly;
   - teardrops, except on the fine-pitch U3/U4 pins.

## Status
KiCad DRC: 0 violations, 0 unconnected pads, schematic parity clean.

| | `../TTC/` | `TTC_auto/` |
|---|---|---|
| Antenna path | 34.1 mm, 0 vias | 35.1 mm, 0 vias |
| Crystal tracks | 68.2 mm, 4 vias | 51.9 mm, 4 vias |
| Signal vias | 96 | 80 |

## Before using it
- +3V3 is 0.2 mm wide (0.3 mm in `../TTC/`), because 0.3 mm cannot leave the MCU's 0.5 mm-pitch pins.
- Surface-mount GND pads connect solidly to the pour. That is fine for machine assembly but harder to hand-solder.
- It has only been checked with DRC. Review the 3D view and part spacing in KiCad before ordering.

The previous git commit holds a more compact version of this board, with parts packed tightly round the MCU.
