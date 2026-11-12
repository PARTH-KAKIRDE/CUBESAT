# Panel for JLCPCB: ADCS | EPS | TTC

This is a separate, JLCPCB-compliant version of `PANEL_kikit/`. Neither that folder nor the board files were changed.

**Order file:** `fab/gerbers.zip` (Gerbers + drill, JLC layer naming)

## Layout

```
 ┌──────────── 5 mm rail: 2 tooling holes, 2 fiducials, title ────────────┐
 │  ADCS (85.2 x 84.2)   │   EPS (85.05 x 84.05)                           │
 │───────────────────────┼─────────────────────────────────────────────────│
 │  TTC (85.05 x 84.05)  │   (empty: TTC's SMA points here; two support   │
 │                       │    strips run from EPS down to the rail)        │
 └──────────── 5 mm rail: 1 tooling hole, 1 fiducial ──────────────────────┘
```

Panel size is **173.3 × 187.3 mm**, 2 layers, 1.6 mm thick.

## JLCPCB guideline compliance

| Item | JLCPCB guideline | This panel |
|---|---|---|
| Longest side | preferably < 250 mm | 187.3 mm ✅ |
| Mouse-bite holes | 0.6 mm, sets of 5–8, edge-to-edge 0.35–0.4 mm, centreline or one third into the board | 0.6 mm, about 6 per tab, 0.4 mm gap, centre 0.1 mm outside the board edge so the hole reaches 0.2 mm (one third) into it ✅ |
| Tab width | ≥ 5 mm with mouse bites | 5 mm ✅ |
| Board spacing | ≥ 1.6–2 mm | 3 mm ✅ |
| Rails | 5 mm for assembly | 5 mm top and bottom ✅ |
| Tooling holes | 2 mm for assembly, ≥ 3 | 3 × 2.0 mm ✅ |
| Fiducials | 1 mm, centred 3.85 mm from the panel edge | 3 × 1 mm copper, 2 mm mask opening, 3.85 mm ✅ |
| Track / clearance | ≥ 0.10 / 0.10 mm | 0.15 / 0.15 mm ✅ |
| Vias | hole ≥ 0.15 mm, pad ≥ hole + 0.1 mm | 0.3 / 0.6 mm ✅ |
| Smallest drill | ≥ 0.15 mm | 0.20 mm (ADCS DRV8833 thermal vias) ✅ |
| NPTH | ≥ 0.5 mm | 0.6 mm mouse bites ✅ |
| SMD pad to pad | ≥ 0.15 mm | 0.15 mm (TMP102) ✅ |
| Copper to routed edge | ≥ 0.2 mm | passes ✅ |
| Silkscreen text | ≥ 1.0 mm high, ≥ 0.15 mm stroke | 1.0 mm / 0.17 mm. 211 texts were raised from 0.9 mm **in the panel only** ✅ |

KiCad DRC run against the JLC limits shows no violations. What remains:
- **8 silkscreen-overlap warnings,** where enlarged reference text touches a part outline (Q1, Q3, D6, D8, U1, R10/R17, C27). These are cosmetic; JLC prints them as-is.
- **4 flags on the TMP102 (U4) pads,** which come from the panel's 0.2 mm rule. JLC's limit is 0.15 mm, so they're fine.

## JLCPCB order settings

| Setting | Value |
|---|---|
| Base material | FR-4 |
| Layers | 2 |
| Dimensions | 173.3 × 187.3 mm |
| Different designs | **3** (JLC may charge for this) |
| Delivery format | **Panel by customer** |
| Thickness | 1.6 mm |
| Surface finish | HASL (lead-free) or ENIG (the Gerbers don't specify one) |
| Remove order number | optional: "specify a location" needs a `JLCJLCJLCJLC` text added first |

## Regenerate after board edits

```
python3 PANEL_jlc/make_panel.py
kikit fab jlcpcb --no-drc PANEL_jlc/Panel.kicad_pcb PANEL_jlc/fab
```

## Still open (board design, not panel)

- **Mounting holes:** EPS has Ø4.3 mm holes; ADCS and TTC have Ø4.0 mm. An M4 rod needs 4.3 mm.
- **Silkscreen text in the board files** is still 0.9 mm. Only the panel copy was raised to 1.0 mm.
