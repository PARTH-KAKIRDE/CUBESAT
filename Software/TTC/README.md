# TT&C firmware (STM32F446, fw 0.4.2, ICD v0.9)

The radio board. It owns the LoRa link (an RFM98W / SX1278), sends every beacon, passes ground packets to and from the OBC over I2C (address 0x33), deploys the antenna once at boot, and executes **emergency commands** by itself when the OBC (Pi) is hung. Register-level C, no HAL.

Build, test and flash from `Software/` (the build system and the shared STM32 code are in `../firmware`):

```
python3 OBC/scripts/keygen.py             # once: ~/.cubesat/ground_key + ~/.cubesat/ttc_key
python3 firmware/build.py arm ttc         # -> build/arm/ttc/ttc.elf (.bin, .hex), key from ~/.cubesat/ttc_key
python3 firmware/build.py arm ttc_notx    # bench build that never transmits (no antenna)
python3 firmware/build.py test            # host unit tests, TT&C logic in the simulator
python3 OBC/scripts/swd_flash.py ttc      # flash from the Pi (docs/NO_PROGRAMMER.md); keeps flash sector 7
python3 -m OBC tlm ttc                    # read the telemetry pages from the Pi
```

## Files

| File | What |
|---|---|
| `ttc_app.c/.h` | The application: radio scheduling, queues (RX, TX, emergency priority), beacons, burn wire and automatic deployment, emergency commands and EVENT reports, SET_RADIO confirm/revert, ICD commands and telemetry. **Portable**: no registers, only `ttc_hw.h`. |
| `ttc_nv.c/.h` | Flag store: an append-only journal in flash sector 7 (deployed flag, burn count, emergency counter, OBC watchdog setting, pending reset report). Portable. |
| `hmac_sha256.c/.h` | SHA-256 / HMAC-SHA256 for emergency-command MACs (tested against FIPS 180-4 / RFC 4231 vectors). Portable. |
| `ttc_hw.h` | The hardware interface the app uses. Two implementations: `hw_stm32.c` + `sx1278.c` (the board) and `../firmware/host/ttc_hw_sim.c` (radio as packet queues, simulated flash and lines, optional half-duplex model). |
| `hw_stm32.c` | The board: TMP102, deploy switch, RBF, burn-wire enable, LED, the three emergency lines (PC6 Pi RUN, PC8 Pi GLOBAL_EN, PC7 ADCS NRST), flash sector 7 driver, system reset. Pin map from `TTC_LoRa.kicad_pcb`. |
| `sx1278.c` | LoRa driver over SPI1: split frequency (RX on the uplink, retune to the downlink for each TX), PA_BOOST power, packets up to 255 bytes, "packet coming in" detection. |
| `main.c` | Boot (watchdog first, then `ttc_init`, then the I2C slave) and the main loop. |

Shared code used from elsewhere: `../ICD/slave/` (I2C framing, common commands, modes, status flags) and `../firmware/stm32/` (startup, clock, watchdog, I2C slave/master, SPI1, log). The emergency key is generated into `build/arm/ttc/ttc_key.c` by `build.py` (never in the repo); the host tests use the public test key in `../firmware/host/ttc_key_test.c`.

## How it works

- **Main loop:** `ttc_step()` = ICD link, beacon timer, radio task, burn task, deploy task, emergency task, housekeeping; then the I2C stuck-bus check, the log and a watchdog kick (4 s). Nothing blocks: the radio is polled, never waited on.
- **Radio:** receives on 433.800 MHz, transmits on 433.300 MHz (10 dBm, SF9, 125 kHz, CR 4/8). Emergency ACKs and EVENTs go first; after every other packet it listens 150 ms so the ground can get a command in.
- **Ground commands:** normal ones are only filtered (satellite ID, length, CRC) and queued for the OBC, which checks the MAC. Emergency ones (`GL_T_ECMD`) are checked here with the TT&C key and executed at once.
- **Boot procedure:** flag store loaded; boot EVENT sent; if the antenna-deployed flag is not set, burn once the RBF plug has been out 60 s (5 s, up to 3 times in the satellite's life).

The full behaviour (commands, modes, packets) is in [`../ICD/ICD.md`](../ICD/ICD.md) sections 6, 8 and 11.

## Bench checks before trusting it

- Deploy switch and RBF polarity: `TTC_DEPLOYED_LEVEL` / `TTC_RBF_REMOVED_LEVEL` in `ttc_app.h` (automatic deployment relies on them).
- The emergency lines need the J8 header and three 2N7002 on the board (not on the routed PCB yet, see `../../TTC/TTC/README.md`).
- Check that releasing Pi GLOBAL_EN restarts the Pi 4B.
