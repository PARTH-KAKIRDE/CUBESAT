# CubeSat EPS: Rev 1.1 changes and TODO

This is the EPS project you sent (Rev 1.0, dated 15/16 Oct), returned with review fixes applied. The issues came from Parth's review and a full schematic/PCB check. Open `CubeSat_EPS.kicad_pro` in **KiCad 10**.

## Verification

| Check | Rev 1.0 | Rev 1.1 |
|---|---|---|
| ERC errors | 4 | **0** |
| DRC violations (with schematic parity) | 4 warnings (missing libraries) | **0** |
| Unconnected pads | 0 | 0 |

Three ERC warnings remain, of type "symbol doesn't match library", for R1, Q1 and C1. They are formatting-only. Clear them with **Tools → Update Symbols from Library**.

---

## Changes

### Safety and damage risks

1. **MP1584 (U2) pin labels were swapped.**
   - In Rev 1.0, pin 1 was labelled IN-, yet it was wired to VBUS_DIST, and OUT+ was wired to GND.
   - The symbol pins are renamed to match the wiring: 1 = IN+, 2 = IN-, 3 = OUT+, 4 = OUT-.
   - IN+/IN-/OUT+/OUT- are added to the U2 silkscreen.
   - The board text over U2 said "5V Buck"; it now says **"3V3 Buck"**.

2. **Pyro arm inhibit added.**
   - New **J26 PYRO_ARM** (JST VH 2-pin), placed in series between +5V and the pyro terminal J25 (new net `PYRO_ARMED`).
   - With no shorting plug in J26, the pyro cannot fire. Treat it as a remove-before-flight plug.
   - The firing net is renamed from `Ras-pie_input` to **`PYRO_FIRE`**.

3. **C1 (solar input) was a 100 µF 0805.** Those parts are usually rated 6.3 V, which is below the panel voltage.
   - C1 is now **100 µF 50 V aluminium electrolytic** (`CP_Elec_6.3x7.7`, polarized symbol).
   - It **moved** to the free area left of U2, at about (108, 81) mm.
   - + connects to MPPT_IN+ through 2 vias and a short B.Cu link; – goes to GND vias.

4. **Q1 reverse-polarity P-FET was drawn with source and drain swapped** (the symbol was GSD).
   - The symbol is now `Q_PMOS_GDS`, value **IRF4905** (TO-220, G-D-S).
   - The copper didn't change: pad 2 (drain) is on the solar side and pad 3 (source) on MPPT_IN+.
   - **D5** (12 V zener, now 1SMA5927) was connected gate–drain. Its cathode now goes to the **source (MPPT_IN+)**, so it actually clamps Vgs. On the PCB, its old track to Q1 was removed and D5 pad 1 is tied to D1 pad 1.

### Ratings and values

5. **Capacitor values:**
   - C3: 100 µF **10 V**, now 1206.
   - C5: 100 µF 6.3 V.
   - C2, C4, C6: 100 nF 50 V.
6. **D6:** now **SS34**.
7. **F6** (MP1584 output): 3 A → **2 A**, the module's continuous rating.
8. **R15:** value written as `100`.

### Routing

9. **J10 AUX output track:** widened from 0.2 mm to **1.0 mm**. The old track would have burned before F4 blew.
10. **Pyro `/DRAIN` tracks:** widened to **1.2 mm** (F.Cu was 0.5 mm and B.Cu 0.8 mm).
11. **Mounting holes H2 and H4:** snapped onto the 74 × 76 mm pattern, which matches ADCS and TT&C. They were 25–50 µm off.

### ERC and housekeeping

12. **PWR_FLAGs** added on the fuse-fed inputs (U1 Vin, U2 IN+, U3 Vin).
13. **Libraries bundled.**
    - `815-2.pretty`, `MP1584.pretty`, `MP1584.kicad_sym` and `yaaj_dcdc_stepdown_lm2596.kicad_sym` are now inside the project folder.
    - They're registered in the project `fp-lib-table` and `sym-lib-table`, so the project opens on any machine without missing footprints.
14. **Connector names:**
    - J12 → `ADCS_5V_A`
    - J13 → `ADCS_5V_B`
    - J8 → `ADCS_3V3_POWER`
15. **Silkscreen and title block:**
    - VIN/GND/VOUT labels added to the U1 (XL4015) silkscreen.
    - Notes block added on the schematic.
    - Title block → **Rev 1.1**.

---

## TODO before ordering or powering up

**Needs information:**
- [ ] **D1 TVS** (currently `TVS TODO`): pick a part with standoff V_WM > the panel's max Voc **and** clamp V_C < 28 V (the CN3722's maximum input). We need the panel's Voc for this.
- [ ] **F1** (currently `TODO 1.5xIsc`): size it to about 1.5 × panel Isc, with a voltage rating ≥ Voc.
- [ ] **F4** (currently `TODO max 2A`): size it to the AUX load. The track is good for up to about 2 A.
- [ ] **F5** (5 A, 5 V rail): confirm it against the OBC's needs (a Pi CM4 can draw about 3 A) and the XL4015's real capability.

**Check on the real hardware, before soldering:**
- [ ] **MP1584 module:** check its IN+/IN-/OUT+/OUT- against the new silkscreen. If it's reversed, VBUS shorts to GND.
- [ ] **INA226 module (J2):** compare its pin order with J2. If pin 3 or 4 is **VBS**, it must connect to VIN-, or the bus voltage reads 0.
- [ ] **INA226 shunt:** many modules use R100, which is only 0.82 A full scale. Swap it for R010–R020 and update the calibration register.
- [ ] **CN3722 module:** check the float voltage is **8.4 V** (2S Li-ion), and check the reverse leakage from the battery during eclipse.
- [ ] **Buck modules:** set U1 = **5.0 V** and U2 = **3.3 V** with no load connected, *before* plugging anything in.

**Test or decide:**
- [ ] **Q3 IRLZ44N** is specified at Vgs 4–5 V, but the Pi drives it at 3.3 V. Test that it fully turns on at the real pyro current, or add a 5 V gate-drive stage.
- [ ] **Firmware:** drive `PYRO_FIRE` from a Pi GPIO in the **9–27** range (pull-down at boot). Avoid GPIO 0–8, which pull up at boot.
- [ ] **Mounting holes:** these are 4.3 mm and ADCS uses 4.0 mm. Agree on one size for the stack.

**Cosmetic:**
- [ ] On the schematic, the C1/C2 value text overlaps D1's label slightly; drag it clear.
- [ ] Open the PCB and check that you're happy with the new C1 and J26 placement.
