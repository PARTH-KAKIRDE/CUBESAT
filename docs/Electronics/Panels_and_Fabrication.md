# Panels and Fabrication Record

Sources: `PANEL/`, `PANEL_jlc/README.md`, `PANEL_pcbpower/README.md`, the `make_panel.py` scripts, and GIT (d86f692, 22c9e2f). Extracted 2026-11-16.

## 1. Board set

| Board | Outline | Layers | Holes | Designers (board text) | Detail |
|---|---|---|---|---|---|
| ADCS | 85 × 84 mm | 2, 1.6 mm | 4 × Ø4.0, 74 × 76 mm | Parth Kakirde, Pushkar Dube, Arpit Rajput | [ADCS/ADCS_Design_Record.md](ADCS/ADCS_Design_Record.md) |
| EPS (Rev 1.1) | 85 × 84 mm | 2, 1.6 mm | 4 × Ø4.3, 74 × 76 mm | Abhijit Dalal, Krish Sitafale | [EPS/EPS_Design_Record.md](EPS/EPS_Design_Record.md) |
| TT&C (Rev A) | 85 × 84 mm | 2, 1.6 mm | 4 × M4 | Parth Kakirde, Pushkar Dube, Rayon Biswas | [TTC/TTC_Design_Record.md](TTC/TTC_Design_Record.md) |

All three boards use the same 74 × 76 mm hole pattern, so they stack.

## 2. Panel variants

| Folder | Fab house | Size | Frame | Order file |
|---|---|---|---|---|
| `PANEL/` | Original panel (KiCad, `Panel Gerber.zip`) | — | — | `Panel Gerber.zip` |
| `PANEL_jlc/` (GIT d86f692) | JLCPCB | 173.3 × 187.3 mm | 5 mm rails top and bottom | `fab/gerbers.zip` |
| `PANEL_pcbpower/` (GIT 22c9e2f) | PCB Power | 189.3 × 187.3 mm | 5 mm on all four sides | `fab/gerbers.zip` |

**Layout (both variants):**
```
 ADCS (85.2 x 84.2) | EPS (85.05 x 84.05)
 TTC  (85.05 x 84.05) | empty (TTC's SMA points here)
```

- Boards are 3 mm apart, held by 5 mm mouse-bite tabs (0.6 mm holes).
- 3 × 2 mm tooling holes and 3 × 1 mm fiducials.
- The panel copies raise silkscreen text from 0.9 to 1.0 mm (211 texts); the board files are unchanged.
- Generated with KiKit: `python3 PANEL_x/make_panel.py`, then `kikit fab jlcpcb --no-drc PANEL_x/Panel.kicad_pcb PANEL_x/fab`.

## 3. Design-rule compliance (from the READMEs)

| Rule | JLCPCB | PCB Power | Panel |
|---|---|---|---|
| Track / clearance | ≥ 0.10 / 0.10 | 0.15 / 0.15 | 0.15 / 0.15 mm (at PCB Power's limit) |
| Via | hole ≥ 0.15, pad ≥ hole + 0.1 | drill ≥ 0.20, pad ≥ 0.40 | 0.3 / 0.6 mm; 0.20 mm thermal vias (ADCS DRV8833) |
| SMD pad gap | ≥ 0.15 | 0.15 | 0.15 mm (TMP102 on TT&C) |
| Silkscreen | ≥ 1.0 mm / 0.15 stroke | ≥ 1.0 / 0.10 | 1.0 / 0.17 mm |

**Remaining KiCad DRC items (both variants):**
- 8 cosmetic silkscreen overlaps (Q1, Q3, D6, D8, U1, R10/R17, C27).
- 4 flags on the TMP102 pads.
- Library and courtyard warnings from EPS and KiKit footprints.

**Order notes:**
- FR-4, 2 layers, 1.6 mm. HASL lead-free or ENIG (not set in the Gerbers).
- Tell the fab house the panel holds **3 different designs** (JLC may charge extra).
- JLC delivery format: "panel by customer".

## 4. Manufacturing status — UNVERIFIED

| Source | Says |
|---|---|
| ICD v0.9 §0 | "The PCBs are manufactured. The software must fit the boards as built" |
| REC (`docs/fyp_record.md`) | "PCBs sent to manufacturing (Oct–Nov 2026)" / "already in manufacturing" |
| ICD open issue 6, TT&C README | "before ordering" / "not yet ordered" |
| Panel READMEs | Order files exist; no order number, invoice or delivery recorded |

Record the fab house, order date, which panel file was sent and the delivery date before writing the report.

## 5. Open across boards

1. **Mounting hole size:** EPS Ø4.3 mm vs ADCS/TT&C Ø4.0 mm. An M4 rod needs 4.3 mm.
2. **Silkscreen text:** 0.9 mm in the board files (1.0 mm only in the panels).
3. **TT&C:** the final PCB lacks J8, R20, R21, D9 and D10 from the schematic (see the TT&C record §11). If the panel was built from it, the emergency-line stage and the BURN_EN pull-down are missing on the real boards.
