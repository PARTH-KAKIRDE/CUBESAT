# Bring-up checklist (when the soldered boards arrive)

Do the steps in order and don't skip ahead. Each step only needs what the earlier steps proved.

## 0. Before power: EPS (from EPS_Rev1.1_Changes.md)

- [ ] Set the buck modules with **no load connected**: U1 = 5.0 V, U2 = 3.3 V.
- [ ] Check the MP1584's IN+/IN-/OUT+/OUT- against the silkscreen.
- [ ] Check the INA226 module's pin order against J2 and **read its shunt resistor**. Put the value in `[sensors] shunt_ohm` (R100 = 0.1, R010 = 0.01).
- [ ] CN3722: float voltage 8.4 V.
- [ ] Leave the J26 PYRO_ARM plug **out**.

## 1. Pi + Nucleo dummy slave (can be done today)

1. Pi setup: docs/RPI_SETUP.md, sections 1 and 2.
2. Flash `build/arm/dummy_slave/dummy_slave.bin`: copy it onto the `NOD_F446RE` USB drive.
3. Wire it as in WIRING.md (Nucleo section) and run `python3 hw_tests/link_test.py`.
4. If there are errors, switch to the i2c-gpio fallback **before** going further.

## 2. ADCS board

1. Power from the bench supply with a current limit of 100 mA on 3.3 V. Check that the current is sane.
2. Flash over J16 (SWD): `openocd ... program build/arm/adcs/adcs.elf`. Watch the SWO log (ITM port 0, SWO clock 16 MHz core / 2 MHz) in STM32CubeProgrammer or `openocd ... itm port 0 on`.
3. The log should show `mag=1 imu=1` and `mode -> 2` (DETUMBLE).
4. Connect J11 to the Pi and run `python3 -m cubesat_obc status adcs` and `tlm adcs`.
5. **Axis check:**
   - Put the board flat with +Z up and rotate it about +Z by hand, counter-clockwise seen from above. `gyro_dps[2]` must be **positive**.
   - Do the same for X and Y.
   - Point +X north: `mag_uT[0]` should be the largest positive value.
   - Fix any axis in `firmware/adcs/board_config.h` (MAP / SIGN) and re-flash.
6. **Coil polarity:**
   - Command NOMINAL, then `mtq_manual(+500, 0, 0, 3000)`.
   - Hold a compass near the X coil: its north pole should point to +X. If not, flip `MTQ_SIGN[0]`.
   - Repeat for Y and Z.
7. **Hard-iron calibration:**
   - Rotate the board slowly through all orientations while logging `mag_uT`.
   - offset = (max + min) / 2 per axis, in LSB = µT / 0.025.
   - Put it in `[adcs] mag_offset`.
8. Coils on at full duty: check that the driver current and temperature are OK, and that no nFAULT appears in `status adcs`.

## 3. TT&C board

1. Power, flash over J4, connect the J6 UART: the log shows `radio=1 mode=3`.
2. Flash `ground/lilygo_bridge/lilygo_bridge.ino` onto the LilyGO.
3. Run `python ground/gs_tool.py --port COMx listen`. A TT&C beacon (`BEACON_TTC`) should arrive about 5 s after boot, then every 30 s.
4. Connect J5 to the Pi. Run the OBC (`run`), then from the ground station `cmd obc ping` and `cmd obc hk`.
5. Burn wire, **with a resistor in place of the burn wire**:
   - `cmd ttc burn_arm`, then `cmd ttc burn_fire 1000` with the RBF pin out.
   - Check BURN_EN on a scope or LED.
   - With the RBF in, the fire command must be refused (`HW_FAULT`).

## 4. Integration

- [ ] Run the full stack on the bench for 24 h. Check the telemetry file for COMM_FAULT, `obc_i2c_resets` and `local_i2c_errors`.
- [ ] Unplug one slave for a minute: the Pi logs COMM_FAULT once, keeps running, and recovers when it's plugged back in.
- [ ] Pull the Pi's power: ADCS keeps detumbling and TT&C keeps sending its own beacon (`obc_contact = 0` after 30 s).
- [ ] Low battery: lower the bench supply below 6.6 V. Everything goes SAFE; above 7.0 V it returns to NOMINAL.
- [ ] Pyro: with a test load and J26 fitted, set `pyro.enabled = true` and do `cmd eps pyro_arm` then `cmd eps pyro_fire 500`. Check that the IRLZ44N turns fully on at 3.3 V gate drive (see the EPS TODO).
