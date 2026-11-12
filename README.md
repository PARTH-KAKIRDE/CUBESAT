# CubeSat software (2U ground prototype)

Software for the OBC (Raspberry Pi 4B), ADCS and TT&C (STM32F446), plus the ground station.

```
icd/        ICD.md + cubesat_icd.h + ground_link.h   <- single source of truth
            icd_crc16.c, icd_pack.h                  shared C (CRC, little-endian packing)
firmware/   STM32 firmware (adcs, ttc, dummy_slave) + PC simulation and unit tests
obc/        Raspberry Pi OBC application (Python): link, slaves, EPS, sensors, scheduler
ground/     gs_tool.py (ground station CLI) + lilygo_bridge (LoRa <-> USB sketch)
tools/      gen_icd_py.py (C header -> Python constants)
docs/       NO_PROGRAMMER.md (start here: Pi-only bring-up), WIRING.md, RPI_SETUP.md, BRINGUP.md
build/      generated output (git-ignored)
```

## Everything at once on this PC (no hardware needed)

```
python tools/gen_icd_py.py --check       # Python constants match the C header
python firmware/build.py check           # STM32 code compiles against the real register headers
python firmware/build.py test            # firmware unit tests + 50-minute detumble simulation
python firmware/build.py sim             # simulation library for the OBC tests
cd obc && python -m pytest -q            # OBC tests against the real firmware logic
python obc/hw_tests/link_test.py --sim   # the deliverable-5 link tests, simulated
python ground/gs_tool.py --sim cmd obc hk
```

## System overview

```
 ground station (LilyGO + gs_tool.py)
        |  LoRa 436.5 MHz, packets in icd/ground_link.h
     TT&C (STM32)  ---- I2C 0x33 ----+
                                      +---- Raspberry Pi 4B (OBC, master, polls at 1 Hz)
     ADCS (STM32)  ---- I2C 0x32 ----+        | i2c-3: INA226 (EPS), TSL2591
                                               | GPIO17: pyro    USB: camera
```

Start with `icd/ICD.md`. Wiring and bring-up instructions are in `docs/`.
