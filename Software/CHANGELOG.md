# Changelog

## 2026-11-15 (6): TT&C firmware moved to `Software/TTC/`

- `firmware/ttc/` → `TTC/` (git history kept), next to `ADCS/` and `OBC/`, with its own README. `firmware/build.py` builds it from there; target names are unchanged (`ttc`, `ttc_notx`). Older CHANGELOG entries still say `firmware/ttc/`.

## 2026-11-15 (5): TT&C code review, fw 0.4.2

**Scope:** every TT&C-specific file, read in full:
- `firmware/ttc/`: `ttc_app.c/h`, `ttc_nv.c/h`, `hmac_sha256.c/h`, `sx1278.c`, `hw_stm32.c`, `ttc_hw.h`, `main.c`;
- the simulator `firmware/host/ttc_hw_sim.c`;
- the OBC's TT&C client (`TtcClient`) and the forwarding allowlist.

The shared I2C/ICD layer (`icd_link.c`, `icd_node.c`, `i2c_slave.c`) was **not** re-reviewed here.

**Fixed:**
- **The deploy switch is debounced while burning.** It must read deployed for 50 ms (`TTC_DEPLOY_DEBOUNCE_MS`). One noisy reading from the burn current could end every automatic burn early and waste all 3 lifetime attempts.
- **`BURN_FIRE` is refused while any burn runs.** An OBC fire during an automatic burn restarted the deadline: up to 13 s of continuous burn, and the auto-burn report was lost.
- **The watchdog now starts before `ttc_init`.** The boot-time flag-store compaction (sector erase, CPU stalled up to 2 s) and the radio init could otherwise hang the board forever.

**Tried and reverted:** making the "packet coming in" hold restart only on RX/TX. A stale hold could then expire just as a real packet arrived, and the TT&C would transmit over it. The original rule is kept: any quiet moment lets a packet out, and a steady signal is held at most 1 s. A new test covers constant interference.

**Checked, no change needed:**
- **Emergency commands:** constant-time MAC compare; counter stored before acting; retransmission cache; key length 0 = disabled.
- **Packets and queues:** queue bounds and slot sizes; QUEUE_TX_PART offsets, overflow and lost parts; the OBC can't forward DEPLOY_RESET or QUEUE_TX_PART from the ground.
- **Flash store:** torn-record handling and compaction order.
- **Radio timing:** airtime formula vs the driver's low-data-rate rule; TX timeouts for 255-byte packets; the SX1278 uses the LF port (RSSI −164) and the FIFO pointers are correct.
- **Watchdog:** escalation and overflow bounds; event-log indexing.

**Accepted (noted, not changed):**
- A second emergency pulse on a line that is already asserted extends it, and only the newer command gets an EVENT.
- The event number is u16, so it wraps after 65535 events.
- If the radio is dead, a pending E_TTC_RESET waits; nothing could have delivered it anyway.

**Tests:** firmware 1616 checks (5-face 1615). Each fix has a regression test, and each of those tests fails without its fix (checked). OBC + ICD 201 passed. STM32 check 47 files; ARM all clean. The receive-only build: 72 transmit-dependent checks fail, by design.

## 2026-11-15 (4): key handling

- **`OBC/scripts/keygen.py`** makes both keys at once in `~/.cubesat/`:
  - `ground_key` for OBC commands; it goes to the Pi.
  - `ttc_key` for TT&C emergency commands; it is built into the firmware and never goes to the Pi.

  The files are mode 600 in a 700 folder. The script won't overwrite without `--force` and refuses to write inside the (public) repo.
- **`firmware/build.py`** finds `~/.cubesat/ttc_key` by itself. It refuses the public host-test key, and a TT&C key equal to `ground_key`.
- **`ground.CounterStore`:** a ground-side counter per key. It is Unix time or more, and saved (fsync + rename) before use, so a counter is never reused, even across a crash or a clock set back.
- **`ground.load_key`** warns if other users can read the key file.
- **`.gitignore`:** now also covers `ttc_key`, the generated `ttc_key.c`, counter files and `.cubesat/`. The public host-test key file stays tracked.
- Tests: OBC + ICD 201 passed.

## 2026-11-15 (3): emergency priority and execution reports, TT&C fw 0.4.1

- **Priority:** ECMD ACKs and the new EVENT packets have their own 4-deep queue, sent before everything else. This replaces the one-slot buffer, which lost an ACK when two ECMDs came in close together. TT&C now listens for at least 150 ms between non-priority packets and never transmits over an incoming packet (capped at 1 s), so an emergency command gets in during a file burst. Before this, the satellite was deaf for the whole burst.
- **Execution reports:** new `GL_T_EVENT` 0x06 (18 B), reporting the end of every deferred emergency action and every autonomous TT&C action:
  - OBC/ADCS line released, then OBC back or OBC not back.
  - Burn result and ms burned; radio defaults or revert, sent on the new settings.
  - TT&C reset (reported after the reboot, via flash); silence end; deployed flag; boot.

  ACKs of deferred commands carry `GL_EA_REPORT_FOLLOWS`. New `GL_E_GET_EVENTS` 0x06 returns the last 3 events. Simulator: optional half-duplex radio model.
- **Flags vs full-chip erase:**
  - `swd_flash.py erase` keeps sector 7; the full wipe is now `wipe-flags`, which asks for confirmation.
  - The emergency counter has a floor, the build time (`ttc_ecmd_ctr_floor`, written by build.py; host tests use 100).
  - A blank or wiped flag store is reported (EVENT NV_BLANK 0x86, new flag key TTC_NV_INIT).
  - After a wipe, a deployed antenna is re-detected without burning.
- Tests: firmware 1608 checks (5-face 1607), OBC + ICD 198 passed. Earlier in this entry, before the erase fix: firmware 1597 checks (5-face 1596; a run with the listen gap set to 1 ms fails the priority check, as it should), OBC + ICD 198 passed, STM32 check 47 files, ARM all clean.

## 2026-11-15 (2): ICD v0.9, GL_VERSION 4, TT&C fw 0.4.0

- **Frequency:** defaults 433.300 MHz down / 433.800 MHz up, 10 dBm, inside India's licence-free 433.05–434.04 MHz segment (v0.8's 433.000 MHz spilled below 433.05). `TTC_FREQ_MIN_HZ` = 433.05 MHz.
- **Automatic antenna deployment** (TT&C boot procedure): RBF out for 60 s → 5 s burn, retry 60 s later, at most 3 burns in the satellite's life. DEPLOY_SW reading deployed for 2 s sets a flash flag; later boots never burn on their own. `TTC_CMD_DEPLOY_RESET` 0x49 clears it (bench only, RBF in). TLM0 deploy flags DEPLOYED_FLAG / AUTO_ACTIVE, TLM1 auto_attempts.
- **Flash flag store** (`firmware/ttc/ttc_nv.c`): append-only journal in STM32 sector 7; linker script stops code at 384 KB.
- **Emergency commands** (GL_T_ECMD 0x05): TT&C checks HMAC + flash-kept counter with its own key (`TTC_KEY_FILE`) and executes without the OBC: ping, TT&C reset, radio defaults, beacon period, radio silence, burn; Pi RUN reset, Pi power cycle (GLOBAL_EN), OBC watchdog (off by default); ADCS NRST reset. ACK target has 0x80 set. New `firmware/ttc/hmac_sha256.c`.
- **SET_RADIO confirm/revert:** unconfirmed settings revert after 10 min to the last good ones (fault RADIO_REVERTED 0x0080).
- **Queues:** RX and TX queues have their own depth and slot size (the TX queue used the RX depth constant). TX slots are 255 bytes.
- **Long FILE packets:** `TTC_CMD_QUEUE_TX_PART` 0x48; FILE blocks up to 248 bytes, default `file_block = 200` (~3× throughput); FILE_START answer +block_len (13 B); FILE_START variant 1 sends the JPEG's EXIF thumbnail (rpicam `--thumb 160:120:60`). TX timeout now scales with airtime.
- Tests: firmware 1541 checks (5-face 1540), OBC + ICD 197 passed, STM32 check 47 files, ARM all targets clean.

## 2026-11-15: ICD v0.8, ground link v3, TT&C fw 0.3.0

- **Split frequency:** TT&C receives on the uplink (default 433.500 MHz) and retunes to the downlink (433.000 MHz) for each transmission. SET_RADIO/GET_RADIO grow from 9 to 13 bytes (freq_up u32 at offset 9); freq_up must be above freq_down. `TTC_FREQ_DEFAULT_HZ` is now `TTC_FREQ_DOWN_DEFAULT_HZ` / `TTC_FREQ_UP_DEFAULT_HZ`.
- **FEC:** default LoRa coding rate 4/8 (was 4/5). A 24-byte packet takes 280 ms instead of 206 ms. The explicit header carries the CR, so receivers follow automatically.
- **ACKs in SAFE:** TT&C now accepts ACK and TLM packets in SAFE, so commands sent in SAFE are acknowledged (was ICD open issue 1). Other downlink is still refused in SAFE.
- **File downlink, Selective Repeat ARQ:** new FILE packet (0x04) and OBC commands FILE_START/FILE_ACK/FILE_ABORT (0x20–0x22). Sender in the OBC, receiver for the ground station: `OBC/comms/arq.py`. obc.toml: `file_window`, `file_timeout_s`.
- **No bit stuffing,** by decision: LoRa frames are length-delimited, so stuffing would add bytes and no protection. Framing and protection are documented in ICD.md section 11.
- Tests: firmware host tests for split frequency, the new radio validation and SAFE ACKs; `OBC/tests/test_arq.py` (lossy-channel runs up to 50 % loss); end-to-end image download through the simulated TT&C.

## 2026-11-14

### Layout
- **OBC split by subsystem:** `OBC/EPS/EPS.py`, `OBC/Payload/Payload.py`, `OBC/comms/` (`bus.py`, `slaves.py`, `ground.py`). The commands changed to `python3 OBC/EPS/EPS.py` and `python3 OBC/Payload/Payload.py`; `python3 -m OBC` is unchanged.
- **Firmware ported** from the base repo into `firmware/`: build system, shared STM32 code, TT&C, Nucleo dummy slave, PC simulation and its tests. Everything builds against `ICD/` (the base repo's `firmware/common/` was identical to `ICD/slave/` and is not copied).
- **ADCS has its own folder,** `ADCS/`, like `OBC/` and `ICD/`.
- **`docs/` rewritten** for this repo from the base repo's docs: `RPI_SETUP.md`, `WIRING.md`, `BRINGUP.md`, `NO_PROGRAMMER.md`. Personal paths and the Pi's IP address were left out, and the SWO baud rate is corrected (115200, not 2 MHz).
- **New:** `README.md` (overview, all build and test commands) and `ADCS/README.md` (how ADCS works).
- The 22 OBC simulation tests that used to be skipped now run (`python3 firmware/build.py sim`).

The ADCS and TT&C firmware came from the base repo (`~/Downloads/code.zip`); nobody on the team wrote it. It was reviewed line by line, and the fixes below came out of that review.

### Shared STM32 code (both boards)
- **Watchdog:** started before it is configured (ST's order). The old order waited on IWDG flags that only clear once the watchdog clock runs, so it could hang at boot.
- **I2C slave:** a second end-of-transfer after a read (STOP after the master's NACK) could re-queue the previous command and run it twice. Fixed in two places: `stm32/i2c_slave.c` tracks the transfer direction, and `ICD/slave/icd_link.c` clears the receive buffer when a read starts.
- **I2C master:** the per-wait timeout went from 5 to 2 ms, so failed sensor reads cannot hold the main loop past the OBC's 20 ms reply window.
- TT&C fw 0.2.1 (shared fixes only; TT&C's own code is not reviewed yet).

### ADCS
- **fw 0.3.0, ICD v0.5: gyro zero-rate offset.** MPU-6050 modules read a few °/s at rest, which could keep the rate above the 2 °/s detumbled limit forever.
  - ADCS measures the offset for 5 s after every boot and keeps it only if the board was still (gyro and raw magnetometer steady).
  - `SET_GYRO_OFFSET` (0x26) overrides it, and telemetry page 3 reports it.
  - The OBC saves the bench value (`adcs_gyro_offset.json`) and re-sends it when ADCS boots tumbling in orbit.
- **fw 0.3.0, recovery:** automatic recovery from fault-SAFE waits only for the magnetometer and coil drivers (a dead gyro or sun ADC no longer blocks it). A fault that returns right after recovery doubles the wait, from 30 s up to 32 min. A commanded mode change switches the coils off at once.
- **fw 0.4.0, ICD v0.6: sun sensors.** ADCS turns its BPW34 readings into a Sun direction (body frame) and a sunlit/eclipse flag, on telemetry page 4. `SET_SUN_CONFIG` (0x27) sets full scale and the lit (albedo) threshold. The EPS eclipse decision uses this flag (older firmware: raw values).
- **fw 0.5.0, ICD v0.7: a sun diode on every face.** With no pointing control a bare face reads as eclipse whenever it faces the Sun (about 5 % of directions).
  - Of each opposite pair only the brighter face is used, so albedo on the far face is ignored.
  - Gyro tracking of the Sun backs up a diode that fails in orbit. Missing light identifies the dead diode (fault `SUN_DIODE`), and the Sun behind it is reported HIDDEN (unknown, not eclipse).
  - `SET_SUN_DISABLED` (0x28) lets the ground switch faces off.
  - A 5-face build remains (`-DSUN_NO_MZ`, target `adcs_5face`).
- The sun sensors (BPW34, attitude) and the payload's TSL2591 (solar flux, science) are separate systems.

### Tests (all on PC)

| Test | Before | Now |
|---|---|---|
| Firmware checks | 91 | 1354 (1353 in the 5-face build) |
| STM32 compile check | 34 files | 43 files |
| OBC tests | 102 (22 skipped) | 114 (none skipped with the simulation built) |
| ICD tests | 47 | 52 |
| Link test | — | 15/15 against both simulated boards |

Each fix was also checked by putting the bug back and confirming a test fails.

### Not done yet
- **ARM builds:** not run (no ARM compiler on the development PC).
- **Untested driver code:** the drivers that only run on the chip (watchdog, I2C slave) are compile-checked only. Check them on the first flash.
- **Bench values** (`docs/BRINGUP.md`):
  - sun-sensor face mapping, full scale and lit threshold;
  - gyro offset;
  - axis map and coil polarity;
  - magnetometer hard-iron offset.
- **TT&C review:** still to do.
- **Ground tool:** `gs_tool.py` and the LilyGO bridge are still in the base repo.
