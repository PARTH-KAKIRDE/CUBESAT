# CubeSat software

Flight software for the OBC (Raspberry Pi 4B) and the two STM32F446 boards (ADCS, TT&C), and the interface between them. Run every command from this folder.

```
Software/
├── ICD/        interface control document v0.9: headers (source of truth), Python master, slave link layer
├── OBC/        OBC flight software (Python package): main loop, EPS, payload, links to ADCS / TT&C / ground
├── ADCS/       ADCS firmware (STM32F446): detumbling, sun direction / eclipse, sensors, coils, gyro offset
├── TTC/        TT&C firmware (STM32F446): LoRa radio, beacons, antenna deployment, emergency commands
├── firmware/   build system, shared STM32 code, Nucleo dummy slave, PC simulation and firmware tests
├── docs/       Pi setup, wiring, bring-up checklists
└── build/      firmware build output (git-ignored, created by firmware/build.py)
```

| Folder | Read first |
|---|---|
| `ICD/` | [ICD/ICD.md](ICD/ICD.md) (the interface), [ICD/README.md](ICD/README.md) (files, rules) |
| `OBC/` | [OBC/README.md](OBC/README.md) |
| `ADCS/` | [ADCS/README.md](ADCS/README.md) |
| `TTC/` | [TTC/README.md](TTC/README.md) |
| `firmware/` | [firmware/README.md](firmware/README.md) |
| Changes | [CHANGELOG.md](CHANGELOG.md) |
| `docs/` | [RPI_SETUP.md](docs/RPI_SETUP.md), [WIRING.md](docs/WIRING.md), [BRINGUP.md](docs/BRINGUP.md), [NO_PROGRAMMER.md](docs/NO_PROGRAMMER.md) |

`ICD/cubesat_icd.h` and `ICD/ground_link.h` are the single source of truth. The firmware compiles them directly, and the OBC imports the generated `ICD/python/icd_defs.py`. Nothing is copied.

## Build and test on a PC

Needs `gcc` and Python 3.11+ with `pytest`.

```
python3 firmware/build.py test                      # firmware unit tests: 1616 checks
python3 firmware/build.py check                     # compile-check all STM32 files: 47 files
python3 firmware/build.py sim                       # build/host/libcubesat_sim.so (real firmware logic, simulated hardware)
python3 -B -m pytest -q OBC/tests                   # 114 OBC tests (32 of them need the sim library)
python3 -B -m pytest -q ICD/tests                   # 52 ICD tests
python3 ICD/tools/gen_icd_py.py --check             # icd_defs.py matches the headers
python3 ICD/python/test_link.py --sim-lib build/host/libcubesat_sim.so   # link test, 15 checks
python3 -m OBC --sim run --ticks 30                 # the whole OBC against the simulated boards
```

On a PC the simulated run logs two expected errors, no ground key (`/home/pi/.cubesat/ground_key`) and a camera timeout (no Pi camera), and one expected warning: no stored gyro offset (the simulated ADCS boots tumbling, so it cannot measure one). Its data goes to a temporary folder (`/tmp/cubesat_sim_*`).

`python3 firmware/build.py arm all` builds the real firmware. It needs `arm-none-eabi-gcc` (see `firmware/README.md`).

## On the Pi

[docs/RPI_SETUP.md](docs/RPI_SETUP.md) covers the copy, packages, `config.txt`, link test and the boot service. The code lives at `/home/pi/cubesat/Software`.

## Not here yet

- **Ground station** for the prototype (Raspberry Pi + RFM98W, laptop over the network): next. The old tool and LilyGO bridge in the base repo (`~/Downloads/code/ground`) predate ICD v0.4 and use the old 436.5 MHz. Keys are already made by `OBC/scripts/keygen.py`; `OBC/comms/ground.py` and `arq.py` hold the packet, counter and file-download code it will use.
