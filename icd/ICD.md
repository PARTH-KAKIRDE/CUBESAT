# ICD: OBC ↔ ADCS / TT&C I2C link — v0.2

The single source of truth is [`cubesat_icd.h`](cubesat_icd.h). This document explains it, and the header wins if the two disagree. Changes go through Parth and bump `ICD_VERSION_*`.

**Scope:** Raspberry Pi 4B (OBC, master) ↔ STM32F446 ADCS and TT&C (slaves).

**Out of scope:**
- The ADCS sensor bus (STM32 I2C1 → MPU-6050 and MMC5983MA).
- The TT&C sensor bus (TMP102).
- EPS and payload, which the Pi drives directly: INA226 at 0x40, TSL2591 at 0x29, a USB camera, and the PYRO GPIO.

## 1. Bus and addressing

| Item | Value |
|---|---|
| Master | Pi only. Slaves never speak first, so the Pi polls. |
| Speed | 100 kHz |
| ADCS | **0x32**. STM32 I2C3: SCL PA8, SDA PC9. Header J11 (1 SDA, 2 SCL, 3 GND). 4.7k pull-ups on the board. |
| TT&C | **0x33**. STM32 I2C3: SCL PA8, SDA PC9. Header J5 (1 SDA, 2 SCL, 3 GND). 4.7k pull-ups on the board. |
| Pi link bus | `/dev/i2c-1`: GPIO2 SDA (pin 3), GPIO3 SCL (pin 5) |
| Pi sensor bus | `/dev/i2c-3` (`dtoverlay=i2c3,pins_4_5`): GPIO4 SDA (pin 7), GPIO5 SCL (pin 29). Carries the INA226 at 0x40 (EPS J6 pins 3 and 4) and the TSL2591 at 0x29. |

**Clock stretching:** the STM32 slaves stretch SCL briefly at the start of each read while the interrupt loads the first byte. The Pi's hardware I2C controller handles stretching badly; if the link test shows errors, use the bit-banged bus 11 (`docs/RPI_SETUP.md`).

**Address check:** the only I2C chip on the EPS is the INA226 module (0x40 by default; the other options are 0x41–0x4F). The CN3722 MPPT charger, the BMS and the buck converters have no I2C. The TSL2591 is fixed at 0x29. 0x32 and 0x33 clash with nothing. They are also on a separate bus from the sensors, so a misbehaving sensor cannot hang the STM32 link.

## 2. Transaction

1. The Pi writes a command frame, then a STOP.
2. The Pi waits at least 5 ms.
3. The Pi reads **exactly 32 bytes**.

If the slave hasn't finished processing, it answers `BUSY` and echoes the CMD_ID. The Pi re-reads up to 3 times, 5 ms apart.

The slave keeps its response until the next command, so a repeated read returns the same frame:
- **CRC error on the read:** the Pi re-reads. It never re-sends, so no command runs twice.
- **Slave answered `BAD_CRC` or `BAD_LENGTH`:** the slave did not act on the frame, so the Pi re-sends once.

Before the first command, a read returns an idle frame (CMD 0x00, `NO_DATA`).

On the slave side, the interrupt only copies bytes. The main loop validates and processes the frame, then stages the response. A frame with a bad CRC or a bad length is discarded and never acted on.

## 3. Frames (little-endian)

```
Command  (Pi → slave): CMD_ID(1) | LEN(1) | PAYLOAD(0..24) | CRC16(2)               4..28 bytes
Response (slave → Pi): CMD_ID(1) | STATUS(1) | LEN(1) | PAYLOAD(0..24) | CRC16(2) | 0x00 pad to 32
```

- **CRC:** CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR). It covers every byte before it and is sent low byte first.
- **Check value:** `"123456789"` → **0x29B1**. Both sides test this: `firmware/host/test_firmware.c` and `obc/tests/test_frames.py`.
- **BAD_LENGTH:** a command whose byte count ≠ LEN + 4, or whose LEN > 24, gets `BAD_LENGTH`.

## 4. Status codes

| Code | Name | Code | Name |
|---|---|---|---|
| 0x00 | OK | 0x05 | INVALID_MODE |
| 0x01 | BAD_CRC | 0x06 | HW_FAULT |
| 0x02 | UNKNOWN_CMD | 0x07 | NO_DATA |
| 0x03 | BAD_LENGTH | 0x08 | **BAD_PARAM** (new in v0.2: value out of range) |
| 0x04 | BUSY | 0x09 | **QUEUE_FULL** (new in v0.2: TT&C TX queue full) |

## 5. Common commands

| ID | Name | Request | Response |
|---|---|---|---|
| 0x01 | PING | – | – |
| 0x02 | GET_VERSION | – | fw major, minor, patch; ICD major, minor (5 bytes) |
| 0x03 | GET_STATUS | – | mode u8, status_flags u8, fault_flags u16, uptime_s u32 (8 bytes) |
| 0x04 | GET_TELEMETRY | page u8 | page data (unknown page → BAD_PARAM) |
| 0x05 | SET_MODE | mode u8 | – |

**status_flags:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | FAULT_ACTIVE | A fault is present right now. |
| 1 | GROUND_PKT_PENDING | TT&C only: a ground packet is waiting. |
| 2 | NO_MASTER_CONTACT | Set after 30 s with no valid transaction. It stays set until reported in a GET_STATUS, so the Pi learns about the gap. |
| 3 | RESET_OCCURRED | Set at every boot or watchdog reset. Cleared by the next GET_STATUS. On seeing it, the Pi re-sends the slave's configuration. |

**fault_flags** are sticky: any fault seen, even briefly, stays set until a GET_STATUS has reported it and the condition is gone.

**Lost GET_STATUS replies.** The slave clears RESET_OCCURRED, NO_MASTER_CONTACT and gone-away sticky faults as soon as it has *answered* a GET_STATUS, not when the Pi has received the answer. So the Pi guards against a lost reply in two ways:
- **Recovery read:** if a command was written but none of its reads succeeded, the Pi reads once more before writing its next command. The slave still holds that reply (section 2), so the flags are recovered and merged into the next status. The same recovery catches a lost GET_RX reply, so a ground packet popped from the queue is not lost.
- **Uptime check:** if `uptime_s` goes backwards, the Pi treats it as a reset. (`uptime_s` wraps after 49.7 days, which causes one harmless re-configuration.)

| Bit | ADCS | TT&C |
|---|---|---|
| 0 | MAG (MMC5983MA) | RADIO_INIT (SX1278) |
| 1 | IMU (MPU-6050) | RADIO_TX_TIMEOUT |
| 2 | DRVX nFAULT | TEMP_SENSOR (TMP102) |
| 3 | DRVY nFAULT | RX_OVERFLOW |
| 4 | DRVZ nFAULT | TX_OVERFLOW |
| 5 | LOCAL_I2C | OBC_I2C peripheral reset |
| 6 | OBC_I2C peripheral reset | LOCAL_I2C |
| 7 | SUN_ADC | – |

## 6. Subsystem commands

**ADCS (0x20–0x3F)**

| ID | Name | Request | Notes |
|---|---|---|---|
| 0x20 | SET_BDOT_GAIN | gain u32 | duty‰ = −(gain/1000) · dB/dt [µT/s]. Max 10,000,000. Default 300,000. |
| 0x21 | SET_DETUMBLE_THRESH | exit u16, enter u16, dwell_s u16 | Rates in 0.01 °/s; enter must be > exit. Defaults 2.00, 5.00 and 60 s. |
| 0x22 | SET_BDOT_TIMING | period_ms u16, settle_ms u16 | Period 100–10000 ms. The coils are off for settle_ms before each magnetometer sample. |
| 0x23 | GET_CONFIG | – | 20 bytes: gain, exit, enter, dwell, period, settle, mag offset x, y, z |
| 0x24 | MTQ_MANUAL | duty x, y, z i16 (‰), duration_ms u16 | NOMINAL only, ≤ 10 s. 0 ms stops it. |
| 0x25 | SET_MAG_OFFSET | x, y, z i16 (magnetometer LSB) | Hard-iron offset, subtracted from every sample. |

**TT&C (0x40–0x5F)**

| ID | Name | Request / response | Notes |
|---|---|---|---|
| 0x40 | GET_RX | resp: one ground packet ≤ 24 bytes | `NO_DATA` if the queue is empty. Queue depth 8. |
| 0x41 | QUEUE_TX | req: 1–24 bytes | `QUEUE_FULL` if full. `INVALID_MODE` in SAFE. |
| 0x42 | SET_BEACON | req: 0–24 bytes | The OBC beacon. TT&C sends it while it is fresh and the OBC is alive, and its own beacon otherwise. |
| 0x43 | SET_BEACON_PERIOD | period_s u16 | 0 = off; otherwise ≥ 10 s. SAFE forces at least 60 s. |
| 0x44 / 0x45 | SET_RADIO / GET_RADIO | freq u32, sf u8, bw u8, cr u8, power i8, sync u8 | 433–438 MHz, SF7–12, bw code 0–3 (62.5/125/250/500 kHz), CR 5–8, 2–17 dBm. Applied when the radio is idle. |
| 0x46 | BURN_ARM | key u32 = 0x4E525542 ("BURN") | Arms for 10 s. |
| 0x47 | BURN_FIRE | duration_ms u16 (≤ 8000) | Needs ARM and the RBF plug removed. Stops at the deadline or as soon as DEPLOY_SW reports deployed. |

**Ground command path:** TT&C receives a packet (it only queues ours: SAT_ID 0xC5 and a valid CRC). The Pi reads it with GET_RX, checks it (section 11), and forwards allowed ICD commands to ADCS or TT&C. TT&C never talks to ADCS. The packet format is in [`ground_link.h`](ground_link.h) and section 11.

## 7. Telemetry (scaled integers, serialised field by field)

**Scales:**
- Magnetometer: **0.025 µT/LSB** (±819 µT; the MMC5983MA has 16384 counts/G).
- Gyro: **65.5 LSB per °/s** (MPU-6050 at ±500 °/s), raw counts.

**ADCS page 0, sensors (18 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0 | uptime_ms | u32 |
| 4, 6, 8 | mag X, Y, Z | i16 |
| 10, 12, 14 | gyro X, Y, Z | i16 |
| 16 | mag_age_ms | u16; 0xFFFF = never valid |

ADCS switches the coils off around each magnetometer sample, so `mag_age_ms` is the age of the last clean sample. The Pi logs `mag_stale = true` when the age exceeds period + 500 ms and never presents an old sample as fresh.

**ADCS page 1, sun sensors and coils (18 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0–10 | SS1..SS6 | u16 raw 12-bit ADC |
| 12, 14, 16 | coil duty X, Y, Z | i16 ‰ |

> **Change from the draft:** each DRV8833 AISEN goes to ground through a 1 Ω sense resistor (R1_0805, R1_806, R1_807), which sets the driver's own current limit (about 200 mV / 1 Ω = 200 mA per bridge). The sense node is not connected to the STM32, so coil current cannot be measured. Page 1 reports the commanded duty instead.

**ADCS page 2, controller (16 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0, 2, 4 | dB/dt X, Y, Z | i16, 0.01 µT/s |
| 6 | rate_norm | u16, 0.01 °/s |
| 8 | imu_temp | i16, 0.01 °C |
| 10 | ctrl_flags | u8: DETUMBLED, COILS_ACTIVE, MANUAL, MAG_VALID |
| 11 | mode_reason | u8: boot / command / fault / auto |
| 12 | obc_i2c_resets | u16 |
| 14 | local_i2c_errors | u16 |

**TT&C page 0, radio (24 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0 | uptime_ms | u32 |
| 4 | radio_state | u8 |
| 5 | rx_queue | u8 |
| 6 | tx_queue | u8 |
| 7 | deploy_flags | u8 |
| 8 | last_rssi | i16, dBm |
| 10 | last_snr | i8, 0.25 dB |
| 11 | mode_reason | u8 |
| 12 | rx_count | u16 |
| 14 | tx_count | u16 |
| 16 | rx_crc_err | u16 |
| 18 | board_temp | i16, 0.01 °C |
| 20 | beacon_period | u16, s |
| 22 | obc_i2c_resets | u16 |

**TT&C page 1, counters (12 bytes):** tx_timeouts, rx_dropped, tx_dropped, beacon_count (u16 each), burn_count u8, reserved u8, last_burn_ms u16.

## 8. Modes

BOOT = 0, SAFE = 1, DETUMBLE = 2, NOMINAL = 3.

- The Pi can never request BOOT.
- From BOOT, only SAFE may be requested.
- SAFE, DETUMBLE and NOMINAL can be requested from each other. Anything else gets `INVALID_MODE`.
- A slave may drop itself to SAFE on a fault.

| Mode | ADCS | TT&C |
|---|---|---|
| BOOT | Self-test. Enters **DETUMBLE by itself**, or SAFE if the magnetometer is missing. | Initialises the radio. Goes to NOMINAL, or SAFE if the radio failed. |
| SAFE | DRV8833 asleep (nSLEEP low). Sensors still sampled. If it entered SAFE on a fault, the fault has been gone for 30 s and the Pi is silent, it returns to DETUMBLE by itself. | RX on. Beacon at least every 60 s. QUEUE_TX and BURN refused. Returns to NOMINAL by itself when the radio recovers. |
| DETUMBLE | B-dot loop every period: coils off → settle → magnetometer sample → dB/dt → coils on. SAFE if the magnetometer fails 3 times or a driver fault lasts 3 cycles. Refused with `HW_FAULT` if the magnetometer is down. | Same as NOMINAL. |
| NOMINAL | Coils off (pointing law TBD). MTQ_MANUAL allowed. The Pi promotes ADCS here once it reports DETUMBLED. | RX, beacons, Pi downlink, burn wire. |

## 9. Timing and failures

- **Polling:** the Pi polls GET_STATUS from both slaves and GET_TELEMETRY (pages 0 and 2) from ADCS at 1 Hz. The slower pages are read every 10 s.
- **COMM_FAULT:** three consecutive failed transactions mark a subsystem COMM_FAULT. It is logged once, polling continues, and it clears on the next success.
- **Lost master:** after 30 s with no valid transaction, a slave sets NO_MASTER_CONTACT and carries on. ADCS keeps detumbling; TT&C keeps beaconing with its own beacon.
- **Bus errors:** on any bus error, the slave resets its I2C peripheral and re-arms. It does the same if the bus is busy with no progress for 25 ms.
- **No alert wire:** faults and pending packets reach the Pi only through the status flags, so the worst-case latency is one poll (1 s).

## 10. Debugging

- **ADCS:** SWD + SWO on J16 (Tag-Connect). Logs go out over SWO/ITM, because ADCS has no free UART.
- **TT&C:** **has** SWD on J4 (Tag-Connect TC2030); the draft said it didn't. It also logs on USART1 at J6 (pin 2 = TX out, pin 1 = RX in, pin 3 = GND), 115200 8N1.

## 11. Ground link (`ground_link.h`, GL_VERSION 2)

Every packet: `SAT_ID(1) | TYPE(1) | SEQ(1) | body | CRC16(2)`, at most 24 bytes. The CRC is the same CRC-16 as on I2C, low byte first. Long answers go down as an ACK plus TLM parts; nothing is chunked on the uplink.

**Command packet (ground → satellite):**

```
SAT_ID | TYPE=0x01 | SEQ | TARGET | CMD | LEN | ARGS (0..8) | COUNTER u32 | MAC (4) | CRC16
```

- **MAC** = first 4 bytes of HMAC-SHA256(key, every byte from SAT_ID to COUNTER).
- **COUNTER** must be higher than the last one the OBC accepted. The OBC saves it before executing (`<data_dir>/ground_counter`), so a recorded command cannot be replayed, even after an OBC restart. The ground tool uses `max(last + 1, unix time)`, so losing its counter file does not lock it out, as long as the PC clock is right.
- **Retransmission** (same packet, because an ACK was lost) is answered from the OBC's ACK cache and not executed again.
- **Rejected silently (no ACK):** bad CRC, bad MAC, an old unauthenticated (v1) packet, or an old counter. The OBC logs each one.
- **Key:** 32 random bytes as hex text, made with `python ground/gs_tool.py keygen`. The same file goes on the Pi (`obc.toml [ground] key_file`, `chmod 600`). It is never committed. **Without a key file the OBC rejects every ground command** (beacons still go out).
- ARGS is limited to 8 bytes (24 − 6 header − 4 counter − 4 MAC − 2 CRC). The largest allowed command, MTQ_MANUAL, is exactly 8.
- Downlink packets (ACK, TLM, beacons) are not authenticated; their CRC only protects integrity.

**What the OBC forwards to the slaves.** A forwarded ICD command can have side effects the OBC depends on, so only these are passed on; anything else gets `GL_ST_REFUSED (0x82)`:

| Target | Forwarded | Not forwarded, and why |
|---|---|---|
| ADCS | PING, GET_VERSION, GET_TELEMETRY, SET_MODE, SET_BDOT_GAIN, SET_DETUMBLE_THRESH, SET_BDOT_TIMING, GET_CONFIG, MTQ_MANUAL, SET_MAG_OFFSET | — |
| TT&C | PING, GET_VERSION, GET_TELEMETRY, SET_MODE, GET_RADIO, SET_BEACON_PERIOD, BURN_ARM, BURN_FIRE | GET_RX / QUEUE_TX / SET_BEACON: the OBC owns the queues and beacon. SET_RADIO: a wrong value cuts the uplink with no way back; change it in `obc.toml`. |
| both | GET_STATUS is **answered from the OBC's last poll**, because a real one would clear the reset and fault flags before the OBC saw them. | |

- BURN_ARM and BURN_FIRE are refused while the OBC is in SAFE or the battery is low, the same rule as the pyro.
- ADCS settings and the beacon period changed from the ground are kept in the OBC's memory, so a slave reset re-applies them instead of the `obc.toml` values. An OBC restart goes back to `obc.toml`.

**EPS reading (GL_EPS_GET response, 8 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0 | vbatt | u16, mV |
| 2 | ibus | i16, mA |
| 4 | pbus | u16, mW |
| 6 | low_battery | u8, 1 = low |
| 7 | pyro_fires | u8, count since OBC start (saturates at 255) |

**Known limitation:** in SAFE, TT&C refuses QUEUE_TX, so the OBC's ACKs and downlink cannot go out; only the beacons do. Commands sent in SAFE are still executed (for example `obc mode nominal`), but the ground sees no ACK. Changing this needs a TT&C firmware change.

**Bit-order note:** the OBC_I2C and LOCAL_I2C fault bits are in opposite positions on ADCS (6 and 5) and TT&C (5 and 6). Always use the names from the header, never the numbers.

