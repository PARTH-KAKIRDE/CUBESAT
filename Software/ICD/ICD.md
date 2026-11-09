# Inter-Subsystem ICD v0.3: OBC ↔ ADCS / TT&C I2C link, and the ground link

**Status:** v0.3, 2026-11-09, owner Parth. Merges the v0.1 draft into v0.2 (what came from where: `README.md`).
The single source of truth is [`cubesat_icd.h`](cubesat_icd.h) (I2C link) and [`ground_link.h`](ground_link.h) (radio link). This document explains them; **if they disagree, the header wins** and this document is fixed. Changes go through Parth and bump `ICD_VERSION_MINOR` (compatible) or `ICD_VERSION_MAJOR` (breaking).
**TBD** = filled in by the named owner before freeze. **Unverified** = written down, not yet measured on the bench.

## 0. Scope

- **Covered:** the I2C link between the Raspberry Pi 4B (OBC, master) and the two STM32F446 slaves, ADCS and TT&C; and the packet format on the LoRa link between TT&C and the ground station (section 11).
- **Not covered:**
  - The ADCS sensor bus (STM32 I2C1 → MPU-6050 and MMC5983MA). The OBC sees those sensors only through ADCS telemetry.
  - The TT&C sensor bus (TMP102).
  - EPS and payload, which the Pi drives directly: INA226 at 0x40, TSL2591 at 0x29, a USB camera, and the PYRO GPIO.
- The PCBs are manufactured. The software must fit the boards as built; only wire patches to existing pins are allowed.

## 1. Bus and addressing

The Pi is the only master. Slaves never speak first, so the Pi polls. 100 kHz, 7-bit addresses.

| Address | Device | Bus | Notes |
|---|---|---|---|
| **0x32** | ADCS STM32 | Pi link bus | STM32 I2C3: SCL PA8, SDA PC9. Header J11 (1 SDA, 2 SCL, 3 GND). 4.7 kΩ pull-ups on the board. |
| **0x33** | TT&C STM32 | Pi link bus | STM32 I2C3: SCL PA8, SDA PC9. Header J5 (1 SDA, 2 SCL, 3 GND). 4.7 kΩ pull-ups on the board. |
| 0x40 | INA226 (EPS) | Pi sensor bus | EPS J6 pins 3 and 4. Options 0x41–0x4F by strap. |
| 0x29 | TSL2591 | Pi sensor bus | Fixed. Reportedly also ACKs 0x28: confirm with `i2cdetect` and keep 0x28 free. |

- **Pi link bus:** `/dev/i2c-1`, GPIO2 SDA (pin 3), GPIO3 SCL (pin 5).
- **Pi sensor bus:** `/dev/i2c-3` (`dtoverlay=i2c3,pins_4_5`), GPIO4 SDA (pin 7), GPIO5 SCL (pin 29). Keeping the sensors on their own bus means a misbehaving sensor cannot hang the STM32 link.
- **Address check:** the only I2C chip on the EPS is the INA226 module. The CN3722 MPPT charger, the BMS and the buck converters have no I2C. 0x32 and 0x33 clash with nothing, and are outside the reserved ranges (0x00–0x07, 0x78–0x7F).
- **Clock stretching:** the STM32 slaves stretch SCL briefly at the start of each read while the interrupt loads the first byte. The Pi's hardware I2C controller handles stretching badly. If the link test shows errors, move the link to the bit-banged bus: `dtoverlay=i2c-gpio,bus=11,i2c_gpio_sda=23,i2c_gpio_scl=24,i2c_gpio_delay_us=2` and use `--bus 11`. The Raspberry Pi overlay documentation gives `delay_us=2` as about 100 kHz (the kernel's 500/delay formula would say 250 kHz, but the software toggling on the Pi adds time). The real speed is **unverified**: measure SCL with a scope or logic analyser at bring-up. If the link test shows errors at 2, try 3–5 (slower, still well inside the 20 ms reply budget).

## 2. Transaction

1. The Pi writes one command frame (START, addr+W, frame, STOP).
2. The Pi waits **≥ 5 ms** (`ICD_TURNAROUND_MS`).
3. The Pi reads **exactly 32 bytes** (START, addr+R, 32 bytes, STOP).

- If the slave hasn't finished processing, it answers `BUSY` and echoes the CMD_ID. The Pi re-reads up to **3 more times, 5 ms apart**.
- The slave keeps its response until the next command, so a repeated read returns the same frame:
  - **Bad CRC, bad LEN or wrong echo on the read:** the Pi re-reads. It never re-sends, so no command runs twice.
  - **The slave answered `BAD_CRC` or `BAD_LENGTH`:** the slave did not act on the frame, so the Pi re-sends it once.
- **Before the first command**, a read returns an idle frame: CMD 0x00, `NO_DATA`.
- **Slave rules:**
  - The interrupt only copies bytes in and out of buffers. The main loop validates and processes the frame, then stages the response.
  - When a command write ends, the interrupt stages a `BUSY` frame for it. If a newer command arrives while an older one is being processed, the older answer is dropped, so the Pi can never receive a stale answer.
  - At the start of each read the interrupt copies the frame to a shadow buffer, so the main loop can never change a frame halfway through a read.
  - Over-long writes are counted and rejected (`BAD_LENGTH`); reads past 32 bytes get 0x00 padding.
- **Response budget:** 5 ms + 3 × 5 ms = **20 ms**. No blocking call in a slave main loop may exceed about 10 ms, so a reply is always ready inside the budget.

## 3. Frames (little-endian)

```
Command  (Pi → slave): CMD_ID(1) | LEN(1) | PAYLOAD(0..24) | CRC16(2)                       4..28 bytes
Response (slave → Pi): CMD_ID(1) | STATUS(1) | LEN(1) | PAYLOAD(0..24) | CRC16(2) | 0x00 pad = 32 bytes
```

- LEN = payload bytes. Byte offsets are in the header (`ICD_CMD_OFS_*`, `ICD_RESP_OFS_*`).
- **CRC:** CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR). It covers every byte before it and is sent **low byte first**. Check value: `"123456789"` → **0x29B1**. Both sides run this self-test first.
- **No floats or structs on the wire.** Every field is packed one by one, little-endian (`icd_pack.h` in C, `struct` in Python).
- **Slave validation order:**
  1. Frame length 4..28, LEN ≤ 24 and frame length = LEN + 4, else `BAD_LENGTH`.
  2. CRC, else `BAD_CRC`.
  3. Command-specific payload length (`BAD_LENGTH`) and values (`BAD_PARAM`).

  A frame failing step 1 or 2 is **discarded and never acted on**, and does not count as master contact.
- **Pi validation:** exactly 32 bytes read, LEN ≤ 24, CRC good, CMD_ID echo matches the command sent. Anything else means re-read (section 2); if the retries run out, the transaction failed.

## 4. Status codes

| Code | Name | Meaning |
|---|---|---|
| 0x00 | OK | |
| 0x01 | BAD_CRC | Frame not acted on. |
| 0x02 | UNKNOWN_CMD | |
| 0x03 | BAD_LENGTH | Frame or payload length wrong. Not acted on. |
| 0x04 | BUSY | Response not ready yet. Only ever means this. |
| 0x05 | INVALID_MODE | Not allowed in this mode, or a forbidden mode change. |
| 0x06 | HW_FAULT | |
| 0x07 | NO_DATA | Nothing to return (empty TT&C RX queue, idle frame). |
| 0x08 | BAD_PARAM | A value is out of range, or an unknown telemetry page. |
| 0x09 | QUEUE_FULL | TT&C TX queue full. |

0x80–0x82 are used only on the ground link (section 11).

## 5. Common commands (both slaves)

| ID | Name | Request | Response payload |
|---|---|---|---|
| 0x01 | PING | — | — |
| 0x02 | GET_VERSION | — | fw major u8, minor u8, patch u8, ICD major u8, ICD minor u8 (5 B) |
| 0x03 | GET_STATUS | — | mode u8 @0, status_flags u8 @1, fault_flags u16 @2, uptime_s u32 @4 (8 B) |
| 0x04 | GET_TELEMETRY | page u8 | page data (≤ 24 B). Unknown page → `BAD_PARAM`. |
| 0x05 | SET_MODE | mode u8 | — (`INVALID_MODE` if not allowed, section 8) |

**Version check:** the Pi reads GET_VERSION at start-up. A different ICD **major** is an error; a different **minor** is a warning (compatible, but the slave should be rebuilt).

**status_flags:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | FAULT_ACTIVE | A fault is present right now. |
| 1 | GROUND_PKT_PENDING | TT&C only: a ground packet is waiting. Stays set while more are queued. |
| 2 | NO_MASTER_CONTACT | Set after 30 s with no valid transaction. Stays set until reported in a GET_STATUS, so the Pi learns about the gap. |
| 3 | RESET_OCCURRED | Set at every boot or watchdog reset. Cleared by the next GET_STATUS. On seeing it, the Pi re-sends the slave's configuration. |
| 4–7 | reserved | Sent as 0 (`ICD_SF_RESERVED_MASK`). |

**fault_flags** are sticky: any fault seen, even briefly, stays set until a GET_STATUS has reported it and the condition is gone.

| Bit | ADCS | TT&C |
|---|---|---|
| 0 | MAG (MMC5983MA no response / bad ID) | RADIO_INIT (SX1278) |
| 1 | IMU (MPU-6050 no response / bad ID) | RADIO_TX_TIMEOUT |
| 2 | DRVX nFAULT | TEMP_SENSOR (TMP102) |
| 3 | DRVY nFAULT | RX_OVERFLOW |
| 4 | DRVZ nFAULT | TX_OVERFLOW |
| 5 | LOCAL_I2C | OBC_I2C peripheral reset |
| 6 | OBC_I2C peripheral reset | LOCAL_I2C |
| 7 | SUN_ADC | — |

Bits 5 and 6 are in opposite order on the two slaves: always use the header names, never the numbers.

**Lost GET_STATUS replies.** The slave clears RESET_OCCURRED, NO_MASTER_CONTACT and gone-away sticky faults as soon as it has *answered*, not when the Pi has received the answer. Two Pi rules make sure nothing is lost:
- **Recovery read:** if a command was written but none of its reads succeeded, the Pi reads once more before writing its next command. The slave still holds that reply (section 2), so it is recovered and merged into the next status. The same rule catches a lost GET_RX reply, so a ground packet taken off the queue is not lost.
- **Uptime rule:** `uptime_s` going backwards means the slave rebooted.

## 6. Subsystem commands

Command ranges: ADCS **0x20–0x3F**, TT&C **0x40–0x5F** (`ICD_CMD_*_FIRST/LAST`).

**ADCS**

| ID | Name | Request | Notes |
|---|---|---|---|
| 0x20 | SET_BDOT_GAIN | gain u32 | duty‰ = −(gain/1000) · dB/dt [µT/s]. Max 10,000,000. Default 300,000. |
| 0x21 | SET_DETUMBLE_THRESH | exit u16, enter u16, dwell_s u16 | Rates in 0.01 °/s; enter must be > exit. Defaults 2.00, 5.00 and 60 s. |
| 0x22 | SET_BDOT_TIMING | period_ms u16, settle_ms u16 | Period 100–10000 ms. The coils are off for settle_ms before each magnetometer sample. |
| 0x23 | GET_CONFIG | — | 20 bytes: gain, exit, enter, dwell, period, settle, mag offset x, y, z |
| 0x24 | MTQ_MANUAL | duty x, y, z i16 (‰), duration_ms u16 | NOMINAL only, ≤ 10 s. 0 ms stops it. |
| 0x25 | SET_MAG_OFFSET | x, y, z i16 (magnetometer LSB) | Hard-iron offset, subtracted from every sample. |

**TT&C**

| ID | Name | Request / response | Notes |
|---|---|---|---|
| 0x40 | GET_RX | resp: one ground packet ≤ 24 bytes | `NO_DATA` if the queue is empty. Queue depth 8. |
| 0x41 | QUEUE_TX | req: 1–24 bytes | `QUEUE_FULL` if full. `INVALID_MODE` in SAFE. |
| 0x42 | SET_BEACON | req: 0–24 bytes | The OBC beacon. TT&C sends it while it is fresh and the OBC is alive, and its own beacon otherwise. |
| 0x43 | SET_BEACON_PERIOD | period_s u16 | 0 = off; otherwise ≥ 10 s. SAFE forces at least 60 s. |
| 0x44 / 0x45 | SET_RADIO / GET_RADIO | freq u32, sf u8, bw u8, cr u8, power i8, sync u8 (9 B) | 433–438 MHz, SF7–12, bw code 0–3 (62.5/125/250/500 kHz), CR 5–8 (4/5..4/8), 2–17 dBm. Applied when the radio is idle. 436.5 MHz is a **placeholder**: use the coordinated frequency. |
| 0x46 | BURN_ARM | key u32 = 0x4E525542 ("BURN") | Arms for 10 s. |
| 0x47 | BURN_FIRE | duration_ms u16 (≤ 8000) | Needs ARM and the RBF plug removed. Stops at the deadline or as soon as DEPLOY_SW reports deployed. |

**Ground command path:** TT&C receives a packet (it only queues ours: SAT_ID 0xC5 and a valid CRC). The Pi reads it with GET_RX, checks it (section 11), and forwards allowed commands to ADCS or TT&C. TT&C never talks to ADCS.

## 7. Telemetry (scaled integers, serialised field by field)

**Scales:**
- Magnetometer: **0.025 µT/LSB** (value = B[µT] × 40, ±819 µT, covering the MMC5983MA's ±800 µT range). The firmware converts the 18-bit raw count (16384 counts/G) with ×125/512.
- Gyro: **65.5 LSB per °/s**, raw MPU-6050 counts at **±500 °/s** (FS_SEL = 1). (v0.1 proposed ±250 °/s; it was widened for the post-deployment tumble. Change the range and this scale together.)

**ADCS page 0, sensors (18 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0 | uptime_ms | u32, ms since boot (wraps after 49.7 days; the Pi must handle the wrap) |
| 4, 6, 8 | mag X, Y, Z | i16, 0.025 µT |
| 10, 12, 14 | gyro X, Y, Z | i16, raw (65.5 LSB/°/s) |
| 16 | mag_age_ms | u16, age of the last clean sample; saturates at 65534; 0xFFFF = never valid |

ADCS switches the coils off around each magnetometer sample, so `mag_age_ms` is the age of the last clean sample. The Pi marks a sample stale when the age exceeds period + 500 ms, and never presents an old sample as fresh.

**ADCS page 1, sun sensors and coils (18 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0, 2, …, 10 | SS1..SS6 | u16, raw 12-bit ADC (0..4095). **TBD (ADCS owner):** which face each index is. |
| 12, 14, 16 | coil duty X, Y, Z | i16, ‰ of full drive, + = AOUT1 high |

> **Coil current is not measured.** Each DRV8833 AISEN goes to ground through a 1 Ω sense resistor (R1_0805, R1_806, R1_807). That sets the driver's own current limit (about 200 mV / 1 Ω = 200 mA per bridge). The sense node is not connected to the STM32, so page 1 reports the commanded duty instead of the v0.1 "coil current".

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
| 0 | uptime_ms | u32 (wraps after 49.7 days) |
| 4 | radio_state | u8: off / rx / tx |
| 5 | rx_queue | u8, packets waiting for the OBC |
| 6 | tx_queue | u8 |
| 7 | deploy_flags | u8: DEPLOY_SW, RBF_IN (raw pin levels), ARMED, BURNING, BURN_DONE |
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

**Dummy slave page 0 (24 bytes, link tests only):** uptime_ms, frames_ok, crc_errors, len_errors, overwritten (u32 each), bus_resets u16, reads (low 16 bits) u16. The dummy reports fw 0.1.0, which is how test tools tell it apart from TT&C (whose page 0 is also 24 bytes).

## 8. Modes

BOOT = 0 · SAFE = 1 · DETUMBLE = 2 · NOMINAL = 3

- The Pi can never request BOOT. From BOOT, only SAFE may be requested. SAFE, DETUMBLE and NOMINAL can be requested from each other. Anything else gets `INVALID_MODE`.
- SAFE is always accepted. A slave may drop itself to SAFE on a fault (and sets FAULT_ACTIVE).
- Why a slave is in its mode is reported as `mode_reason` (boot / command / fault / auto).

| Mode | ADCS | TT&C |
|---|---|---|
| BOOT | Self-test. Enters **DETUMBLE by itself**, without waiting for the Pi, or SAFE if the magnetometer is missing. | Initialises the radio. Goes to NOMINAL, or SAFE if the radio failed. |
| SAFE | DRV8833s asleep (nSLEEP low). Sensors still sampled, telemetry still served. If it entered SAFE on a fault, the fault has been gone for 30 s and the Pi is silent, it returns to DETUMBLE by itself. | RX on. Beacon at least every 60 s. QUEUE_TX and BURN refused. Returns to NOMINAL by itself when the radio recovers. |
| DETUMBLE | B-dot loop every period: coils off → settle → magnetometer sample → dB/dt → coils on. SAFE if the magnetometer fails 3 times or a driver fault lasts 3 cycles. Refused with `HW_FAULT` if the magnetometer is down. | Same as NOMINAL. |
| NOMINAL | Coils off (pointing law TBD). MTQ_MANUAL allowed. The Pi promotes ADCS here once it reports DETUMBLED. | RX, beacons, Pi downlink, burn wire. |

## 9. Timing and failures

- **Polling:** every `ICD_POLL_PERIOD_MS` (1 s) the Pi reads GET_STATUS from both slaves and telemetry pages 0 and 2 from ADCS. The slower pages are read every `ICD_SLOW_POLL_MS` (10 s).
- **Failed transaction:** a NACK or I/O error, a bad frame after all re-reads, an echo mismatch, or still BUSY after the retries. A failed transaction returns in well under one poll period; it never hangs.
- **COMM_FAULT:** three consecutive failed transactions mark a subsystem COMM_FAULT. It is logged once, polling continues, and it clears on the next success.
- **Lost master:** after 30 s with no valid transaction, a slave sets NO_MASTER_CONTACT and keeps its mode. ADCS keeps detumbling; TT&C keeps beaconing with its own beacon. Cleared by the next valid frame.
- **Bus errors:** on any bus error (BERR, ARLO, OVR, TIMEOUT), the slave resets its I2C peripheral and re-arms. It does the same if the bus is busy with no progress for 25 ms.
- **No alert wire:** faults and pending packets reach the Pi only through the status flags, so the worst-case latency is one poll (1 s).

## 10. Debugging

- **ADCS:** SWD + SWO on J16 (Tag-Connect). Logs go out over SWO/ITM, because ADCS has no free UART.
- **TT&C:** SWD on J4 (Tag-Connect TC2030); v0.1 said it had none. It also logs on USART1 at J6 (pin 2 = TX out, pin 1 = RX in, pin 3 = GND), 115200 8N1. The log must stay non-blocking (DMA or ring buffer) and covers boot reason, mode changes, every rejected frame with its status, and bus resets.
- Without a programmer, the Pi can flash both STM32s by SWD bit-bang and read the ADCS SWO log on its UART (see the main project's `docs/NO_PROGRAMMER.md`).

## 11. Ground link (`ground_link.h`, GL_VERSION 2)

Every packet: `SAT_ID(1) | TYPE(1) | SEQ(1) | body | CRC16(2)`, at most 24 bytes. The CRC is the same CRC-16 as on I2C, low byte first. Long answers go down as an ACK plus TLM parts; nothing is chunked on the uplink.

**Command packet (ground → satellite):**

```
SAT_ID | TYPE=0x01 | SEQ | TARGET | CMD | LEN | ARGS (0..8) | COUNTER u32 | MAC (4) | CRC16
```

- **MAC** = first 4 bytes of HMAC-SHA256(key, every byte from SAT_ID to COUNTER).
- **COUNTER** must be higher than the last one the OBC accepted. The OBC saves it before executing, so a recorded command cannot be replayed, even after an OBC restart. The ground tool uses `max(last + 1, unix time)`, so losing its counter file does not lock it out, as long as the PC clock is right.
- **Retransmission** (same packet, because an ACK was lost) is answered from the OBC's ACK cache and not executed again.
- **Rejected silently (no ACK):** bad CRC, bad MAC, an old unauthenticated (v1) packet, or an old counter. The OBC logs each one.
- **Key:** 32 random bytes as hex text, made with the ground tool's `keygen`. The same file goes on the Pi (`chmod 600`). It is never committed. Without a key file the OBC rejects every ground command (beacons still go out).
- ARGS is limited to 8 bytes (24 − 6 header − 4 counter − 4 MAC − 2 CRC). The largest allowed command, MTQ_MANUAL, is exactly 8.
- Downlink packets (ACK, TLM, beacons) are not authenticated; their CRC only protects integrity.

**What the OBC forwards to the slaves.** Only these commands are passed on; anything else gets `GL_ST_REFUSED (0x82)`:

| Target | Forwarded | Not forwarded, and why |
|---|---|---|
| ADCS | PING, GET_VERSION, GET_TELEMETRY, SET_MODE, SET_BDOT_GAIN, SET_DETUMBLE_THRESH, SET_BDOT_TIMING, GET_CONFIG, MTQ_MANUAL, SET_MAG_OFFSET | — |
| TT&C | PING, GET_VERSION, GET_TELEMETRY, SET_MODE, GET_RADIO, SET_BEACON_PERIOD, BURN_ARM, BURN_FIRE | GET_RX / QUEUE_TX / SET_BEACON: the OBC owns the queues and beacon. SET_RADIO: a wrong value cuts the uplink with no way back. |
| both | GET_STATUS is **answered from the OBC's last poll**, because a real one would clear the reset and fault flags before the OBC saw them. | |

- BURN_ARM and BURN_FIRE are refused while the OBC is in SAFE or the battery is low, the same rule as the pyro.
- ADCS settings and the beacon period changed from the ground are kept in the OBC's memory, so a slave reset re-applies them. An OBC restart goes back to the configuration file.

**Ground statuses:** 0x80 BAD_PACKET, 0x81 COMM_FAULT (the OBC could not reach the slave), 0x82 REFUSED.

**OBC commands (target 0):** PING 0x01, GET_HK 0x02 (16-byte `GL_HK_*` layout), SET_MODE 0x03, CAPTURE_IMAGE 0x10, GET_SLAVE_TLM 0x11 (target u8, page u8), REBOOT 0x1F (key "BOOT").

**EPS commands (target 3):** GET 0x01, PYRO_ARM 0x20 (key "PYRO"), PYRO_FIRE 0x21 (duration_ms u16). GET returns 8 bytes:

| Offset | Field | Type / unit |
|---|---|---|
| 0 | vbatt | u16, mV |
| 2 | ibus | i16, mA |
| 4 | pbus | u16, mW |
| 6 | low_battery | u8, 1 = low |
| 7 | pyro_fires | u8, count since OBC start (saturates at 255) |

**Beacons:** the OBC beacon (type 0x10, 22 bytes, `GL_BO_*`) is built by the OBC and sent by TT&C; TT&C's own beacon (type 0x11, 20 bytes, `GL_BT_*`) goes out when the OBC is silent.

## 12. Verification

| Item | Status (2026-11-09) |
|---|---|
| CRC-16 in C and Python, 0x29B1, agree on 5,000 random inputs | Passing on PC (`tests/test_icd.py`) |
| Every layout and constant adds up; header and Python in sync; generator rejects unparsable lines | Passing on PC |
| Reference slave (`slave/`) against the Python master: boot frame, common commands, bad frames not acted on, BUSY staging, newest command wins, mode rules, status flags, lost-reply recovery, uptime rule | Passing on PC |
| Link tests (a) bad frames, (b) unplug, (c) 10,000 PINGs, against the firmware simulation | 15/15 on PC (`python/test_link.py --sim-lib`) |
| Same link tests on the Pi against the Nucleo dummy slave and the real boards | **To run** (no ST-LINK or Nucleo yet; boards flashed from the Pi by SWD bit-bang) |

## 13. Open issues

1. **SAFE-mode downlink:** in SAFE, TT&C refuses QUEUE_TX, so the OBC's ACKs and downlink cannot go out; only beacons do. Commands sent in SAFE still run, but the ground sees no ACK. Fixing it needs a TT&C firmware change (owner: TT&C).
2. **Coil current limit:** the 1 Ω AISEN resistors cap each coil at about 200 mA. Confirm this suits the coil design; the detumble simulation does not model the cap (owner: ADCS).
3. **Unverified bench values:** axis map and coil polarity, deploy pin polarity, INA226 shunt value (many modules use 0.1 Ω, not 0.01 Ω), and the LoRa frequency.
4. **Sun sensor faces:** which face each SS1..SS6 index is (owner: ADCS).
5. **uptime_ms wrap** at 49.7 days (ADCS and TT&C page 0, and `uptime_s` derived from it). Acceptable for the EM as long as the Pi handles the wrap; a flight unit should use a wider clock.

**Resolved since v0.1:** EPS address check (only the INA226 is on I2C); bus split (sensors on i2c-3); BUSY no longer means "queue full" (QUEUE_FULL 0x09); RESET_OCCURRED loss on a lost reply (recovery read + uptime rule); gyro range (±500 °/s); TT&C pins, modes and telemetry pages (filled in); ADCS commands (0x20–0x25 defined).
