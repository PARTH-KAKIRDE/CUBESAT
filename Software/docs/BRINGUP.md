# Bring-up checklist (when the soldered boards arrive)

Do the steps in order and don't skip ahead. Each step only needs what the earlier steps proved. Commands run from `Software/` (on the Pi: `/home/pi/cubesat/Software`).

No programmer? [NO_PROGRAMMER.md](NO_PROGRAMMER.md) does steps 1–2 with only the Pi.

## 0. Before power: EPS (from `EPS/CubeSat_EPS/EPS_Rev1.1_Changes.md`)

- [ ] Set the buck modules with **no load connected**: U1 = 5.0 V, U2 = 3.3 V.
- [ ] Check the MP1584's IN+/IN-/OUT+/OUT- against the silkscreen.
- [ ] Check the INA226 module's pin order against J2 and **read its shunt resistor**. Put the value in `OBC/config/obc.toml`, `[sensors] shunt_ohm` (R100 = 0.1, R010 = 0.01), and set `max_current_a` to match.
- [ ] INA226 module: wire its VBUS pin to IN−. Then `python3 OBC/EPS/EPS.py --bus 3` must show the battery voltage.
- [ ] CN3722: float voltage 8.4 V.
- [ ] Leave the J26 PYRO_ARM plug **out**.

## 1. Pi + Nucleo dummy slave (can be done today)

1. Pi setup: [RPI_SETUP.md](RPI_SETUP.md), sections 1 and 2.
2. Build and flash the dummy slave: `python3 firmware/build.py arm dummy_slave`, then copy `build/arm/dummy_slave/dummy_slave.bin` onto the `NOD_F446RE` USB drive.
3. Wire it as in [WIRING.md](WIRING.md) (Nucleo section) and run `python3 ICD/python/test_link.py --bus 1 --addr 0x32`. Expect `15/15 passed`.
4. If there are errors, switch to the i2c-gpio fallback **before** going further.

## 2. ADCS board

1. Power from the bench supply with a current limit of 100 mA on 3.3 V. Check that the current is sane.
2. Flash over J16 (SWD): `python3 OBC/scripts/swd_flash.py adcs`, or with an ST-LINK `openocd ... program build/arm/adcs/adcs.elf`. Watch the log: `python3 OBC/scripts/swo_log.py` (SWO, 115200 baud NRZ, ITM port 0).
3. The log should show `mag=1 imu=1` and `mode -> 2` (DETUMBLE).
4. Connect J11 to the Pi and run `python3 -m OBC status adcs` and `python3 -m OBC tlm adcs 0`.
5. **Gyro offset:** power-cycle ADCS and keep it untouched on the bench for the first 5 s. Then `python3 -m OBC tlm adcs 3` should show `state_name: AUTO`, an offset of a few °/s at most, `gyro_spread_dps` ≤ 0.5 and `mag_spread_uT` ≤ 0.5. `REJECTED` means it moved (or the magnetometer is missing): power-cycle and try again. With the OBC running, it saves this value and re-sends it whenever ADCS later boots while moving.
6. **Axis check:**
   - Put the board flat with +Z up and rotate it about +Z by hand, counter-clockwise seen from above. The Z gyro rate must be **positive**.
   - Do the same for X and Y.
   - Point +X north: the X magnetic field should be the largest positive value.
   - Fix any axis in `ADCS/board_config.h` (`MAG_MAP`/`MAG_SIGN`, `GYRO_MAP`/`GYRO_SIGN`) and re-flash.
7. **Coil polarity:**
   - Stop the OBC first (`sudo systemctl stop cubesat-obc`): there must be only one I2C master. Command NOMINAL, then a manual coil pulse, X at +500 ‰ for 3000 ms. There is no command-line verb for the pulse yet, so use this one-liner (it prints `0` = OK; `5` = INVALID_MODE means ADCS is not in NOMINAL):

     ```bash
     python3 -m OBC mode adcs nominal
     python3 -c "import OBC; from icd_link import SlaveLink; from OBC.comms.bus import LinuxI2CBus; from OBC.comms.slaves import AdcsClient; print(AdcsClient(SlaveLink(LinuxI2CBus(1), 0x32, 'ADCS')).mtq_manual(500, 0, 0, 3000))"
     ```
   - Hold a compass near the X coil: its north pole should point to +X. If not, flip `MTQ_SIGN[0]` in `board_config.h`.
   - Repeat for Y and Z.
8. **Hard-iron calibration:**
   - Rotate the board slowly through all orientations while logging the magnetometer (`python3 -m OBC tlm adcs 0`).
   - offset = (max + min) / 2 per axis, in LSB = µT / 0.025.
   - Put it in `OBC/config/obc.toml`, `[adcs] mag_offset`. The OBC sends it to ADCS at start-up and after every ADCS reset.
9. **Sun sensors** (BPW34, all six faces):
   - Shine a lamp on each face in turn. `python3 -m OBC tlm adcs 1` shows which SS channel rises; set the channel-to-face map in `ADCS/board_config.h`. `tlm adcs 4` must then list that face in `lit_faces`.
   - In sunlight, with one face square to the Sun: its count is `[adcs] sun_full_scale`. Get it within ~10 %: dead-diode detection compares the total light against it.
   - Facing a bright diffuse surface (the stand-in for Earth albedo): the count must stay under `sun_lit_threshold`; raise it if not.
   - Cover every face: `tlm adcs 4` must show `lit: false, hidden: false` (eclipse), and `dead_faces: []`.
   - Unplug one diode and turn its face slowly towards the lamp: within ~10 s it appears in `dead_faces` (fault SUN_DIODE), and with the lamp straight on that face `hidden: true` (not eclipse). Plug it back: it leaves `dead_faces` once lit.
10. Coils on at full duty: check that the driver current and temperature are OK, and that no nFAULT appears in `python3 -m OBC status adcs`.

## 3. TT&C board

1. Power, flash over J4 (`python3 OBC/scripts/swd_flash.py build/arm/ttc_notx/ttc_notx.elf` until an antenna or dummy load is fitted), connect the J6 UART: the log shows `radio=1 mode=3`.
2. Flash the LilyGO ground-station bridge (`ground/lilygo_bridge/lilygo_bridge.ino`, still in the base repo).
3. With the normal `ttc` build and an antenna or dummy load: run the ground tool's `listen` (`ground/gs_tool.py`, base repo). A TT&C beacon (`BEACON_TTC`) should arrive about 5 s after boot, then every 30 s.
4. Connect J5 to the Pi. Run the OBC (`python3 -m OBC run`), then from the ground station `cmd obc ping` and `cmd obc hk`.
5. Burn wire, **with a resistor in place of the burn wire**:
   - `cmd ttc burn_arm`, then `cmd ttc burn_fire 1000` with the RBF pin out.
   - Check BURN_EN on a scope or LED.
   - With the RBF in, the fire command must be refused (`HW_FAULT`).

## 4. Payload (Pi only)

- [ ] `rpicam-still --list-cameras` shows the Pi camera.
- [ ] `python3 OBC/Payload/Payload.py --bus 3` logs TSL2591 readings and takes images (no magnetometer in this mode).
- [ ] TSL2591 saturates in direct sun without a filter: decide on an ND filter or diffuser.
- [ ] The magnetometer is logged from ADCS telemetry only while the OBC runs; ADCS samples it with the coils off.

## 5. Integration

- [ ] Run the full stack on the bench for 24 h. Check the telemetry file for COMM_FAULT, `obc_i2c_resets` and `local_i2c_errors`.
- [ ] Unplug one slave for a minute: the Pi logs COMM_FAULT once, keeps running, and recovers when it's plugged back in.
- [ ] Pull the Pi's power: ADCS keeps detumbling and TT&C keeps sending its own beacon (`obc_contact = 0` after 30 s).
- [ ] Low battery: lower the bench supply below 6.6 V. Everything goes SAFE; above 7.0 V it returns to NOMINAL (`[eps] low_batt_mv`, `recover_mv`).
- [ ] Pyro: with a test load and J26 fitted, set `[pyro] enabled = true` and do `cmd eps pyro_arm` then `cmd eps pyro_fire 500`. Check that the IRLZ44N turns fully on at 3.3 V gate drive (see the EPS TODO).
- [ ] EPS state from the ground (`GL_EPS_GET_STATE`, ICD v0.4): the base repo's ground tool does not send it yet.
