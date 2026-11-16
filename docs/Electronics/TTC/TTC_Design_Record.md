# TT&C Design Record — Evidence-Based Extraction

Sources read: the TTC firmware archive (`TTC.zip`), the TTC KiCad archive (`TTC_1.zip`), and the public repository PARTH-KAKIRDE/CUBESAT (read-only clone), plus a few verification calculations.

# PART 1 — REQUIREMENTS, SOURCES, CORRECTIONS

## 0. Ground rules I applied

- Evidence only. Nothing is inferred from "KiCad files exist" or "code exists".
- Status tags: DISCUSSED, PROPOSED, SELECTED, DESIGNED, IMPLEMENTED, TESTED, REVISED, REJECTED, PENDING, UNVERIFIED.
- Source tags: FW (firmware), SCH (KiCad schematic), PCB (KiCad layout), GIT (commit or branch), ICD (ICD.md, cubesat_icd.h, ground_link.h), CHLOG (Software/CHANGELOG.md), PRIOR (earlier project discussion), MYCALC (my own calculation, kept separate from project history).
- "Not documented" means I found no source for it.
- I checked the repository directly; I did not rely on the READMEs alone.

## 0.1 Corrections to your framing

1. **Form factor is 2U, not 3U.**
   - Your 3U / 6 kg brief was dropped. The TTC README, the schematic title block ("2U CubeSat TT&C"), the firmware docs and PRIOR all say 2U (10×10×20 cm, 84 mm class PCBs).
   - The only place 3U survives is your current prompt and the 3U-era mechanical notes.
2. **"~2 kg target mass" is not in any file I can see.** The brief said 6 kg max, and other notes mention MTQ sizing at "2U / 4 kg". Treat 2 kg as UNVERIFIED.
3. **"~600 km polar orbit" is not in any TTC file.** It appears only in your prompt. I used it as an assumption in the MYCALC sections.
4. **The OBC identity conflicts across sources.**
   - PRIOR and the schematic say RPi CM4 ("RPi CM4+").
   - The firmware, README and ICD wire the emergency lines to a Pi 4B RUN and GLOBAL_EN.
   - You need to settle which one it is.
5. **Confirmed:** STM32F446RET6, RFM98W/SX1278, dedicated TTC PCB, repo PARTH-KAKIRDE/CUBESAT. Branch Final_PCB does contain TTC/TTC. Branch rayon/ttc-v2 contains a different TTC layout lineage.

## 0.2 What I could not access

- RFM98W, SX1278, TMP102 and TPS7A2033 datasheets.
- TTC_Component_Sheet_LoRa.md. The schematic title block references it, but it is not in the uploads or the repo.
- cubesat_project_master_record.md.
- Verbatim text of earlier chats. I only have the recorded outcomes, so the original link-budget numbers are not recoverable.
- Any lab measurement, scope capture, RF test log or antenna file.
- KiCad itself. I parsed the files as text. I did not run ERC or DRC, and I did not see a 3D view.
- spi1.c and log.c (SPI mode and speed are unverifiable), plus the I2C frame format in ICD §1–5, which I did not extract.

## A. Source inventory (what each source is)

**S1. TTC.zip**
- 11 firmware files: fw 0.4.2, ICD v0.9.
- Byte-identical (11/11) to Software/TTC on main at d00c4b8 (2026-11-15).

**S2. TTC_1.zip**
- KiCad project: .kicad_pcb, .kicad_sch, .kicad_pro, .kicad_dru, temp-freerouting.dsn, PDF.
- Byte-identical to Final_PCB:TTC/TTC at commit 504b046 (2026-10-23, "Add final PCB designs").
- Its README.md differs from every repo README.

**S3. Repo branches** (public)
- main d00c4b8 (2026-11-15)
- Final_PCB b284b07 (2026-11-13)
- rayon/ttc-v2 86b40a9 (2026-10-17)
- final-pcb-update 784d80a
- software 78109b4
- adcs 0492f34

**S4. ICD set on main:** ICD.md v0.9 (473 lines), cubesat_icd.h, ground_link.h (GL_VERSION 4), CHANGELOG.md, docs/BRINGUP.md, docs/WIRING.md.

**S5. software branch**
- ground/gs_tool.py (307 lines) and ground/lilygo_bridge/lilygo_bridge.ino. I read only their headers.
- Also firmware/host/ttc_hw_sim.c, which I did not read.

**S6. PRIOR project decisions** (RF chain, PA/LNA dropped, filter plan, ground station hardware).

**Provenance warning (CHLOG, 2026-11-15):** the ADCS and TT&C firmware came from a "base repo" (~/Downloads/code.zip). The changelog says "nobody on the team wrote it". It was reviewed line by line, with fixes in fw 0.4.2. Be careful how you describe authorship in your report.

---

# PART 2 — EXTRACTION, CATEGORY BY CATEGORY

## 2. TTC system architecture

**Purpose [IMPLEMENTED in code, sim-tested only; FW/ICD]**
- TT&C owns the radio.
- It sends every beacon.
- It filters uplink packets by satellite ID, length and CRC.
- It queues good packets for the OBC.
- It executes authenticated emergency commands (ECMD) by itself when the OBC (Pi) is hung.
- It deploys the antenna by burn wire once at boot (v0.9).

**Chain, ground to satellite**
- Ground PC (gs_tool.py) ↔ USB serial ↔ LilyGO LoRa32 (433 MHz SX1278, lilygo_bridge.ino) ↔ LoRa RF ↔ RFM98W on the TTC board ↔ SPI1 ↔ STM32F446 (TTC).
- STM32F446 ↔ I2C3 slave at address 0x33 ↔ RPi OBC (I2C master).
- The OBC talks to ADCS (I2C slave, 0x32).
- The OBC talks directly to the EPS INA226 (0x40) and the payload sensors. The EPS has no MCU.

**Who does what [ICD §11]**
- TT&C (STM32): radio scheduling, beacon timing, uplink filtering.
- OBC (Pi): builds every ACK, TLM, FILE and OBC-beacon packet. It checks the HMAC and counter of each command, then executes or forwards it.
- Exception: ECMD (type 0x05) is checked and executed by TT&C with its own key.

**Flows**
- Command flow: ground → LoRa → TT&C filter → rx queue (8 deep) → OBC polls GET_RX → MAC/counter check → execute or forward → ACK built by OBC → QUEUE_TX → TT&C tx queue (8 deep) → LoRa.
- Telemetry flow: slaves → OBC (1 s status poll, slower pages every 10 s) → OBC beacon (22 B) → SET_BEACON → TT&C transmits it. If the OBC is silent for more than about 3 beacon periods, TT&C sends its own 20 B beacon instead.
- Emergency flow: ECMD → TT&C executes → ACK in a 4-deep priority queue → EVENT packet when the action ends.

**TT&C to other subsystems**
- No data link to ADCS or EPS.
- Planned reset lines: PC6 → Pi RUN, PC8 → Pi GLOBAL_EN, PC7 → ADCS NRST, each through a 2N7002. These are DESIGNED in firmware only. The final PCB has no J8 and no MOSFETs (see §11, §18).
- EPS has no TT&C line. ICD says power-cycling the Pi is the EPS-level recovery.

**Power:** from EPS 3.3 V (J1) or 5 V (J7), JST-XH. EPS bus topology details are outside this extraction.

**Modes [ICD §8]** (BOOT=0, SAFE=1, DETUMBLE=2, NOMINAL=3)
- BOOT: load flash flags, init radio. Goes to NOMINAL, or SAFE if the radio failed.
- SAFE: RX on, beacon at least every 60 s, OBC burn commands refused. QUEUE_TX accepts only ACK and TLM. Returns automatically when the radio recovers.
- DETUMBLE: treated as NOMINAL.
- NOMINAL: RX, beacons, OBC downlink, burn wire.
- A commanded SAFE (low battery) also holds automatic deployment and E_BURN. ECMD works in every mode.

## 3. Transceiver and RF hardware

### 3.1 RFM98W / SX1278 (as actually used)

| Item | Value | Source / status |
|---|---|---|
| Part | RF_Module:RFM98W-433S2, footprint HOPERF_RFM9XW_SMD (16 pads) | SCH/PCB, DESIGNED |
| Rationale | Module over a discrete RF chain: EM-first, pre-tuned RF. Integrated PA used. No external PA, LNA or RF switch. | PRIOR, SCH comment "pre-tuned RF" |
| Modulation | LoRa only. The driver sets the LoRa bit; FSK/OOK never used | FW, IMPLEMENTED |
| Frequency | Default 433.300 MHz down, 433.800 MHz up. Accepted range 433.05 to 438 MHz | FW/ICD, SELECTED |
| Spreading factor | SF9 (allowed 7 to 12) | FW |
| Bandwidth | 125 kHz (62.5/125/250/500 selectable) | FW |
| Coding rate | 4/8 (allowed 4/5 to 4/8) | FW |
| Power | 10 dBm via PA_BOOST (allowed 2 to 17) | FW |
| Sync word | 0x12 | FW |
| Preamble | 8 symbols, hard-coded | FW |
| Header | Explicit | FW |
| Payload CRC | On | FW |
| Interface | SPI1 | FW |

**Registers set in sx1278.c**
- LNA = 0x23 (max gain, boost).
- PA_DAC = 0x84 (default, no +20 dBm mode).
- OCP = 0x2B (on, 100 mA trim per the code comment).
- PA_CONFIG = 0x80 | (P−2), so 10 dBm gives 0x88. This follows Pout = 2 + OutputPower.
- ModemConfig1 = (BW code << 4) | ((CR−4) << 1).
- ModemConfig2 = (SF << 4) | 0x04.
- ModemConfig3 = 0x04 (AGC on), plus 0x08 (low-data-rate optimisation) when the symbol time exceeds 16 ms. At SF9/125 kHz the symbol time is 4.096 ms, so optimisation is off.
- Version register must read 0x12, or init fails.
- Opmode: LORA | SLEEP at init, STDBY, TX (0x03), RX_CONT (0x05).

**Frequency programming**
- Code formula: Frf = f × 2^19 / 32 MHz.
- MYCALC, 433.3 MHz: Frf = 7,099,187 = 0x6C5333.
- MYCALC, 433.8 MHz: Frf = 7,107,379 = 0x6C7333.
- Step is 61.035 Hz.

**Split frequency [IMPLEMENTED]**
- One half-duplex chip. It receives on the uplink frequency and retunes in standby to the downlink frequency before each TX, then back to RX.
- The retune is three register writes.

**Pins**
- SPI1: PA5 SCK, PA6 MISO, PA7 MOSI, PA4 NSS (pin 20), PC4 RESET.
- DIO0 to DIO5: PB0, PB1, PB12 to PB15. They are wired but not used. The driver polls RegIrqFlags over SPI instead.

**Receive and RSSI**
- RSSI = −164 + PktRssi (low-frequency port).
- If SNR < 0: RSSI += SNR/4.
- SNR is stored raw in units of 0.25 dB.

**RFM98W module vs SX1278 IC (general engineering, not project history)**
- The SX1278 is Semtech's transceiver IC. The RFM98W is HopeRF's module built around it. It adds the reference crystal, matching and filtering, the antenna path, and 16 castellated pads.
- The firmware comment says the module brings out only PA_BOOST. That is why PA_BOOST is used.
- The exact "-433S2" variant's electrical limits are Not documented in the sources.

**Not used in your implementation**
- FSK/OOK, sleep mode (the radio is in continuous RX), CAD, frequency hopping, implicit header, +20 dBm, DIO interrupts.

**Not documented:** supply current by mode, output impedance, SPI clock and mode, frequency stability (ICD assumes ±20 ppm).

### 3.2 RF front end (from the schematic and layout)

Fitted path:
```
U2 pad 9 (ANT) -> [C6 DNP shunt] -> R2 0R -> node [C7 DNP shunt] -> R18 0R (fitted) -> J3 SMA edge-mount (Samtec SMA-J-P-H-ST-EM1)
```

Alternative path (R19 0R DNP, fit R18 OR R19 but not both):
```
node -> L1 18 nH -> J9 pin1 (C23 7.5 pF to GND)
node -> C24 7.5 pF -> J9 pin2 (L2 18 nH to GND)
```
This is an LC lattice balun feeding a dipole (J9 = "DIPOLE FEED" wire pads). The schematic note reads "50 ohm to 50 ohm BAL @ 436 MHz".

**Findings (all DESIGNED only)**
- No bandpass or harmonic filter is fitted. The Pi-match pads have R2 = 0 Ω and C6/C7 DNP.
- PRIOR said a self-fabricated LC bandpass filter was planned with values deferred. It is not in the schematic. Status: PROPOSED, not DESIGNED.
- No RF-line ESD or TVS.
- Module supply: +3V3 → FB1 (BLM18PG121 ferrite) → module pin 13, with C4 10 µF and C5 100 nF.
- Balun frequency mismatch: the balun is annotated for 436 MHz, but the firmware defaults are 433.3/433.8 MHz.
  - MYCALC ideal values for a 50 Ω lattice balun, L = R/ω and C = 1/(ωR): at 436 MHz, 18.25 nH and 7.30 pF; at 433.3 MHz, 18.37 nH and 7.35 pF.
  - The fitted 18 nH and 7.5 pF are the nearest standard values, so the mismatch is small. Verify the real centre frequency (UNVERIFIED).

**Layout facts (parsed from PCB)**
- RF nets Net-(U2-ANT), Net-(C7-Pad1) and Net-(J3-In) use 0.5 mm tracks on F.Cu. The J3 net also has a short B.Cu segment and 2 vias.
- Track length is about 28.7 mm (8.9 + 9.0 + 10.8), measured track-only.
- MYCALC characteristic impedance (Hammerstad) uses the KiCad default stackup in the file: 1.51 mm core, εr 4.5. That gives about 109 Ω at 0.5 mm width, so this is not a 50 Ω line.
- About 2.9 mm width would be needed for 50 Ω on that stackup. The run is roughly 0.07 wavelengths long (λg ≈ 395 mm), so the effect is modest but unquantified. The real fab stackup is unknown.

## 4. Frequency and communication architecture

**Architecture [SELECTED/IMPLEMENTED in FW]**
- One transceiver. Half-duplex. Split frequency. Uplink 500 kHz above downlink.

**Frequency history**
- Single default frequency. The bridge comment says 436.5 MHz, a "placeholder: use your coordinated amateur frequency". REVISED.
- v0.8 (2026-11-15): 433.000 down / 433.500 up. REVISED.
- v0.9 (2026-11-15): 433.300 down / 433.800 up. The v0.8 down channel (433.000 MHz) reached 432.94 MHz, below the band. CHLOG.
- The rayon README says 435 to 438 MHz. That is stale text.

**Regulatory claims in ICD (these are the project's claims; I did not verify them)**
- India licence-free band 433.05 to 434.79 MHz, 10 mW ERP, 10 % duty cycle.
- A 125 kHz channel is allowed only within 433.05 to 434.04 MHz.
- The ICD says both channels sit at least 180 kHz inside that segment.
- The ICD open-issues list still has "licence" and "antenna gain vs 10 mW ERP" items unresolved.
- Caution: this is a technical frequency choice. The authorisation to operate a transmitter on a satellite is a separate regulatory question. The bridge comment (amateur frequency) and the ICD (licence-free ISM) assume different regulatory routes.

**Doppler**
- ICD uses ±11 kHz.
- MYCALC at 600 km: maximum radial velocity 6.91 km/s gives ±10.0 kHz at 433.3 MHz. ICD's 11 kHz matches using the full orbital speed (7.56 km/s) as a bound.
- No Doppler compensation exists in firmware (Not documented).

## 5. Antenna design and RF integration

- Antenna selected: conflicting.
  - PRIOR: a COTS turnstile (EnduroSat/ISIS) via RG-316 and an SMA edge connector.
  - PRIOR (later): antenna fixed, no deployment mechanism.
  - SCH: SMA output (default) or LC balun to dipole pads.
  - FW (v0.9): burn-wire antenna deployment with a deploy switch and an RBF pin. Implies a deployable antenna.
- The burn-wire power stage is not on this board. BURN_EN (PC2) is a logic output to an external MOSFET, which is Not documented elsewhere.
- Not documented: antenna dimensions, resonance, bandwidth, gain, polarization, pattern, VSWR, return loss, simulations, design files, interference tests, placement.
- General note (label: engineering reference): a quarter-wave at 433.3 MHz is about 173 mm in free space, so a stowed dipole does not fit a 200 mm 2U envelope without deployment.
- ICD open issue: antenna gain vs the 10 mW ERP limit is unchecked.
- Status: antenna design not completed. Transceiver compatibility is only a 50 Ω SMA interface, and the track impedance is the concern above.

## 6. Communication protocols and data architecture

**Protocol:** custom ground link, GL_VERSION 4 (FW/ICD). Not AX.25, CCSDS or KISS. The sources never mention considering them. The v0.8 "no bit stuffing" decision explicitly rejects HDLC/KISS-style framing because LoRa frames are length-delimited.

**Interfaces**
- SPI1: RFM98W.
- I2C3 (PA8 SCL, PC9 SDA): OBC link, slave 0x33.
- I2C1 (PB6 SCL, PB7 SDA): TMP102 at 0x48 (master).
- USART1 (PA9 TX, PA10 RX): log at 115200 8N1 via J6.
- I2C total pull-up about 1.1 kΩ at 100 kHz (WIRING). The ICD frame format was not extracted.

**Packet** (all multi-byte fields little-endian)
```
SAT_ID 0xC5 | TYPE | SEQ | body | CRC16 (CCITT-FALSE, low byte first, ICD check value 0x29B1)
```
- Maximum 24 bytes. FILE packets may be up to 255.

**Types**
- CMD 0x01, ACK 0x02, TLM 0x03, FILE 0x04, ECMD 0x05, EVENT 0x06.
- BEACON_OBC 0x10, BEACON_TTC 0x11.

**CMD / ECMD layout**
```
SAT_ID | TYPE | SEQ | TARGET | CMD | LEN | ARGS(0-8) | COUNTER u32 | MAC(4) | CRC16
```
- MAC is the first 4 bytes of HMAC-SHA256(key, all bytes from SAT_ID to COUNTER).
- Targets: OBC 0, ADCS 1, TTC 2, EPS 3. 0x80 is set in the ACK target for emergency answers.

**ACK layout**
```
SAT | 0x02 | SEQ | TARGET | CMD | STATUS | LEN | DATA (up to 15)
```
- Longer answers follow as TLM parts (16 B each).

**Reliability**
- Replay protection: the counter must exceed the last accepted one. TT&C keeps its counter in flash and writes it before acting.
- Retransmission (same packet): the same ACK is re-sent and the command is not re-run.
- Bad CRC, MAC or counter packets are dropped silently.
- No uplink fragmentation.
- Downlink file transfer is Selective Repeat ARQ: window 16 by default (maximum 32), block 200 B by default (1 to 248), whole-file CRC-32, up to 65,535 blocks. The OBC sends long blocks to TT&C through QUEUE_TX_PART.
- Sequence: SEQ is 1 byte. EVENT numbers are u16 since boot.

**Statuses**
- ACK and the core packet formats: IMPLEMENTED (sim-tested).
- KISS, AX.25, CCSDS: Not documented as considered.

## 7. Telemetry architecture

**TT&C telemetry page 0 (24 B)**
- uptime_ms u32, radio_state, rx_queue, tx_queue, deploy_flags.
- last_rssi i16, last_snr i8 (0.25 dB), mode_reason.
- rx_count, tx_count, rx_crc_err (u16), board_temp i16 (0.01 °C), beacon_period u16, obc_i2c_resets u16.

**Page 1 (12 B)**
- tx_timeouts, rx_dropped, tx_dropped, beacon_count (u16), burn_count u8, auto_attempts u8, last_burn_ms u16.

**TT&C beacon (20 B)**
- uptime_s u32, mode, status_flags, faults u16, rx_count u16, temp i16, last_rssi i16, obc_contact u8, CRC.

**OBC beacon (22 B)**
- uptime, obc_mode, vbatt_mV, ibus_mA, adcs_mode, ttc_mode, adcs_rate (0.01 dps), comm_flags, cpu_temp, lux.

**Housekeeping 16 B, EPS GET 8 B, EPS GET_STATE 24 B** (battery V, load I, battery I, solar power, SOC, power state, flags, sun flags, min/max, energy used).

**Fault bits:** RADIO_INIT, RADIO_TX_TIMEOUT, TEMP_SENSOR, RX_OVERFLOW, TX_OVERFLOW, OBC_I2C, LOCAL_I2C, RADIO_REVERTED, NV.

**Origins**
- Temperature: TMP102 on the TTC board.
- Radio and deploy: TTC.
- Battery and load: INA226 via the OBC.
- Lux: TSL2591 via the OBC.
- Sun and ADCS: ADCS slave via the OBC.

**Beacon timing**
- Default period 30 s (minimum 10 s, SAFE at least 60 s). First beacon about 5 s after boot.
- No telemetry store in TT&C. The OBC keeps logs. Log compression and size cap are pending.
- Store-and-forward at TTC: Not documented. Bulk image downlink exists via FILE.

## 8. Telecommand and uplink architecture

**OBC forward allowlist** (ICD §11)
- ADCS: PING, GET_VERSION, GET_TELEMETRY, SET_MODE, SET_BDOT_GAIN, SET_DETUMBLE_THRESH, SET_BDOT_TIMING, GET_CONFIG, MTQ_MANUAL, SET_MAG_OFFSET, SET_GYRO_OFFSET, SET_SUN_CONFIG, SET_SUN_DISABLED.
- TT&C: PING, GET_VERSION, GET_TELEMETRY, SET_MODE, GET_RADIO, SET_BEACON_PERIOD, BURN_ARM, BURN_FIRE.
- Not forwarded: SET_RADIO (a wrong value would cut the uplink with no way back), GET_RX, QUEUE_TX, SET_BEACON. Anything else gets REFUSED (0x82).

**Emergency commands (executed by TT&C with its own key)**
- E_PING, E_TTC_RESET (key "BOOT"), E_RADIO_DEFAULTS, E_BEACON_PERIOD, E_TX_SILENCE (up to 1,440 min), E_GET_EVENTS, E_BURN (up to 8 s, RBF out, not in commanded SAFE).
- E_OBC_RESET (RUN low 200 ms), E_OBC_POWER_CYCLE (GLOBAL_EN low 1 to 60 s), E_OBC_WATCHDOG (5 to 1,440 min, kept in flash, off by default; at most 3 actions per outage: first a reset, then power cycles).
- E_ADCS_RESET (NRST low 20 ms).

**Execution reports (EVENT, 18 B):** OBC_BACK 0x80, OBC_NOT_BACK 0x81 (after 300 s), DEPLOYED 0x82, RADIO_REVERTED 0x83, SILENCE_END 0x84, BOOT 0x85, NV_BLANK 0x86. The last three events can be re-read with E_GET_EVENTS.

**Priority and fail-safes**
- ECMD is checked in the radio task the moment it arrives.
- A 4-deep priority queue sends ECMD ACKs and EVENTs ahead of everything. If it is full, the oldest entry is dropped.
- TT&C listens at least 150 ms between non-priority packets. It never transmits over an incoming packet, with a hold of at most 1 s.
- The ground should repeat an ECMD about every 100 ms until the ACK returns.
- SET_RADIO is confirm-or-revert: unconfirmed settings revert after 10 minutes. A reverted setting raises a fault bit and an EVENT.
- Burn is stopped by deadline, the deploy switch (debounced 50 ms), RBF reinsertion, or a mode change.

**Status:** all of the above are IMPLEMENTED in firmware and tested only in the PC simulation. Whether gs_tool.py supports ECMD is UNVERIFIED. Its usage text lists no emergency commands.

## 9. Link budget and range

**Project record**
- PRIOR says a link budget confirmed enough internal margin to drop the external PA and LNA, at both bench-test distances and LEO.
- No calculation survives in any source I can see. Not documented.
- ICD gives only airtime figures (§10).

**Separate section: MYCALC reference budget, labelled UNVERIFIED**
- Assumptions: circular 600 km orbit, Earth radius 6371 km, f = 433.3 MHz, no Earth rotation, ideal pass.

Equations:
- Slant range: d = sqrt((Re+h)² − Re² cos²ε) − Re sin ε.
- FSPL (dB) = 32.44 + 20 log10(f MHz) + 20 log10(d km).
- Pr = Pt + Gt + Gr − FSPL − Lmisc.
- Sensitivity ≈ −174 + 10 log10(BW) + NF + SNRmin, with NF = 6 dB assumed.
- Dimensional check: d in km with the constant 32.44. In metres the constant would be −27.55.

| Elevation | Slant range | FSPL |
|---|---|---|
| 90° | 600 km | 140.7 dB |
| 30° | 1075 km | 145.8 dB |
| 10° | 1932 km | 150.9 dB |
| 0° | 2829 km | 154.2 dB |

Sensitivity:
- SF9, 125 kHz: −174 + 51 + 6 − 12.5 ≈ −129.5 dBm.
- SF12, 125 kHz: −174 + 51 + 6 − 20 ≈ −137 dBm.
- Noise floor is −123 dBm before NF, about −117 dBm with NF.

Received power with 0 dBi at both ends and no other losses (Pt = 10 dBm, default):
- SF9, 90°: −130.7 dBm, margin −1.7 dB.
- SF9, 10°: −140.9 dBm, margin −11.9 dB.
- SF9, 0°: −144.2 dBm, margin −15.2 dB.
- SF12, 90°: margin +6.3 dB.
- SF12, 10°: margin −3.9 dB.
- At 17 dBm: add 7 dB.

**Reading of the numbers**
- Under the firmware defaults (10 dBm, SF9) and unity-gain antennas, the budget does not close at low elevation.
- It would need ground or satellite antenna gain, a lower spreading factor margin trade (SF11 or SF12), or both.
- Polarization loss (up to 3 dB for linear-to-circular), cable and connector loss, and pointing are not yet included.
- The 10 mW ERP rule also caps the ground transmitter.
- So the earlier "sufficient margin" conclusion cannot be reproduced from the defaults. Treat it as UNVERIFIED and recompute with your real antenna gains.
- Eb/N0 is not applicable to LoRa chirp spread spectrum.
- Raw bit rate at SF9/125 kHz/CR 4/8 is about 1,099 bit/s.

**Orbit and visibility (MYCALC)**
- Period 96.5 min, orbital speed 7.56 km/s.
- Horizon-to-horizon overhead pass about 12.8 min. Above 10° elevation about 8.5 min.
- These are upper bounds. Most passes are shorter.

**Throughput (ICD)**
- SF9/125 kHz/CR 4/8, 200 B blocks, window 16: about 100 B/s.
- 17 B blocks: about 32 B/s.
- A 50 kB image takes about 8 min. The EXIF thumbnail (3 to 5 kB) takes under a minute.
- The 10 % duty-cycle limit caps the long-run average at a tenth of those rates.

## 10. LoRa configuration, airtime and trade-offs

**Actual values** are in the §3.1 table. Not documented: packet error rate, reliability.

**Airtime** is computed with the firmware's own formula (ttc_airtime_ms). I checked it against ICD. MYCALC, SF9/125 kHz/CR 4/8:
- 20 B: 247 ms (matches ICD's TT&C beacon figure).
- 24 B: 280 ms (matches ICD).
- 50 B: 476 ms.
- 100 B: 837 ms.
- 200 B: 1,558 ms.
- 255 B: 1,951 ms.
- CR 4/5 at 24 B: 206 ms (matches ICD).
- SF12, 24 B: 1,974 ms.
- TX timeout is max(3 s, 2 × airtime + 500 ms).

**Trade-offs (general engineering, labelled)**
- Higher SF: better sensitivity, but airtime rises about 2× per step and the data rate falls.
- Wider bandwidth: faster, but sensitivity drops about 3 dB per doubling.
- Stronger coding rate: more FEC, more overhead. CR 4/8 doubles payload time versus 4/5 in the worst case.
- Higher TX power: more range but more current and a higher ERP. The licence-free band caps this at 10 dBm.
- The firmware allows up to 17 dBm via SET_RADIO. The OBC does not forward SET_RADIO from the ground.
- Note: OCP trim of 100 mA may limit output at 17 dBm. Check the datasheet (UNVERIFIED).

## 11. TTC electronics and PCB design

**Two layout lineages exist. Do not mix them up.**
- rayon/ttc-v2 head (86b40a9): includes J8 footprint, R20, R21, D9, D10; 1,011 track segments, 255 vias.
- Final_PCB TTC (= your TTC_1.zip): 679 segments, 81 vias (15 on GND). It lacks J8, R20, R21, D9, D10.
- The final schematic is newer than the final PCB (see parity below).

**Board**
- 85 × 84 mm (edge rectangle 92.5,93 to 177.5,177), 2-layer, 1.6 mm FR4 (KiCad default stackup: 35 µm Cu, 1.51 mm core, εr 4.5, tanδ 0.02), 4 × M4 holes, 3 fiducials.
- Net classes: Default 0.2 mm track/clearance, via 0.6/0.3 mm; PWR 0.5 mm.
- Routed widths: 0.15 mm (25 segments), 0.2 mm (621), 0.5 mm (34).
- Board text: "TTC PCB 10/2026 VJTI, Parth Kakirde, Pushkar Dube, Rayon Biswas". Schematic title: "TT&C Board Rev A - LoRa (STM32F446 + RFM98W)", dated 2026-09-26.

**Pin map (from PCB nets)**

| MCU pin | Function |
|---|---|
| PA4/5/6/7 | RFM NSS, SCK, MISO, MOSI |
| PC4 | RFM RESET |
| PB0, PB1, PB12 to PB15 | RFM DIO0 to DIO5 |
| PA8 / PC9 | OBC I2C SCL / SDA (J5) |
| PA9 / PA10 | UART TX / RX (J6) |
| PB6 / PB7 | I2C1 SCL / SDA (TMP102) |
| PC0 / PC1 / PC2 | DEPLOY_SW, RBF_IN, BURN_EN (J2) |
| PC11 | LED D7 via 1 kΩ |
| PC6 / PC7 / PC8 | planned Pi RUN / ADCS NRST / Pi GLOBAL_EN (pin pads only) |
| PA13 / PA14 / PB3 | SWDIO / SWCLK / SWO (J4 Tag-Connect TC2030) |

**Circuits and status**
- Power input (DESIGNED):
  - 3.3 V path: J1 → F1 0.5 A PTC → Q1 AO3401A (reverse-polarity P-FET, gate R1 10 kΩ) → C1 22 µF → R14 0 Ω → +3V3. R14 is fitted, so this is the default path.
  - 5 V path: J7 → F2 0.5 A PTC → U1 TPS7A2033PDBVR (3.3 V LDO) → R15 0 Ω (DNP).
  - Fit R14 OR R15 only.
  - D1 (SMBJ3.3A) and D8 (SMBJ5.0A) are DNP in the schematic. The rayon commit message says their clamp voltages do not protect the rails.
- MCU (DESIGNED):
  - Y1 8 MHz crystal (HSE), Y2 32.768 kHz crystal (LSE, 7 pF).
  - VCAP_1 C16 4.7 µF, NRST C15 100 nF, BOOT0 pulled down by R3 10 kΩ.
  - Decoupling C8 to C14 and C21.
  - This is different from the ADCS board, which has no HSE.
- Debug: J4 Tag-Connect TC2030 plus the USART1 log. This REVISES the earlier decision that no SWD header was needed.
- Temperature: TMP102 (U4), 4.7 kΩ I2C pull-ups R16/R17, ALERT unconnected.
- I/O protection: ESD D2 to D6 on UART and mission lines (optional), 1 kΩ series resistors R4 to R8.
- Spare header J8 (2×13, DNP in schematic; no footprint on the final PCB).

**Schematic vs final PCB parity (PCB lacks five schematic parts)**
- Missing on the PCB: J8, R20 (100 kΩ BURN_EN pull-down), R21 (10 kΩ NSS pull-up), D9/D10 (OBC I2C ESD).
- Effects: no BURN_EN pull-down on the board (risk of a floating gate on the external burn-wire switch at reset), no NSS pull-up, no OBC-line ESD, and no way to fit the emergency-line stage.
- PCB value fields for C17 to C20 read "CL TBD", while the schematic has 8.2 pF (HSE) and 6.8 pF (LSE). The values were changed after the rayon commit (12 pF and 8.2 pF there).
- D1/D8 show no DNP in the PCB value text.
- README says "schematic parity clean". My check contradicts that for the board you uploaded (UNVERIFIED because I did not run KiCad).

**README vs file findings:** the README claims 0.15/0.3/0.6 mm tracks, about 160 stitching vias, and an antenna path of 36 mm with no vias. None of that matches the Final_PCB file (widths 0.15/0.2/0.5 mm, 81 vias, about 28.7 mm path with 2 vias on the J3 net). That text appears to describe the rayon lineage.

**Revision history (GIT)**
- 2026-10-06 cc42fbc (Pushkar Dube): schematic and component placement.
- 2026-10-14 (Rayon Biswas): 23c6f7e first fully routed layout; d20aa38 crystal re-route; 16187b9 teardrops and "RF via fence"; f9c4dad, deac0fd, b9867ab auto-placed layouts; 598a49e spread auto layout becomes the single board; 7719c52 resize to 85×84 and move mounting holes.
- 2026-10-17 86b40a9: pre-fab fixes — R20, R21, crystal caps, D1/D8 DNP, D9/D10, J6 silkscreen. ERC 0, DRC 0.
- 2026-10-20: KiCad backup zips.
- 2026-10-23 504b046 (Parth): "Add final PCB designs" (Final_PCB branch).
- 2026-11-13 b284b07: software added.
- 2026-11-15 d00c4b8: TT&C fw 0.4.2 on main.

**Panel and fabrication:** PANEL_jlc and PANEL_pcbpower contain ADCS | EPS | TTC panels with Gerbers. Those are order files, not proof that boards were ordered or received (see §18).

## 12. Embedded firmware

**Provenance and versions:** fw 0.2.1 → 0.3.0 → 0.4.0 → 0.4.1 → 0.4.2. See §0.2 and CHLOG.

| File | Contents | Status |
|---|---|---|
| main.c | Boot: watchdog (4 s), ttc_init, I2C slave start, main loop with log and kick | IMPLEMENTED |
| ttc_app.c (1010 lines) | Queues, beacons, burn, deploy, ECMD, SET_RADIO confirm and revert, ICD commands, telemetry, airtime | IMPLEMENTED, sim-tested |
| ttc_app.h | Constants, state struct, deploy polarity macros | IMPLEMENTED |
| sx1278.c (194 lines) | Register-level LoRa driver, polling, split frequency | IMPLEMENTED, no hardware test evidence |
| hw_stm32.c | TMP102, GPIO, flash sector driver, emergency lines | IMPLEMENTED |
| ttc_nv.c/h | Append-only flash journal in sector 7 (flags, counters) | IMPLEMENTED, sim-tested |
| hmac_sha256.c/h | SHA-256 and HMAC-SHA256, tested against FIPS 180-4 and RFC 4231 vectors | IMPLEMENTED, TESTED (PC vectors) |
| ttc_hw.h | Hardware abstraction (real board and simulation) | IMPLEMENTED |

**Main loop (`ttc_step`):** ICD link → beacon timer → radio task → burn task → deploy task → emergency task → housekeeping, then I2C stuck-bus check, log, watchdog kick. Nothing blocks. Radio is polled.

**Notable code details**
- `radio_task`: poll, RX parse, TX_DONE handling, TX timeout restarts the radio, 1 s receive hold, priority queue first, 150 ms listen gap, beacon choice (OBC beacon if fresh, else TT&C beacon).
- `ecmd_rx`: length check, HMAC compare of `GL_AUTH_MAC_LEN` bytes, counter greater than flash value and greater than a build-time floor, retransmission cache, ACK with the emergency bit.
- Flash store: 8-byte records (CRC-16 protected). Compaction when about 75 % full; the sector erase stalls the CPU for 1 to 2 s, so the watchdog is started first.
- Emergency lines: initialised low with pull-downs. A high level asserts reset or power-off on the target.
- Automatic deployment: RBF out for 60 s → 5 s burn → retry after 60 s, at most 3 burns in the satellite's life. The deployed flag is set when the switch reads deployed for 2 s.

**Build:** `build.py arm ttc` (or `ttc_notx`, a bench build that never transmits). The linker script stops code at 384 KB so flash sector 7 is protected. The emergency key is generated into a file outside the repo. Pi can flash by SWD bit-bang (swd_flash.py).

**Not documented:** SPI mode and clock, log module internals, Pi-side TtcClient details.

## 13. Ground station architecture

- Hardware (PRIOR and bridge source): a LilyGO LoRa32 (433 MHz SX1278, T3 v1.6.1 pin map) running lilygo_bridge.ino (RadioLib). It replaces TinyGS firmware. Whip antenna.
- Serial protocol: `TX <hex>`, `CFG <freq_hz> <sf> <bw_khz> <cr> <power_dbm> <sync_hex>`, `RX <rssi> <snr> <hex>`, `TXDONE`.
- gs_tool.py (PC): listen, cmd obc/adcs/eps/ttc, keygen, `--sim` (whole satellite simulated on the PC). It retransmits the same SEQ until an ACK arrives. The key file and counter file are under ground/.
- File downlink receiver: OBC/comms/arq.py.
- **Stale settings:** the bridge header still states 436.5 MHz, CR 4/5. The ICD v0.9 defaults are 433.3/433.8 MHz and CR 4/8. The CFG command can override, but the defaults need updating. UNVERIFIED which is actually flashed.
- **Conflict:** the TTC_1.zip README says the prototype ground station is a Raspberry Pi with its own RFM98W. Other sources say LilyGO.
- Antenna tracking, pass prediction, orbital prediction software: Not documented.
- Pending (from your task list): ground-station decoding of EPS GET_STATE, and updating gs_tool and the bridge.
- Status: code exists (IMPLEMENTED, simulation only). No RF contact documented. Telemetry visualization: Not documented.

## 14. Power, mass, thermal budgets

- Not documented: TX, RX, standby or sleep current; MCU current; regulator losses; average and peak power; mass of PCB, module, antenna, cables; thermal dissipation.
- Known limits: OCP trim 100 mA (code comment); F1/F2 are 0.5 A PTCs.
- Derived duty (MYCALC): a 247 ms beacon every 30 s is about 0.82 %. In SAFE (60 s) it is about 0.41 %.
- The radio sits in continuous RX when not transmitting. Sleep modes are not used.
- The EPS power-budget report (by a teammate) is unverified.

## 15. Orbital operations

- Visibility, Doppler and duty cycle: see §4 and §9 (MYCALC, assumptions only).
- Attitude dependence: not documented. ADCS is detumble-only, so antenna pattern nulls and polarization loss at random orientation need analysis.
- Radio failure handling (IMPLEMENTED): radio retry every 10 s, TX timeout restarts the radio, SAFE on radio failure, SET_RADIO revert after 10 min, lost-master detection after 30 s (TT&C keeps beaconing).
- OBC / EPS / ADCS relationships: OBC polls both slaves every 1 s. A low battery commands SAFE and holds burns. TT&C can reset the OBC and ADCS by hardware lines (hardware missing on final PCB).
- Store-and-forward on TT&C: not implemented.

## 16. Testing and validation

| Test | Class | Result / evidence |
|---|---|---|
| SHA-256/HMAC vectors; flash journal; automatic deploy; every ECMD, replay and forgery; radio revert; QUEUE_TX_PART; airtime formula; split frequency; SAFE ACKs; constant-interference hold | SIMULATED (PC host tests) | Passing per ICD §12 and CHLOG |
| 15 ICD link tests (bad frames, unplug, 10,000 PINGs) | SIMULATED | 15/15 passing on the firmware simulation |
| Same link tests on the Pi against real boards or a Nucleo | NOT TESTED | "To run" (no ST-LINK or Nucleo yet) |
| SPI/register read-write, RF TX/RX, range, RSSI/SNR, packet loss, antenna, OBC integration, ground-station integration | NOT TESTED | Not documented |
| TTC bring-up procedure (BRINGUP.md §3) | PLANNED | A procedure only, with no recorded results |

- I invented no test results. Nothing in the sources is RF-tested or hardware-integrated.

## 17. Design evolution (summary)

- Original brief: 3U, 6 kg, Pi + STM32 master/slave. Final: 2U.
- Controller: STM32F446 retained. STM32H7 doubted; PyCubed rejected (ready-made hardware, does not validate your own design).
- RF: RFM98W module over a discrete chain. External PA, LNA and RF switch REJECTED after the link budget (numbers unrecoverable, see §9).
- Filter: LC bandpass PROPOSED, then absent from the schematic.
- Debug: no SWD header, then J4 TC2030 added (REVISED).
- Antenna: COTS turnstile, then fixed, then SMA/dipole pads, then burn-wire deployment logic (v0.9).
- Frequency: 436.5 placeholder → 433.0/433.5 → 433.3/433.8.
- FEC: CR 4/5 → 4/8 (v0.8).
- SAFE downlink: ACKs blocked in SAFE → ACK and TLM allowed (v0.8).
- Protocol: unauthenticated → authenticated CMD (v2) → file ARQ (v3) → ECMD, EVENT, long FILE (v4). Block size 17 B → 200 B.
- Layout: auto-placed lineage → single 85×84 board → final lineage with schematic newer than the PCB.
- Board plan: 90×96 mm (3U-era) → 85×84 mm.

## 18. Current status

**A. Finalized decisions:** STM32F446RET6, RFM98W LoRa, 433.3/433.8 MHz split, SF9/125 kHz/CR 4/8/10 dBm, I2C3 link at 0x33, custom ground link v4, authenticated commands, ECMD, burn-wire auto-deploy logic.
**B. Components:** see §11 (BOM-level list in the schematic, 78 parts).
**C. Schematic:** complete (Rev A, newer than the PCB).
**D. PCB:** routed Final_PCB lineage, but out of parity with the schematic (§11).
**E. Manufactured hardware:** UNVERIFIED. Conflict: your earlier notes said "in manufacturing / not yet arrived", while the ICD (Nov 14 to 15) and README say "before ordering" and "not yet ordered". Panel Gerbers exist.
**F. Firmware:** implemented in code, sim-tested only.
**G. Verified RF communication:** none.
**H. Ground station:** code exists, no RF contact documented.
**I. Planned, unimplemented:** emergency-line hardware (J8 + 3× 2N7002), LC filter, Doppler compensation, store-and-forward, antenna design, ground station updates.
**J. Rejected:** external PA/LNA, RF switch, bit stuffing, PyCubed.
**K. Outstanding problems**
1. Link budget does not close under defaults (MYCALC).
2. 50 Ω RF track mismatch (about 109 Ω, MYCALC, stackup UNVERIFIED).
3. Balun centre 436 MHz vs 433 MHz operation.
4. Final PCB lacks J8, R20, R21, D9, D10; crystal caps unspecified on the PCB.
5. Deploy and RBF pin polarity unverified (a wrong polarity either sets the flag at once or burns three times).
6. Antenna type unresolved; burn-wire power stage off-board.
7. Pi 4B vs CM4.
8. Regulatory route (licence-free vs amateur) unresolved.
9. ground tool default settings stale.
10. 49.7-day uptime wrap.
**L. Missing:** datasheets, component sheet, hardware measurements, antenna files, power and mass budgets, link-budget numbers from earlier discussion.

## 19. Contradictions list

1. 3U vs 2U.
2. CM4 vs Pi 4B.
3. SWD "excluded" vs J4 present.
4. Antenna: turnstile / fixed / dipole / deployable.
5. Frequency: 436.5 vs 435 to 438 vs 433.3/433.8.
6. Balun 436 MHz vs 433.3 MHz.
7. Ground station: LilyGO + TinyGS vs LilyGO bridge vs Pi with RFM98W.
8. README (0.6 mm RF tracks, about 160 vias) vs file (0.5 mm, 81 vias).
9. Crystal caps: 12/8.2 pF (rayon) vs 8.2/6.8 pF (schematic) vs "CL TBD" (PCB).
10. PCB order status.
11. Doppler ±11 kHz (ICD) vs ±10 kHz (MYCALC).

## 20. What is needed to close the gaps

1. The original link-budget numbers (or antenna gains for a recompute).
2. The antenna decision.
3. Confirmation of the Pi model.
4. Whether the boards were ordered, and from which file.
5. Datasheets for the exact RFM98W variant.

Possible follow-ups (Part 3): the full ICD I2C frame definitions, the complete net-by-net connectivity, the ground tool and OBC TtcClient details, or the ADCS/EPS interfaces.
