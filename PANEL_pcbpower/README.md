# Panel for PCB Power: ADCS | EPS | TTC

A copy of `PANEL_jlc/` with one change: PCB Power's recommended **5 mm border on all four sides**. Neither `PANEL_jlc/` nor the board files were changed.

**Order file:** `fab/gerbers.zip` (Gerbers + drill)

## Layout

```
 ┌───────────── 5 mm frame on all four sides ────────────┐
 │  ADCS (85.2 x 84.2)   │   EPS (85.05 x 84.05)          │
 │───────────────────────┼────────────────────────────────│
 │  TTC (85.05 x 84.05)  │   (empty: TTC's SMA points     │
 │                       │    here; no tabs on this side) │
 └───────────────────────────────────────────────────────┘
```

- **Size:** 189.3 × 187.3 mm, 2 layers, 1.6 mm thick.
- **Frame:** 5 mm wide, with a 3 mm routed gap to the boards. There are no cuts in the frame corners.
- **Tabs:** 5 mm mouse-bite tabs (0.6 mm holes at 1.0 mm pitch). The two edges that face the empty slot get no tabs (the TTC's right edge with the SMA, and the EPS's bottom edge). Each board is still held on its other three edges.
- **Tooling and fiducials:** 3 × 2 mm tooling holes and 3 × 1 mm fiducials, both on the frame.
- **Silkscreen:** text is 1.0 mm in the panel copy (PCB Power's minimum character height is 1.00 mm).

## PCB Power compliance

| Rule | PCB Power | This panel |
|---|---|---|
| Track / spacing (1 oz) | 0.15 / 0.15 mm | 0.15 / 0.15 mm ✅ (at their limit) |
| Minimum via drill | 0.20 mm | 0.20 mm thermal vias (ADCS), 0.30 mm elsewhere ✅ |
| Annular ring (measured from the drill) | 0.10 mm | 0.10–0.15 mm ✅ |
| Via pad | ≥ 0.40 mm | 0.60 mm ✅ |
| Drill to drill / copper to routed edge | 0.15 / 0.20 mm | passes ✅ |
| Silkscreen | text ≥ 1.0 mm, line ≥ 0.10 mm | 1.0 / 0.17 mm ✅ |
| Maximum board | 430 × 585 mm | 189.3 × 187.3 mm ✅ |
| Gap with break routing | ≥ 2 mm | 3 mm ✅ |
| Border | 5 mm on all four sides (recommended) | 5 mm ✅ |
| Mixed designs | break routing recommended | routed gaps with mouse-bite tabs ✅ |

KiCad DRC shows the same inherited items as `PANEL_jlc/`:
- 4 flags on the TMP102 pads (0.15 mm apart, which equals PCB Power's spacing limit)
- 8 cosmetic silkscreen-overlap warnings
- courtyard and library warnings from the EPS board and KiKit's own footprints

**When ordering,** tell PCB Power the panel contains **3 different designs**.

## Regenerate

```
python3 PANEL_pcbpower/make_panel.py
kikit fab jlcpcb --no-drc PANEL_pcbpower/Panel.kicad_pcb PANEL_pcbpower/fab
```
