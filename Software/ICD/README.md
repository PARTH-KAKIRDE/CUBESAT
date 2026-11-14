# CubeSat ICD v0.7 (v0.1 + v0.2 merged, plus v0.4–v0.7 additions)

The interface between the OBC (Raspberry Pi 4B) and the ADCS / TT&C STM32 boards, plus the ground radio link. It merges the v0.1 draft (no longer kept in this repo) and v0.2 (the base repo's `~/Downloads/code/icd/`), keeping the better version of each part; v0.4–v0.6 then added constants only (v0.7 also gives page-4 byte 9, reserved in v0.6, a meaning). Section "What came from where" below lists every choice.

**Drop-in:** the file and constant names follow v0.2, so v0.2 code builds against these headers unchanged. The firmware in `../firmware` (ported from the base repo) built against v0.4 with no code change; v0.5 adds the ADCS gyro offset. Current results: 1354 firmware checks (1353 in the 5-face build), the 43-file compile check, 114 OBC tests and the 15-check link test pass. (The v0.3 merge was first tested in the base repo, which had 39 OBC tests then.)

## Files

| File | What |
|---|---|
| `ICD.md` | The interface control document (read this first) |
| `cubesat_icd.h` | I2C link: addresses, commands, status codes, modes, flags, telemetry offsets. **Source of truth.** |
| `ground_link.h` | Radio link packet format, authentication, ground-forwarding rules. **Source of truth.** |
| `icd_crc16.c/.h` | CRC-16/CCITT-FALSE in C (one-shot and streaming) |
| `icd_pack.h` | Little-endian field packing and saturating conversions (C) |
| `tools/gen_icd_py.py` | Generates `python/icd_defs.py` from the headers; `--check` fails if it is stale |
| `python/icd_defs.py` | Generated constants. Never edit by hand. |
| `python/icd_link.py` | OBC master: CRC, frames, transactions, re-reads, lost-reply recovery, COMM_FAULT. Standard library only. |
| `python/test_link.py` | Link acceptance tests (deliverables 4 and 5) for the Pi, or the firmware simulation |
| `slave/icd_link.c/.h` | Slave framing: interrupt byte hooks, BUSY staging, read shadow buffer. No hardware access. |
| `slave/icd_node.c/.h` | Slave common commands, mode rules, status flags, sticky faults |
| `slave/icd_port.h` | The two platform hooks the slave code needs (interrupt lock/unlock) |
| `tests/` | Tests for this folder (`test_icd.py`, with `host_slave.c` building the slave code on a PC) |

The STM32 register-level I2C driver that calls the `slave/` hooks lives in `../firmware/stm32/i2c_slave.c`, with the rest of the firmware. `../firmware/build.py` compiles the `slave/` code and `icd_crc16.c` straight from this folder.

## Use

```
python tools/gen_icd_py.py            # after editing a header (then update ICD.md and the version)
python tools/gen_icd_py.py --check
python -m pytest -q tests             # needs pytest and gcc

# On the Pi (link bus 1, ADCS address); --addr 0x33 for TT&C, --bus 11 for the bit-banged bus
python3 python/test_link.py
# On a PC, against the firmware simulation (from Software/: python3 firmware/build.py sim)
python3 python/test_link.py --sim-lib ../build/host/libcubesat_sim.so
```

## Rules

- `cubesat_icd.h` and `ground_link.h` are the single source of truth. Never redefine their values elsewhere.
- Headers contain only `#define NAME <integer>` lines with an optional `/* comment */`. The generator stops with an error on anything else.
- After a header change: regenerate, update `ICD.md`, bump `ICD_VERSION_MINOR` (compatible) or `ICD_VERSION_MAJOR` (breaking). Changes go through Parth.
- No floats or structs on the wire: pack field by field, little-endian.

## What came from where

**Taken from v0.2** (better, or matches the boards as built):

| Item | v0.1 | v0.3 (from v0.2) | Why |
|---|---|---|---|
| Gyro scale | ±250 °/s, 131 LSB/°/s | ±500 °/s, 65.5 LSB/°/s | The firmware sets ±500; ±250 was too small for the tumble rate (v0.1 open issue 6). |
| ADCS page 1 bytes 12–17 | Coil current u16 (TBD) | Commanded duty i16 | AISEN goes to ground through 1 Ω and is not connected to the STM32: current cannot be measured. |
| ADCS fault bits | 2 drivers, 4 bits | 3 drivers X/Y/Z + I2C + sun ADC, 8 bits | The board has three DRV8833s and three nFAULT pins. |
| Status codes | 0x00–0x07; QUEUE_FULL proposed as 0x08 | + 0x08 BAD_PARAM, 0x09 QUEUE_FULL | BUSY no longer doubles as "queue full" (v0.1 open issue 3); out-of-range values get their own code. |
| Idle frame at boot | CMD 0x00, BUSY | CMD 0x00, NO_DATA | BUSY should only mean "answer not ready"; an idle slave has no answer pending. |
| Unknown telemetry page | NO_DATA (proposed) | BAD_PARAM | NO_DATA means "valid request, nothing yet"; a wrong page number is a bad parameter. |
| Subsystem commands, TT&C pages, modes, addresses, pins | TBD | Defined | Filled in by the owners. |
| Ground link | none | `ground_link.h` with HMAC + counter authentication | New in v0.2. |
| CRC in C | one-shot | one-shot + streaming `icd_crc16_update` | Superset. |
| `icd_pack.h` | none | packing + saturation helpers | Enforces the "no structs on the wire" rule. |
| Python constants | parsed from the header at run time; unreadable lines skipped silently | generated file, `--check`, generator stops on any unreadable line | A missing constant fails at build time, not in flight. |
| Master retry rules | bad CRC on read = failed transaction | re-read on bad CRC/LEN/echo; re-send once after BAD_CRC/BAD_LENGTH; lost-reply recovery read | Fewer false COMM_FAULTs, and no lost reset flags or ground packets. |
| Slave code | HAL interrupt glue + PING/GET_VERSION only | portable framing + all common commands, status flags, sticky faults, mode rules | Tested in the firmware simulation. The v0.1 HAL glue busy-waited in the main loop for up to 10 ms of the 20 ms reply budget and was never run on hardware, so it was not carried over. |
| Debug | TT&C has no SWD | TT&C has SWD on J4 | From the KiCad files. |
| Bus split, address check | TBD | sensors on i2c-3, only INA226 on EPS I2C | Done. |

**Taken from v0.1** (missing or weaker in v0.2):

| Item | Added to v0.3 |
|---|---|
| Frame byte offsets | `ICD_CMD_OFS_*`, `ICD_RESP_OFS_*` (used by `python/icd_link.py`) |
| Command ranges | `ICD_CMD_ADCS_FIRST/LAST`, `ICD_CMD_TTC_FIRST/LAST` (checked by the tests) |
| Poll period | `ICD_POLL_PERIOD_MS` (plus `ICD_SLOW_POLL_MS` for the 10 s pages) |
| Reserved status bits | `ICD_SF_RESERVED_MASK`, "bits 4–7 sent as 0" |
| uptime_ms wrap | noted on both page-0 uptime fields and as open issue 5 |
| Document conventions | status line, TBD / unverified markers, scope section, verification table, open-issues list |
| Explicit validation order | slave: length → CRC → payload; rejected frames are not master contact. Pi: 32 bytes, LEN, CRC, echo. |
| Response budget | 5 + 3 × 5 = 20 ms, why slave main loops must not block |
| Address table | with the TSL2591 0x28 caveat and the reserved address ranges |
| Version check rule | warn on a minor mismatch (v0.2 only checked major) |
| Python master, standard library only | `I2CDev` on `/dev/i2c-N` (no smbus2 needed on a fresh Pi), `bus_speed_hz()`, `crc_selftest()`, exact-32-byte read check |
| Link test cases | bus-speed check; six bad-frame cases (flipped CRC, flipped payload bit, LEN > 24, shorter than LEN, PING with payload, unknown command); slowest-failure time check in the unplug test; error histogram in the soak test |

**Fixed while merging:**
- Bit-banged bus delay: v0.1 used `i2c_gpio_delay_us=5` ("about 100 kHz", from the kernel's 500/delay formula). The Raspberry Pi overlay documentation says the default `2` is about 100 kHz on the Pi, so v0.3 keeps v0.2's `2` and marks the real speed as to be measured.
- v0.2's link test took TT&C telemetry page 0 for the dummy slave's counters (both are 24 bytes) and printed nonsense. v0.3 checks the firmware version first.

## Version history

- **v0.1** (draft): framing, CRC, status codes, common commands, ADCS page 0/1 proposal, dummy slave with HAL glue, CM4 test script.
- **v0.2**: hardware facts filled in, ADCS and TT&C commands and pages, BAD_PARAM and QUEUE_FULL, ground link, lost-reply guards, strict generator.
- **v0.3**: this merge. Compatible with v0.2 (only constants added; no value changed except `ICD_VERSION_MINOR`). Slaves built against v0.2 work with a v0.3 master, which only warns about the minor version.
- **v0.4** (2026-11-12): EPS GET_STATE ground command (`GL_EPSS_*`, `GL_PS_*`, `GL_EF_*`, `GL_SUNF_*`); INA226 #2 and VEML6075 addresses added commented out; camera is the Pi camera (CSI). Only constants added: a v0.3 slave works with a v0.4 master (minor-version warning only).
- **v0.5** (2026-11-14): ADCS gyro zero-rate offset. ADCS measures it for 5 s after every boot and keeps it only if the board was still (`ADCS_GYRO_CAL_*`); `ADCS_CMD_SET_GYRO_OFFSET` (0x26) overrides it; telemetry page 3 (`ADCS_TLM3_*`, `ADCS_GCAL_*`) reports it. Page 0 gyro now has the offset removed. Only constants added: a v0.4 slave works with a v0.5 master (minor-version warning; page 3 and 0x26 then answer BAD_PARAM / UNKNOWN_CMD).
- **v0.6** (2026-11-14): sun sensors. ADCS turns its BPW34 readings (5 faces, no nadir diode) into a body-frame Sun direction and a sunlit/eclipse flag, telemetry page 4 (`ADCS_TLM4_*`, `ADCS_SUNF_*`, `ADCS_FACE_*`); `ADCS_CMD_SET_SUN_CONFIG` (0x27) sets the lit threshold and full scale (`ADCS_SUN_*`). The EPS eclipse decision uses the ADCS flag. Only constants added: with a v0.5 slave, page 4 / 0x27 answer BAD_PARAM / UNKNOWN_CMD and the OBC falls back to the raw page-1 values.
- **v0.7** (2026-11-14): a sun diode on every face (a bare face reads as eclipse whenever it faces the Sun). Gyro tracking of the Sun: dead-diode detection from missing light (`ADCS_FLT_SUN_DIODE`, `ADCS_SUN_DEFICIT_PERMILLE`, `ADCS_SUN_DEAD_*`) and `ADCS_SUNF_HIDDEN` / `ADCS_SUNF_PREDICTED` (unknown instead of a false eclipse). `ADCS_CMD_SET_SUN_DISABLED` (0x28); page-4 byte 9 is now `ADCS_TLM4_OFS_DEAD_FACES` (was reserved 0).
