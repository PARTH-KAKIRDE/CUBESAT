#!/usr/bin/env python3
"""PCB Power panel of ADCS, EPS and TTC (KiKit building blocks). Copy of PANEL_jlc/ plus PCB Power's
recommendation of a 5 mm border on all four sides (full frame instead of top/bottom rails). Tabs on the
two edges that face the empty slot (TTC right edge with the edge-launch SMA, EPS bottom edge) are left
out, so nothing sits under the SMA and no long support strips cross the empty slot.

Original JLC notes:

Changes from PANEL_kikit/ to meet JLCPCB's guidelines:
  - 2 x 2 layout (ADCS | EPS on top, TTC below left, one slot empty) so the longest side is under 250 mm.
    TTC's edge-launch SMA points into the empty slot.
  - Mouse bites 0.6 mm holes, 1.0 mm pitch (0.4 mm edge-to-edge), holes one third into the board edge.
  - Tooling holes 2.0 mm on the 5 mm rails; fiducials 1 mm copper, centred 3.85 mm from the panel edge (voffset 3.8 mm + 0.05 mm outline offset).
  - Silkscreen text below 1.0 mm high is raised to 1.0 mm (stroke 0.17 mm) in the panel only.
Source boards are not modified. Nets get a per-board prefix; reference designators stay unchanged.

Run from the repo root:  python3 PANEL_pcbpower/make_panel.py
"""
import os
import pcbnew
from pcbnew import LoadBoard, VECTOR2I
from kikit import panelize_ui_impl as ki
from kikit.panelize import Panel, Origin
from kikit.panelize_ui import Section
from kikit.units import mm

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BOARDS = [  # (net prefix, board file, (column, row))
    ("ADCS", os.path.join(ROOT, "ADCS/ADCS.kicad_pcb"), (0, 0)),
    ("EPS", os.path.join(ROOT, "EPS/CubeSat_EPS/CubeSat_EPS.kicad_pcb"), (1, 0)),
    ("TTC", os.path.join(ROOT, "TTC/TTC/TTC_LoRa.kicad_pcb"), (0, 1)),
]
OUT = os.path.join(HERE, "Panel.kicad_pcb")
GAP = 3 * mm                     # space between boards = mouse-bite tab length (JLC: >= 1.6-2 mm)
MIN_TEXT_H = 1.0 * mm            # JLC minimum silkscreen text height
TEXT_STROKE = 0.17 * mm          # ~1:6 stroke:height, JLC preferred

sec = lambda s: Section().convert(s, None, None)
preset = ki.obtainPreset([],
    tabs=sec("fixed; vwidth: 5mm; hwidth: 5mm; vcount: 2; hcount: 2"),
    cuts=sec("mousebites; drill: 0.6mm; spacing: 1.0mm; offset: -0.1mm; prolong: 0.5mm"),
    framing=sec("frame; width: 5mm; space: 3mm; cuts: none"),
    tooling=sec("3hole; hoffset: 5mm; voffset: 2.5mm; size: 2mm"),
    fiducials=sec("3fid; hoffset: 12mm; voffset: 3.8mm; coppersize: 1mm; opening: 2mm"),
    text=sec("simple; text: VJTI 2U CubeSat - ADCS | EPS | TTC - panel {date}; anchor: mt; voffset: 2.5mm; "
             "hjustify: center; vjustify: center; width: 1.2mm; height: 1.2mm; thickness: 0.2mm"),
    post=sec("millradius: 1mm; refillzones: true"),
)

boards = [LoadBoard(p) for _, p, _ in BOARDS]
bbs = [b.GetBoardEdgesBoundingBox() for b in boards]   # outline only (ignores the SMA overhang)
colw = [max(bb.GetWidth() for bb, (_, _, (c, r)) in zip(bbs, BOARDS) if c == k) for k in (0, 1)]
rowh = [max(bb.GetHeight() for bb, (_, _, (c, r)) in zip(bbs, BOARDS) if r == k) for k in (0, 1)]

panel = Panel(OUT)
panel.inheritDesignSettings(boards[2])   # TTC: smallest clearance of the three (0.15 mm)
panel.inheritProperties(boards[2])
panel.inheritTitleBlock(boards[2])
panel.inheritLayerNames(boards[2])

for (prefix, path, (c, r)), bb in zip(BOARDS, bbs):
    x = sum(colw[:c]) + c * GAP
    y = sum(rowh[:r]) + r * GAP
    panel.appendBoard(path, VECTOR2I(int(x), int(y)), sourceArea=bb, origin=Origin.TopLeft, tolerance=15 * mm,
                      netRenamer=lambda n, orig, p=prefix: f"{p}-{orig}",
                      refRenamer=lambda n, orig: orig, inheritDrc=True)

substrates = panel.substrates
framingSubstrates = ki.dummyFramingSubstrate(substrates, preset)
panel.buildPartitionLineFromBB(framingSubstrates)
tp = preset["tabs"]
panel.clearTabsAnnotations()
panel.buildTabAnnotationsFixed(tp["hcount"], tp["vcount"], tp["hwidth"], tp["vwidth"], tp["mindistance"], framingSubstrates)
# drop tabs on the edges facing the empty slot: TTC right edge (index 2), EPS bottom edge (index 1)
dropped = 0
for idx, axis in ((2, 0), (1, 1)):
    s = substrates[idx]
    edge = s.bounds()[2 + axis]                       # maxx for axis 0, maxy for axis 1
    keep = [a for a in s.annotations if abs(a.origin[axis] - edge) > 1 * mm]
    dropped += len(s.annotations) - len(keep)
    s.annotations = keep
print("tabs dropped on edges facing the empty slot:", dropped)
tabCuts = panel.buildTabsFromAnnotations(tp["fillet"])
preFrameSubstrate = panel.boardSubstrate.substrates
frameCuts = ki.buildFraming(preset, panel)
ki.buildTabFillets(preset, panel, preFrameSubstrate)
ki.buildTooling(preset, panel)
ki.buildFiducials(preset, panel)
ki.buildText(preset["text"], panel)
ki.buildPostprocessing(preset["post"], panel)
ki.makeTabCuts(preset, panel, tabCuts)
ki.makeOtherCuts(preset, panel, frameCuts)
ki.buildCopperfill(preset["copperfill"], panel)
ki.setStackup(preset["source"], panel)
ki.setPageSize(preset["page"], panel, boards[2])
ki.positionPanel(preset["page"], panel)

# JLC minimum silkscreen text height: raise anything smaller to 1.0 mm (panel copy only)
raised = 0
def fix(t):
    global raised
    if t.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS) and t.IsVisible() and t.GetTextHeight() < MIN_TEXT_H:
        ratio = MIN_TEXT_H / t.GetTextHeight()
        t.SetTextSize(VECTOR2I(int(t.GetTextWidth() * ratio), int(MIN_TEXT_H)))
        t.SetTextThickness(max(t.GetTextThickness(), int(TEXT_STROKE)))
        raised += 1
for f in panel.board.GetFootprints():
    for t in [f.Reference(), f.Value()] + [g for g in f.GraphicalItems() if isinstance(g, pcbnew.PCB_TEXT)]:
        fix(t)
for d in panel.board.GetDrawings():
    if isinstance(d, pcbnew.PCB_TEXT):
        fix(d)
print("silkscreen texts raised to 1.0 mm:", raised)

# KiKit cannot merge project settings of several source boards: use the TTC project as the panel rule set.
panel.sourcePaths = type(panel.sourcePaths)([p for p in panel.sourcePaths if p.endswith("TTC_LoRa.kicad_pcb")])
panel.save()
print("saved", OUT)
