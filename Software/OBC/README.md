# OBC flight software (Raspberry Pi 4B)

Python package `OBC`. It is the I2C master for ADCS and TT&C, drives the EPS and the payload directly, and handles ground commands. It uses the ICD next to it (`../ICD`) as the single source of truth: the constants (`ICD/python/icd_defs.py`) and the I2C master (`ICD/python/icd_link.py`) are imported from there, not copied.

Ported from the base repo (`~/Downloads/code/obc/cubesat_obc`) on 2026-11-12, with these changes:
- `EPS.py` replaces `eps.py`;
- `Payload.py` replaces `sensors.py` (the USB camera and the old TSL2591 code are gone);
- the base repo's `frames.py` / `link.py` are replaced by the ICD master;
- built against ICD v0.4, now v0.5 (ADCS gyro offset, see below).

## Layout

```
OBC/
├── obc_app.py        main loop
├── __main__.py       command line
├── factory.py        builds the OBC (Pi or simulation)
├── config.py         default settings
├── EPS/              EPS subsystem
│   └── EPS.py
├── Payload/          payload subsystem
│   └── Payload.py
├── comms/            links to the other boards and to the ground
│   ├── bus.py
│   ├── slaves.py
│   └── ground.py
├── config/obc.toml   the Pi's settings
├── scripts/          Pi setup, STM32 flashing and logs
├── systemd/          boot service
└── tests/
```

| File | What |
|---|---|
| `obc_app.py` | Main loop, once per second (ICD section 9): <br>• GET_STATUS and telemetry from ADCS and TT&C <br>• EPS and payload <br>• ground commands, ACKs and the beacon <br>• one telemetry line per second |
| `EPS/EPS.py` | INA226 #1 (load); power state (sun/eclipse, charging); battery %; low/critical battery; pyro; `EPS GET_STATE`. INA226 #2 is written but commented out (`# BATT: ` lines). |
| `Payload/Payload.py` | TSL2591, Pi camera (background `rpicam-still`), VEML6075 (off), and logging of the ADCS magnetometer. Fresh samples only; ADCS takes them with the coils off. |
| `comms/slaves.py` | ADCS / TT&C clients: commands and telemetry decoding |
| `comms/ground.py` | Ground-link packets: authenticated commands, ACK/TLM, beacons |
| `comms/bus.py` | `/dev/i2c-N` (smbus2) and the PC simulation bus |
| `config.py`, `config/obc.toml` | Defaults and the Pi's configuration |
| `factory.py`, `__main__.py` | Build the OBC for the Pi or the simulation; command line |
| `scripts/` | `setup_pi.sh` (one-time Pi setup); `swd_flash.py` / `swo_log.py` (program the STM32s and read their logs from the Pi) |
| `systemd/cubesat-obc.service` | Runs the OBC at boot, restarts it on a crash or a 30 s hang |
| `tests/` | 114 tests (below) |

## Running (from `Software/`)

```
python3 -m OBC --config OBC/config/obc.toml run      # on the Pi
python3 -m OBC scan                                   # which devices answer
python3 -m OBC status adcs | tlm adcs 0 | mode adcs safe
python3 OBC/EPS/EPS.py --bus 3                        # INA226 bench check
python3 OBC/Payload/Payload.py --bus 3                # payload alone (no magnetometer)
python3 -m OBC --sim run --ticks 30                   # PC simulation (needs CUBESAT_SIM_LIB)
```

Data goes to `/home/pi/cubesat_data`:
- `telemetry/telemetry_<day>.jsonl`
- `payload/payload_<day>.jsonl`
- `images/`
- `ground_<day>.jsonl`
- `adcs_gyro_offset.json` (last gyro offset ADCS measured on the bench)

## Tests

```
cd Software && python3 -B -m pytest -q OBC/tests
```

- **Without the firmware simulation**, 82 tests run: payload, EPS, link and ground codec. The 32 tests that need the simulation (31 OBC-plus-firmware tests and the bench link-test hook) are skipped.
- **With the firmware simulation**, all 114 run. Build `libcubesat_sim.so` from the ADCS/TT&C firmware in `../firmware` with `python3 firmware/build.py sim` (from `Software/`). The tests find it in `Software/build/host/`; `CUBESAT_SIM_LIB` overrides that path.
- **The gyro offset tests** (`test_gyro_offset.py`) boot ADCS still and tumbling with a simulated 2.7 °/s gyro offset: the OBC saves the bench value, re-sends it after a tumbling reset and after an OBC restart, and a manual or ground value always wins.
- **The sun tests** (`test_sun.py`) point the simulated satellite at chosen Sun directions: the ADCS Sun direction and lit faces are right, an eclipse turns the EPS state to ECLIPSE, a broken diode with the Sun behind it gives HIDDEN (never ECLIPSE) and the SUN_DIODE fault, and the light thresholds and disabled faces from obc.toml (or the ground) reach ADCS and survive its reset.
- **The magnetometer test** checks that, through DETUMBLE and manual coil commands, every logged magnetometer sample is fresh and unique, and that the simulator saw no sample taken while a coil was driven.

## Bring-up checklist

The OBC-side items. The full board-by-board procedure is in [../docs/BRINGUP.md](../docs/BRINGUP.md), the Pi setup in [../docs/RPI_SETUP.md](../docs/RPI_SETUP.md).

- [ ] INA226 module: read the shunt marking (R100 = 0.1 Ω, R010 = 0.01 Ω) and set `[sensors] shunt_ohm` and `max_current_a`. R100 can only measure up to 0.82 A; swap to R010 for flight loads.
- [ ] INA226 module: wire its **VBUS pin to IN−** (the EPS J2 socket does not carry it), or the bus voltage reads 0. `python3 OBC/EPS/EPS.py --bus 3` must show the battery voltage.
- [ ] Gyro offset: power ADCS up once on the bench and leave it untouched for 5 s. `python3 -m OBC tlm adcs 3` must show state `AUTO`; the OBC saves it to `<data_dir>/adcs_gyro_offset.json` and re-sends it whenever ADCS boots moving. (`[adcs] gyro_offset = [x, y, z]` forces a value.)
- [ ] `[eps] r_int_ohm`: measure the voltage drop for a known load step.
- [ ] Sun sensors (ADCS fw ≥ 0.4.0 decides sunlit/eclipse itself): set `[adcs] sun_full_scale` (one diode facing the Sun, within ~10 %) and `sun_lit_threshold` (above the albedo glow) from `python3 -m OBC tlm adcs 1` and `tlm adcs 4` in sunlight. `[eps] sun_adc_threshold` is only used with older ADCS firmware.
- [ ] TSL2591 saturates in direct sun without a filter: decide on an ND filter or diffuser.
- [ ] Pi camera: `rpicam-still --list-cameras`.
- [ ] Ground key: generate it on the ground PC and copy it to `/home/pi/.cubesat/ground_key` (`chmod 600`). Never commit it.
