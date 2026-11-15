# Inter-Subsystem ICD v0.9: OBC ↔ ADCS / TT&C I2C link, and the ground link

**Status:** v0.9, 2026-11-15, owner Parth. v0.9 moves the TT&C defaults into India's licence-free band (433.3 MHz down, 433.8 MHz up, 10 dBm), adds automatic antenna deployment at boot with a flash flag, emergency commands that TT&C executes without the OBC (GL_T_ECMD, GL_VERSION 4), long FILE packets (QUEUE_TX_PART 0x48, 200-byte blocks, EXIF thumbnails), DEPLOY_RESET 0x49 (bench), and a confirm/revert timer for SET_RADIO (sections 6, 7, 8, 11). v0.8 splits the TT&C radio into a downlink (433.0 MHz) and a higher uplink (433.5 MHz) frequency, makes LoRa CR 4/8 the default, lets ACK/TLM go down in SAFE, and adds Selective Repeat file downlink (GL_VERSION 3; sections 6, 8, 11). v0.7 fits a sun diode on every face, adds gyro tracking of the Sun (dead-diode detection, HIDDEN instead of a false eclipse), the SUN_DIODE fault, SET_SUN_DISABLED 0x28 and page-4 dead_faces. v0.6 added the sun sensors' processed output: ADCS works out the Sun direction and sunlit/eclipse from its BPW34 diodes (5 faces, none on nadir) and reports them on telemetry page 4; SET_SUN_CONFIG 0x27 sets the light thresholds (sections 6, 7). v0.5 added the ADCS gyro zero-rate offset: SET_GYRO_OFFSET 0x26 (section 6), telemetry page 3 (section 7), automatic measurement at boot (section 8); page 0 gyro is now reported with the offset removed. v0.4 (2026-11-12) added the EPS GET_STATE ground command (section 11) and the INA226 #2 address, commented out. Earlier text edit (2026-11-12): the camera is a Pi camera on CSI, not USB; VEML6075 entries added commented out. Merges the v0.1 draft into v0.2 (what came from where: `README.md`).
The single source of truth is [`cubesat_icd.h`](cubesat_icd.h) (I2C link) and [`ground_link.h`](ground_link.h) (radio link). This document explains them; **if they disagree, the header wins** and this document is fixed. Changes go through Parth and bump `ICD_VERSION_MINOR` (compatible) or `ICD_VERSION_MAJOR` (breaking).
**TBD** = filled in by the named owner before freeze. **Unverified** = written down, not yet measured on the bench.

## 0. Scope

- **Covered:** the I2C link between the Raspberry Pi 4B (OBC, master) and the two STM32F446 slaves, ADCS and TT&C; and the packet format on the LoRa link between TT&C and the ground station (section 11).
- **Not covered:**
  - The ADCS sensor bus (STM32 I2C1 → MPU-6050 and MMC5983MA). The OBC sees those sensors only through ADCS telemetry.
  - The TT&C sensor bus (TMP102).
  - EPS and payload, which the Pi drives directly: INA226 at 0x40, TSL2591 at 0x29, a Raspberry Pi camera module on the CSI connector (captured with `rpicam-still`), and the PYRO GPIO.
<!--  - VEML6075 UV sensor at 0x10 on the Pi sensor bus (not fitted yet; see ICD_ADDR_VEML6075 in cubesat_icd.h). -->
  - The magnetometer data the payload logs comes from ADCS telemetry page 0 (section 7). ADCS samples it with the coils off; the OBC only records it.
- The PCBs are manufactured. The software must fit the boards as built; only wire patches to existing pins are allowed.

## 1. Bus and addressing

The Pi is the only master. Slaves never speak first, so the Pi polls. 100 kHz, 7-bit addresses.

| Address | Device | Bus | Notes |
|---|---|---|---|
| **0x32** | ADCS STM32 | Pi link bus | STM32 I2C3: SCL PA8, SDA PC9. Header J11 (1 SDA, 2 SCL, 3 GND). 4.7 kΩ pull-ups on the board. |
| **0x33** | TT&C STM32 | Pi link bus | STM32 I2C3: SCL PA8, SDA PC9. Header J5 (1 SDA, 2 SCL, 3 GND). 4.7 kΩ pull-ups on the board. |
| 0x40 | INA226 (EPS) | Pi sensor bus | EPS J6 pins 3 and 4. Options 0x41–0x4F by strap. |
| 0x29 | TSL2591 | Pi sensor bus | Fixed. Reportedly also ACKs 0x28: confirm with `i2cdetect` and keep 0x28 free. |
<!-- | 0x10 | VEML6075 | Pi sensor bus | Fixed. UV sensor, not fitted yet. Clashes with nothing on the sensor bus (0x28/0x29, 0x40). | -->
<!-- | 0x41 | INA226 #2 (battery current) | Pi sensor bus | Spliced into the battery cable before EPS J4 (no PCB change); A0 strapped to VCC. Not fitted yet. | -->

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
| 8 | SUN_DIODE (v0.7): a sun diode reads dark where the gyro-tracked Sun should light it | — |

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
| 0x26 | SET_GYRO_OFFSET | x, y, z i16 (gyro LSB) | v0.5. Gyro zero-rate offset, subtracted from every reading. Each axis ≤ 1310 LSB (20 °/s, the MPU-6050 limit), else `BAD_PARAM`. Overrides the boot measurement (page 3 state COMMANDED). |
| 0x27 | SET_SUN_CONFIG | lit u16, full u16 (ADC counts) | v0.6. full = a diode with the Sun straight on it (100–4095); lit = the albedo threshold, a face at or above it is sunlit (above 60, below full). Defaults 1200 / 3000, **TBD (measure)**. |
| 0x28 | SET_SUN_DISABLED | face mask u8 (bits 0–5) | v0.7. Faces ADCS must ignore (a diode known broken). Also forgets the dead diodes ADCS found itself. 0 = use all. |

**TT&C**

| ID | Name | Request / response | Notes |
|---|---|---|---|
| 0x40 | GET_RX | resp: one ground packet ≤ 24 bytes | `NO_DATA` if the queue is empty. Queue depth 8. |
| 0x41 | QUEUE_TX | req: 1–24 bytes | `QUEUE_FULL` if full. In SAFE only ACK and TLM packets are taken (v0.8); anything else gets `INVALID_MODE`. |
| 0x48 | QUEUE_TX_PART | flags u8 (bit 0 LAST), offset u8, data 1–22 | v0.9. Builds one downlink packet of up to 255 bytes. Offset 0 starts a new packet; each part must start where the last ended (else `BAD_PARAM`, packet dropped); over 255 bytes `BAD_LENGTH`. The LAST part queues it (`QUEUE_FULL`: start again). Refused in SAFE. TX queue: 8 slots of 255 bytes; RX queue: 8 of 24 (each queue has its own size since v0.9). |
| 0x42 | SET_BEACON | req: 0–24 bytes | The OBC beacon. TT&C sends it while it is fresh and the OBC is alive, and its own beacon otherwise. |
| 0x43 | SET_BEACON_PERIOD | period_s u16 | 0 = off; otherwise ≥ 10 s. SAFE forces at least 60 s. |
| 0x44 / 0x45 | SET_RADIO / GET_RADIO | freq_down u32, sf u8, bw u8, cr u8, power i8, sync u8, freq_up u32 (13 B, v0.8) | Both 433.05–438 MHz and **freq_up > freq_down**, SF7–12, bw code 0–3 (62.5/125/250/500 kHz), CR 5–8 (4/5..4/8), 2–17 dBm. Applied when the radio is idle. Defaults (v0.9): down **433.300 MHz**, up **433.800 MHz**, SF9, 125 kHz, **CR 4/8**, **10 dBm**, sync 0x12. **Confirm or revert (v0.9):** new settings are kept only if a valid ground packet arrives on them within 10 min; otherwise TT&C goes back to the last confirmed settings and raises RADIO_REVERTED (0x0080). TT&C listens on freq_up and retunes to freq_down for each packet it sends (half duplex: never both). The old 9-byte layout gets `BAD_LENGTH`. |
| 0x46 | BURN_ARM | key u32 = 0x4E525542 ("BURN") | Arms for 10 s. |
| 0x47 | BURN_FIRE | duration_ms u16 (≤ 8000) | Needs ARM and the RBF plug removed. `INVALID_MODE` while any burn (e.g. the automatic one) is running (v0.9). Stops at the deadline, when DEPLOY_SW has read deployed for 50 ms (debounced against burn-current noise), or if the RBF plug goes back in (v0.9). |
| 0x49 | DEPLOY_RESET | key u32 = 0x52414552 ("REAR") | v0.9, bench only: clears the antenna-deployed flag and the automatic burn count in flash. Only with the RBF plug **in** (else `INVALID_MODE`). Not forwarded from the ground. |

**Ground command path:** TT&C receives a packet (it only queues ours: SAT_ID 0xC5 and a valid CRC). The Pi reads it with GET_RX, checks it (section 11), and forwards allowed commands to ADCS or TT&C. TT&C never talks to ADCS over I2C; the only exception to "the OBC decides" is the emergency commands (section 11), which TT&C checks and executes itself, and which can pulse ADCS NRST and the Pi RUN / GLOBAL_EN lines.

## 7. Telemetry (scaled integers, serialised field by field)

**Scales:**
- Magnetometer: **0.025 µT/LSB** (value = B[µT] × 40, ±819 µT, covering the MMC5983MA's ±800 µT range). The firmware converts the 18-bit raw count (16384 counts/G) with ×125/512.
- Gyro: **65.5 LSB per °/s**, MPU-6050 counts at **±500 °/s** (FS_SEL = 1), with the zero-rate offset (page 3) removed since v0.5. (v0.1 proposed ±250 °/s; it was widened for the post-deployment tumble. Change the range and this scale together.)

**ADCS page 0, sensors (18 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0 | uptime_ms | u32, ms since boot (wraps after 49.7 days; the Pi must handle the wrap) |
| 4, 6, 8 | mag X, Y, Z | i16, 0.025 µT |
| 10, 12, 14 | gyro X, Y, Z | i16, 65.5 LSB/°/s, offset removed (v0.5; v0.4 sent the raw value) |
| 16 | mag_age_ms | u16, age of the last clean sample; saturates at 65534; 0xFFFF = never valid |

ADCS switches the coils off around each magnetometer sample, so `mag_age_ms` is the age of the last clean sample. The Pi marks a sample stale when the age exceeds period + 500 ms, and never presents an old sample as fresh.

**ADCS page 1, sun sensors and coils (18 bytes):**

| Offset | Field | Type / unit |
|---|---|---|
| 0, 2, …, 10 | SS1..SS6 | u16, raw 12-bit ADC (0..4095) from the BPW34 sun sensors (J4..J9). Face of each index: `ADCS/board_config.h` (placeholder SS1..SS6 = +X, −X, +Y, −Y, +Z, −Z). **Unverified.** |
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
| 7 | deploy_flags | u8: DEPLOY_SW, RBF_IN (raw pin levels), ARMED, BURNING, BURN_DONE, DEPLOYED_FLAG 0x20 and AUTO_ACTIVE 0x40 (v0.9) |
| 8 | last_rssi | i16, dBm |
| 10 | last_snr | i8, 0.25 dB |
| 11 | mode_reason | u8 |
| 12 | rx_count | u16 |
| 14 | tx_count | u16 |
| 16 | rx_crc_err | u16 |
| 18 | board_temp | i16, 0.01 °C |
| 20 | beacon_period | u16, s |
| 22 | obc_i2c_resets | u16 |

**TT&C page 1, counters (12 bytes):** tx_timeouts, rx_dropped, tx_dropped, beacon_count (u16 each), burn_count u8, auto_attempts u8 (v0.9: automatic burns ever made, from flash; was reserved), last_burn_ms u16.

**Dummy slave page 0 (24 bytes, link tests only):** uptime_ms, frames_ok, crc_errors, len_errors, overwritten (u32 each), bus_resets u16, reads (low 16 bits) u16. The dummy reports fw 0.1.0, which is how test tools tell it apart from TT&C (whose page 0 is also 24 bytes).

**ADCS page 3, gyro offset (12 bytes, v0.5):**

| Offset | Field | Type / unit |
|---|---|---|
| 0, 2, 4 | gyro offset X, Y, Z | i16, gyro LSB, subtracted from every reading |
| 6 | cal_state | u8: 0 NONE (no IMU at boot), 1 RUNNING, 2 AUTO (measured, board still), 3 REJECTED (board moved, or no magnetometer), 4 COMMANDED (SET_GYRO_OFFSET) |
| 7 | samples | u8, gyro reads averaged |
| 8 | gyro_spread | u16, gyro LSB: largest max − min of one axis over the window |
| 10 | mag_spread | u16, magnetometer LSB: largest max − min of one axis (raw, before the hard-iron offset) |

**ADCS page 4, Sun (16 bytes, v0.6):**

| Offset | Field | Type / unit |
|---|---|---|
| 0, 2, 4 | sun X, Y, Z | i16, unit vector × 10000, body frame; 0 when not valid |
| 6 | sun_flags | u8: 0x01 LIT (some working face ≥ lit), 0x02 VECTOR_VALID, 0x04 BLIND_SIDE (a component inferred), 0x08 HIDDEN (v0.7: dark, but the tracked Sun is behind an unavailable face: unknown, not eclipse), 0x10 PREDICTED (v0.7: vector is the gyro-tracked one). Eclipse = neither LIT nor HIDDEN. |
| 7 | lit_faces | u8 face bits: 0x01 +X, 0x02 −X, 0x04 +Y, 0x08 −Y, 0x10 +Z, 0x20 −Z |
| 8 | fitted_faces | u8 face bits with a diode (0x3F since v0.7) |
| 9 | dead_faces | u8 face bits ignored: detected dead or disabled (v0.7; reserved 0 in v0.6) |
| 10 | sun_max | u16 counts, brightest fitted face |
| 12, 14 | lit_threshold, full_scale | u16 counts in use (SET_SUN_CONFIG) |

**How the Sun direction is worked out (v0.6, v0.7).** Sun sensors (BPW34, attitude) and the payload solar-flux sensor (TSL2591 on the Pi, science) are separate: only the sun sensors are used here. Since v0.7 there is a diode on **every face**. With no pointing control any face can face the Sun, and v0.6's bare nadir face read as eclipse whenever it did (about 5 % of all directions).

- **Direction:** each face reads ≈ full × cos(angle to the Sun), 0 when the Sun is behind it; below 60 counts a face counts as dark. Of each opposite pair only the brighter face is used, because the Sun can light only one and the other reading is albedo or noise. That gives the axis component as ±(brighter / full).
- **Lit** is the albedo threshold: a face counts as sunlit only at or above it, so the Earth's glow cannot fake daylight in the Earth's shadow.
- **Unavailable faces:** a face is unavailable if it is not fitted, detected dead, or disabled with SET_SUN_DISABLED. If an axis has only one available side and that side is dark, the component comes from |s| = 1 (BLIND_SIDE).
- **Gyro tracking:** between measurements ADCS turns the last Sun direction with the gyro.
  - *Dead diodes:* with all diodes working the components add up to a unit vector. If, in daylight, they add up to less than √0.75, light is missing, and the dark face that can hold it, on the side the tracked Sun favours, is suspected. Still dark after 5 cycles while the tracked Sun lights it, that face is marked dead (fault SUN_DIODE). A face that reads light again is trusted again. This needs `full` within about 10 %.
  - *HIDDEN:* all faces dark while the tracked Sun is behind an unavailable face. The state is unknown, not eclipse, and the EPS drops ADCS's vote. The tracked direction is reported (PREDICTED).
  - *Limits:* tracking is believed for 5 min (gyro drift), so an eclipse that starts while the Sun is hidden shows only after that. Without a gyro there is no tracking.
- Computed once per control cycle, with the coils off.

**How the offset is measured (v0.5).** For `ADCS_GYRO_CAL_MS` (5 s) after every boot ADCS averages its gyro reads (every 100 ms). It keeps the mean only if the board was still: at least 40 reads, every gyro axis within 33 LSB (0.5 °/s), and every magnetometer axis within 20 LSB (0.5 µT) over at least 3 samples. The magnetometer is what catches a steady rotation, which the gyro alone cannot tell from an offset: 0.5 °/s turns a 30 µT field by about 1 µT between the first and last sample. Otherwise the state is REJECTED and the offset stays 0. So the offset can be measured on the bench (board still at power-on) but not after a reset in orbit (tumbling). The OBC therefore saves the last AUTO value and sends it with SET_GYRO_OFFSET when it sees NONE or REJECTED; `[adcs] gyro_offset` in obc.toml can force a value instead.

## 8. Modes

BOOT = 0 · SAFE = 1 · DETUMBLE = 2 · NOMINAL = 3

- The Pi can never request BOOT. From BOOT, only SAFE may be requested. SAFE, DETUMBLE and NOMINAL can be requested from each other. Anything else gets `INVALID_MODE`.
- SAFE is always accepted. A slave may drop itself to SAFE on a fault (and sets FAULT_ACTIVE).
- Why a slave is in its mode is reported as `mode_reason` (boot / command / fault / auto).

| Mode | ADCS | TT&C |
|---|---|---|
| BOOT | Self-test. Enters **DETUMBLE by itself**, without waiting for the Pi, or SAFE if the magnetometer is missing. Starts the 5 s gyro offset measurement (page 3). | Loads the flash flags, initialises the radio, goes to NOMINAL (or SAFE if the radio failed). Then the **deploy check** (v0.9): if the flash flag says not deployed and fewer than 3 automatic burns were ever made, it waits until the RBF plug has been out for 60 s without a break, burns 5 s (stopping early on DEPLOY_SW), and tries again 60 s later. DEPLOY_SW reading deployed for 2 s sets the flag, so later boots never burn on their own. Each attempt is counted in flash before the burn. |
| SAFE | DRV8833s asleep (nSLEEP low). Sensors still sampled, telemetry still served. If it entered SAFE on a fault, the faults that stop B-dot (magnetometer, coil drivers) have been gone for 30 s and the Pi is silent, it returns to DETUMBLE by itself; a dead gyro or sun-sensor ADC does not block this (v0.5). If the fault returns within 10 min of such a restart, the wait doubles each time (30 s … 32 min), and resets after 10 clean minutes or any commanded mode change. | RX on. Beacon at least every 60 s. OBC burn commands refused. QUEUE_TX takes ACK and TLM only (v0.8), so commands sent in SAFE are still acknowledged. Returns to NOMINAL by itself when the radio recovers. A **commanded** SAFE (OBC, low battery) also holds automatic deployment and E_BURN; a SAFE caused by a radio fault does not. Emergency commands work in every mode. |
| DETUMBLE | B-dot loop every period: coils off → settle → magnetometer sample → dB/dt → coils on. SAFE if the magnetometer fails 3 times or a driver fault lasts 3 cycles. Refused with `HW_FAULT` if the magnetometer is down. | Same as NOMINAL. |
| NOMINAL | Coils off (pointing law TBD). MTQ_MANUAL allowed. The Pi promotes ADCS here once it reports DETUMBLED. Any commanded mode change switches the coils off at once (v0.5; before, the old duty stayed on until the end of the control cycle). | RX, beacons, Pi downlink, burn wire. |

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
- Without a programmer, the Pi can flash both STM32s by SWD bit-bang and read the ADCS SWO log on its UART (see `../docs/NO_PROGRAMMER.md`).

## 11. Ground link (`ground_link.h`, GL_VERSION 3)

**Who does what.** TT&C (STM32) owns the radio: it schedules and sends every beacon, checks every uplink packet's satellite ID, length and CRC, and queues the good ones. The OBC (Pi) collects all telemetry, builds the OBC beacon and every ACK/TLM/FILE packet, checks the MAC and counter of each command and executes it (or forwards it). The OBC key never leaves the Pi. The one exception (v0.9) is **emergency commands** (below): TT&C checks those with its own key and executes them itself, so they work with the Pi hung.

**Framing, outermost first.** No bit stuffing anywhere: LoRa delimits frames itself, so no byte value is reserved.

```
LoRa PHY   preamble (8 + 4.25 sym) | sync word 0x12 | explicit header (length, CR, CRC on; own CRC) | payload | CRC16 (SX1278)
payload    SAT_ID 0xC5 | TYPE | SEQ | body | CRC16 (CCITT-FALSE, low byte first)          <= 24 bytes (FILE: <= 255, v0.9)
```

Protection: LoRa FEC (CR 4/8 default: every 4 data bits sent as 8) and interleaving; the LoRa CRC; our CRC16 (also covers the trip through the TT&C queues); the HMAC on commands; ACKs and Selective Repeat (below). The explicit header carries the coding rate, so a receiver decodes any CR without being told.

**Frequencies (v0.9).** Uplink 433.800 MHz, downlink 433.300 MHz (uplink above downlink). India's licence-free rules (2022, 433.05–434.79 MHz, 10 mW ERP, 10 % duty cycle; only 433.05–434.04 MHz allows a 125 kHz channel) put both channels inside 433.05–434.04 with ≥ 180 kHz to either edge, enough for crystal error (±20 ppm ≈ ±9 kHz) and Doppler (±11 kHz). v0.8's 433.000 MHz reached down to 432.94 MHz, outside the band. Default power 10 dBm (10 mW; keep antenna gain ≤ 0 dBd for 10 mW ERP). The SX1278 is half duplex, so TT&C listens on the uplink and retunes to the downlink for every packet it sends, then back. The ground station mirrors it: transmit on 433.800, receive on 433.300. Doppler at 433 MHz is at most about ±10 kHz in LEO, inside LoRa's offset tolerance at 125 kHz and SF9, so no Doppler correction is needed.

Airtime at the defaults (SF9, 125 kHz, CR 4/8): 280 ms for a 24-byte packet, 247 ms for the TT&C beacon (CR 4/5: 206 ms).

**Acknowledgements.** Every authenticated command gets exactly one ACK (status + up to 15 bytes), plus TLM parts for longer answers, in every mode (TT&C passes ACK/TLM in SAFE since v0.8). Every I2C transaction has a status reply (section 3). Not acknowledged: beacons (broadcast), single FILE blocks (acknowledged in groups by FILE_ACK), and packets dropped for a bad CRC, MAC or counter (silent on purpose: noise and forgeries get no reply).


Every packet: `SAT_ID(1) | TYPE(1) | SEQ(1) | body | CRC16(2)`, at most 24 bytes (only FILE packets may be up to 255, v0.9). The CRC is the same CRC-16 as on I2C, low byte first. Long answers go down as an ACK plus TLM parts; nothing is chunked on the uplink.

**Command packet (ground → satellite):**

```
SAT_ID | TYPE=0x01 | SEQ | TARGET | CMD | LEN | ARGS (0..8) | COUNTER u32 | MAC (4) | CRC16
```

- **MAC** = first 4 bytes of HMAC-SHA256(key, every byte from SAT_ID to COUNTER).
- **COUNTER** must be higher than the last one the OBC accepted. The OBC saves it before executing, so a recorded command cannot be replayed, even after an OBC restart. The ground tool uses `max(last + 1, unix time)`, so losing its counter file does not lock it out, as long as the PC clock is right.
- **Retransmission** (same packet, because an ACK was lost) is answered from the OBC's ACK cache and not executed again.
- **Rejected silently (no ACK):** bad CRC, bad MAC, an old unauthenticated (v1) packet, or an old counter. The OBC logs each one.
- **Key:** 32 random bytes as hex text, made with `OBC/scripts/keygen.py` together with the separate TT&C emergency key (which goes into the TT&C firmware, never the Pi). The OBC key goes on the Pi (`chmod 600`). Neither is ever committed (the repo is public; `.gitignore` lists both). Without a key file the OBC rejects every ground command (beacons still go out).
- ARGS is limited to 8 bytes (24 − 6 header − 4 counter − 4 MAC − 2 CRC). The largest allowed command, MTQ_MANUAL, is exactly 8.
- Downlink packets (ACK, TLM, beacons) are not authenticated; their CRC only protects integrity.

**Emergency commands (GL_T_ECMD 0x05, v0.9).** Same layout as a command (`SAT_ID | 0x05 | SEQ | TARGET | CMD | LEN | ARGS | COUNTER | MAC | CRC16`), signed with a **separate TT&C key** (`TTC_KEY_FILE` at firmware build time; the host tests use bytes 0..31). TT&C checks the MAC and the counter itself, keeps the counter in flash (written before acting, so a reboot never reopens a replay), answers a retransmission from its cached ACK, and sends the ACK itself, ahead of everything else, with `TARGET | 0x80` (GL_TGT_EMERGENCY). Bad MAC, old counter or bad length: no answer. ECMDs never reach the OBC; because TYPE is inside the MAC, a CMD can never be replayed as an ECMD or the other way round. A firmware built without a key ignores every ECMD.

| Target | Cmd | Args | Does |
|---|---|---|---|
| TT&C 2 | E_PING 0x01 | — | 15-byte status (`GL_EP_*`): uptime, mode, faults, seconds since the OBC last talked, deploy flags, auto attempts, watchdog setting/actions, silence left |
| TT&C | E_TTC_RESET 0x02 | key "BOOT" | Reboots TT&C after the ACK has gone out |
| TT&C | E_RADIO_DEFAULTS 0x03 | — | Back to the default radio settings, after the ACK (sent on the old ones) |
| TT&C | E_BEACON_PERIOD 0x04 | period_s u16 | As SET_BEACON_PERIOD |
| TT&C | E_TX_SILENCE 0x05 | minutes u16 (0 = end, ≤ 1440) | Stops beacons and OBC packets; only ECMD ACKs go out |
| TT&C | E_BURN 0x10 | duration_ms u16 | Burn wire in any mode except commanded SAFE; RBF must be out |
| OBC 0 | E_OBC_RESET 0x01 | key "BOOT" | Pulls the Pi 4B RUN pin low for 200 ms |
| OBC | E_OBC_POWER_CYCLE 0x02 | key, off_s u8 (1–60) | Holds the Pi GLOBAL_EN low (PMIC off) for off_s |
| OBC | E_OBC_WATCHDOG 0x03 | minutes u16 (0 = off, 5–1440) | Kept in flash. If the OBC is silent that long: RUN reset; after another period: 10 s power cycle; at most 3 per outage. **Off by default** (bench work with the OBC stopped) |
| ADCS 1 | E_ADCS_RESET 0x01 | key "BOOT" | Pulls ADCS NRST low for 20 ms |
| EPS 3 | — | — | `UNKNOWN_CMD`: the EPS has no processor and no TT&C line; power-cycling the Pi is the EPS-level recovery |

| TT&C | E_GET_EVENTS 0x06 | — | The last 3 events, newest first: no u16, target, cmd, result (15 B) |

The lines are PC6 (Pi RUN), PC8 (Pi GLOBAL_EN), PC7 (ADCS NRST), each through a 2N7002 (high = asserted), on the spare-GPIO header J8 (section 13, issue 6).

**Priority.** Emergency traffic goes first at every step:
- **Uplink:** an ECMD is checked and executed in the radio task the moment it arrives. It never waits in the 8-packet queue for the OBC.
- **Downlink:** ECMD ACKs and EVENTs have their own 4-packet queue, sent before beacons, OBC ACKs and FILE blocks. Radio silence does not stop them. If that queue is full, the oldest waiting packet makes room for a new ACK.
- **Getting heard:** the radio is half duplex. Before v0.9 TT&C started the next queued packet the instant the last one ended, so during a file burst it could not hear the ground at all. Now it stays in receive for at least 150 ms (`TTC_LISTEN_GAP_MS`) between non-priority packets. It also never starts a transmission while the SX1278 reports a packet coming in, for at most 1 s so noise cannot block it.
- **What the ground does:** repeat the ECMD every ~100 ms until its ACK comes back. In the simulation, with 8 long FILE packets queued, the ACK came back after the first one (without the gap: after all 8).
- **Cost:** about 8 % of file throughput.

**Execution reports (GL_T_EVENT 0x06, 18 bytes).** The ACK says the command was accepted. For an action that finishes later, the ACK carries one data byte, `GL_EA_REPORT_FOLLOWS`, and an EVENT reports how it ended:

| Action | EVENT cmd | Result / detail |
|---|---|---|
| E_OBC_RESET, E_OBC_POWER_CYCLE, E_ADCS_RESET | the same cmd | OK when the line is released |
| after an OBC reset or power cycle | OBC_BACK 0x80, or OBC_NOT_BACK 0x81 | detail = s until the Pi talked again; NOT_BACK after 300 s, result COMM_FAULT |
| E_BURN, automatic deployment burn | E_BURN | OK (deadline or antenna open), HW_FAULT (RBF back in), INVALID_MODE (commanded SAFE); detail = ms burned |
| E_RADIO_DEFAULTS, automatic revert | E_RADIO_DEFAULTS / RADIO_REVERTED 0x83 | sent **on the new settings**, where the ground now listens |
| E_TTC_RESET | E_TTC_RESET | sent after the reboot (pending flag in flash), with the ECMD's SEQ |
| E_TX_SILENCE running out | SILENCE_END 0x84 | — |
| Deployed flag set | DEPLOYED 0x82 | detail = deploy flags |
| Any other boot | BOOT 0x85 | — |
| Watchdog actions | E_OBC_RESET / E_OBC_POWER_CYCLE, source AUTO | then OBC_BACK / NOT_BACK |

Each EVENT carries the source (ecmd / auto), the ECMD's SEQ (0 if autonomous), an event number (gaps show a missed one) and the TT&C uptime. EVENTs are not acknowledged; E_GET_EVENTS reads the last three back. Commands that act at once (PING, BEACON_PERIOD, TX_SILENCE, OBC_WATCHDOG, GET_EVENTS) are complete when their ACK says OK.

**What the OBC forwards to the slaves.** Only these commands are passed on; anything else gets `GL_ST_REFUSED (0x82)`:

| Target | Forwarded | Not forwarded, and why |
|---|---|---|
| ADCS | PING, GET_VERSION, GET_TELEMETRY, SET_MODE, SET_BDOT_GAIN, SET_DETUMBLE_THRESH, SET_BDOT_TIMING, GET_CONFIG, MTQ_MANUAL, SET_MAG_OFFSET, SET_GYRO_OFFSET, SET_SUN_CONFIG, SET_SUN_DISABLED | — |
| TT&C | PING, GET_VERSION, GET_TELEMETRY, SET_MODE, GET_RADIO, SET_BEACON_PERIOD, BURN_ARM, BURN_FIRE | GET_RX / QUEUE_TX / SET_BEACON: the OBC owns the queues and beacon. SET_RADIO: a wrong value cuts the uplink with no way back. |
| both | GET_STATUS is **answered from the OBC's last poll**, because a real one would clear the reset and fault flags before the OBC saw them. | |

- BURN_ARM and BURN_FIRE are refused while the OBC is in SAFE or the battery is low, the same rule as the pyro.
- ADCS settings (including a gyro offset) and the beacon period changed from the ground are kept in the OBC's memory, so a slave reset re-applies them. An OBC restart goes back to the configuration file.

**Ground statuses:** 0x80 BAD_PACKET, 0x81 COMM_FAULT (the OBC could not reach the slave), 0x82 REFUSED.

**OBC commands (target 0):** PING 0x01, GET_HK 0x02 (16-byte `GL_HK_*` layout), SET_MODE 0x03, CAPTURE_IMAGE 0x10, GET_SLAVE_TLM 0x11 (target u8, page u8), REBOOT 0x1F (key "BOOT").

**EPS commands (target 3):** GET 0x01, GET_STATE 0x02 (v0.4, below), PYRO_ARM 0x20 (key "PYRO"), PYRO_FIRE 0x21 (duration_ms u16). GET returns 8 bytes:

| Offset | Field | Type / unit |
|---|---|---|
| 0 | vbatt | u16, mV |
| 2 | ibus | i16, mA |
| 4 | pbus | u16, mW |
| 6 | low_battery | u8, 1 = low |
| 7 | pyro_fires | u8, count since OBC start (saturates at 255) |

**EPS GET_STATE (0x02, added in v0.4)** returns 24 bytes (ACK with 15 bytes plus one TLM part with 9). The min/max fields restart after each GET_STATE; a retransmitted command is answered from the ACK cache, so a lost ACK loses nothing.

| Offset | Field | Type / unit |
|---|---|---|
| 0 | vbatt | u16, mV (battery bus VBUS) |
| 2 | iload | i16, mA: INA226 #1, VBUS → VBUS_DIST, the whole satellite |
| 4 | ibatt | i16, mA: INA226 #2, + discharging, − charging. `0x7FFF` = not fitted |
| 6 | psolar | u16, mW delivered by the charger (= load + charging power). `0xFFFF` without INA226 #2 |
| 8 | soc | u8, battery percent. `0xFF` = unknown |
| 9 | state | u8: 0 unknown, 1 eclipse, 2 sunlit charging, 3 sunlit full, 4 sunlit but discharging |
| 10 | flags | u8: INA_LOAD_OK, INA_BATT_FITTED, INA_BATT_OK, CLIPPED, VBUS_IMPLAUSIBLE, LOW_BATT, CRITICAL_BATT, SOC_FROM_VOLTAGE |
| 11 | sun | u8: ADCS_FRESH, TSL_FRESH, LIT |
| 12 | vmin | u16, mV since the last GET_STATE |
| 14 | vmax | u16, mV since the last GET_STATE |
| 16 | imax | u16, mA load peak since the last GET_STATE |
| 18 | e_load | u32, mWh used by the load since OBC start |
| 22 | state_age | u16, s in the current state (saturates at 65535) |

How the OBC fills it (the EPS has no MCU; the INA226 on J11 sees only the load):
- **Sun / eclipse** comes from the ADCS sun sensors (page 1, six faces) and the payload TSL2591. Lit if either says so; eclipse only if fresh ADCS data says dark.
- **Charging** without INA226 #2: the battery-voltage trend, corrected for the load (sunlit and falling = discharging; at the full voltage = full). With INA226 #2: the sign of the battery current.
- **SOC** without INA226 #2: the load-corrected voltage on a Li-ion rest-voltage curve (rough; reads high while charging; SOC_FROM_VOLTAGE set). With INA226 #2: charge counting, set to 100 % at full.

**Beacons:** the OBC beacon (type 0x10, 22 bytes, `GL_BO_*`) is built by the OBC and sent by TT&C; TT&C's own beacon (type 0x11, 20 bytes, `GL_BT_*`) goes out when the OBC is silent.

**File downlink, Selective Repeat ARQ (GL_VERSION 3).** For bulk data (Pi camera images). Commands keep one ACK per command.

| Cmd (target OBC) | Args | ACK data |
|---|---|---|
| FILE_START 0x20 | image u16 (0 = newest) [, variant u8: 0 full, 1 EXIF thumbnail (v0.9)] | xfer u8, size u32, blocks u16, crc32 u32, window u8, block_len u8 (`GL_FS_*`, 13 B since v0.9) |
| FILE_ACK 0x21 | xfer u8, base u16, bitmap u32 (`GL_FA_*`, 7 B) | blocks now being sent, u8 (0 = transfer complete and closed) |
| FILE_ABORT 0x22 | xfer u8 | — |

```
FILE packet:  SAT_ID | TYPE=0x04 | SEQ=xfer | BLOCK u16 | DATA (1..B) | CRC16      block k = bytes Bk .. Bk+B-1
```

1. FILE_START: the OBC answers with the size, block count and CRC-32, then sends blocks 0..W−1 (W = `file_window` in obc.toml, default 16, at most 32).
2. When the burst has ended (the satellite cannot hear the ground while it transmits), the ground sends FILE_ACK: `base` = the first block it is missing, bit i of `bitmap` = it has block base+i.
3. The OBC slides its window to `base` and sends every block in [base, base+W) that is not marked. Only lost blocks are sent again: that is what makes it Selective Repeat rather than Go-Back-N, which would resend the whole window after every loss.
4. Repeat until FILE_ACK has base = blocks; that ACK has data 0 and closes the transfer. The ground checks the CRC-32.

- **Timers live on the ground.** If nothing arrives (a lost FILE_ACK or a lost burst), the ground sends FILE_ACK again. Each FILE_ACK is a new request with a fresh counter, so the OBC acts on it instead of answering from its ACK cache.
- One transfer at a time; a new FILE_START replaces it. Dropped after `file_timeout_s` (default 1800 s) without a FILE_ACK. Refused (`REFUSED`) in SAFE or on low battery.
- Errors are acknowledged too: `BAD_LENGTH`, `NO_DATA` (no such image / no transfer), `BAD_PARAM` (wrong xfer, empty file, or over 65535 blocks = 1.1 MB).
- **Block size (v0.9):** B = `file_block` (default 200, 1–248), announced in the FILE_START answer. Packets over 24 bytes go to TT&C with QUEUE_TX_PART. v0.8 used B = 17 (one QUEUE_TX); each LoRa packet has ~12 symbols of fixed preamble and header, so small blocks wasted most of the airtime.
- **Throughput** (SF9/125 kHz/CR 4/8, W = 16, ~3 s turnaround per round, 150 ms listen gap): B = 17 ≈ 32 B/s, B = 200 ≈ 100 B/s, B = 248 ≈ 107 B/s. A 50 kB image: ~23 min → ~8 min. The EXIF thumbnail (rpicam `--thumb 160:120:60`, ~3–5 kB) comes down in under a minute. On the bench, SF7/CR 4/5 would give ~400 B/s. The 10 % duty-cycle limit of the licence-free band caps the long-run average at a tenth of these numbers.

## 12. Verification

| Item | Status (2026-11-14) |
|---|---|
| CRC-16 in C and Python, 0x29B1, agree on 5,000 random inputs | Passing on PC (`tests/test_icd.py`) |
| Every layout and constant adds up; header and Python in sync; generator rejects unparsable lines | Passing on PC |
| Reference slave (`slave/`) against the Python master: boot frame, common commands, bad frames not acted on, BUSY staging, newest command wins, mode rules, status flags, lost-reply recovery, uptime rule | Passing on PC |
| A second end-of-transfer after a read (STOP after the master's NACK) never re-runs the last command (`slave/icd_link.c` clears the receive buffer when a read starts) | Passing on PC (firmware host tests) |
| Gyro offset (v0.5): measured when still, rejected when tumbling, not spoiled by a SET_MAG_OFFSET in the window, SET_GYRO_OFFSET limits; OBC saves and re-sends it | Passing on PC (firmware host tests, `OBC/tests/test_gyro_offset.py`) |
| Sun direction and eclipse over a 324-point grid: six faces ≤ 1.2°, never dark in sunlight; albedo 0.3 × full from below: ≤ 0.3°; the albedo threshold keeps a shadowed satellite dark. A diode dying in orbit: found in 12 s, then HIDDEN instead of eclipse, tracked ≤ 4.2°; a wrong tracker never condemns a good diode; the 5-face build still passes. EPS: HIDDEN is not eclipse | Passing on PC (firmware host tests in both builds, `OBC/tests/test_sun.py`) |
| Link tests (a) bad frames, (b) unplug, (c) 10,000 PINGs, against the firmware simulation | 15/15 on PC (`python/test_link.py --sim-lib`) |
| v0.9 TT&C: SHA-256/HMAC against FIPS 180-4 / RFC 4231 vectors; flash journal (torn record, compaction, write failure); automatic deployment (delay, retry, flag across reboots, 3-burn lifetime cap, brown-out mid-burn, commanded SAFE, RBF re-inserted, DEPLOY_RESET rules); every emergency command, replay/forgery/retransmission, reboot keeps the counter, watchdog escalation; radio revert; QUEUE_TX_PART; LoRa airtime vs the Semtech formula; Python-signed ECMD accepted by the C firmware with the OBC not running; 200-byte FILE blocks and the EXIF thumbnail end to end | Passing on PC (firmware host tests, `OBC/tests`) |
| Same link tests on the Pi against the Nucleo dummy slave and the real boards | **To run** (no ST-LINK or Nucleo yet; boards flashed from the Pi by SWD bit-bang) |

## 13. Open issues

1. ~~SAFE-mode downlink~~: resolved in v0.8 (TT&C passes ACK and TLM in SAFE).
2. **Coil current limit:** the 1 Ω AISEN resistors cap each coil at about 200 mA. Confirm this suits the coil design; the detumble simulation does not model the cap (owner: ADCS).
3. **Unverified bench values:** axis map and coil polarity, **deploy pin polarity** (now critical: automatic deployment trusts DEPLOY_SW; a wrong polarity either sets the flag at once or burns 3 times), INA226 shunt value (many modules use 0.1 Ω, not 0.01 Ω). Frequencies moved inside 433.05–434.04 MHz in v0.9; still to check: antenna gain vs 10 mW ERP, the 10 % duty cycle during file downlink, and that a licence-free *ground* rule does not cover a satellite in orbit (that needs a WPC/IN-SPACe filing, normally in the amateur 435–438 MHz band) (owner: TT&C).
4. **Sun sensor faces:** which face each SS1..SS6 index is (placeholder in `ADCS/board_config.h`: SS1..SS6 = +X, −X, +Y, −Y, +Z, −Z), plus `full` (within ~10 %) and `lit` (above the albedo glow) measured in sunlight (owner: ADCS).
5. **uptime_ms wrap** at 49.7 days (ADCS and TT&C page 0, and `uptime_s` derived from it). Acceptable for the EM as long as the Pi handles the wrap; a flight unit should use a wider clock.
6. **Emergency lines have no connector yet (v0.9):** the schematic has a spare-GPIO header J8 (2×13, DNP) but the routed PCB has **no J8 footprint**, so PC6/PC7/PC8 only reach the MCU pins. Add J8 (or a 1×4 header) with three 2N7002 stages before ordering the PCB, wired to Pi 4B RUN, Pi 4B GLOBAL_EN and ADCS NRST. Check on the bench that releasing GLOBAL_EN restarts the Pi (owner: TT&C).
7. ~~Full-chip erase wipes TT&C's flags~~: mitigated in v0.9.
   - **Tools:** `program` only erases the sectors the image covers, and the linker script keeps code below sector 7. `swd_flash.py erase` now erases sectors 0–6 only. A full wipe needs `swd_flash.py wipe-flags` and typing WIPE. Avoid "full chip erase" in STM32CubeProgrammer.
   - **If it happens anyway:**
     - TT&C boots with a blank store and sends EVENT NV_BLANK (0x86), so the ground knows.
     - A deployed antenna is found again from DEPLOY_SW in 2 s, long before any burn could start.
     - Every build embeds its build time as the lowest emergency counter accepted (`ttc_ecmd_ctr_floor`). The ground's counters are Unix time, so any command recorded before that build is still refused.
   - **What is still lost:** the burn count (a stowed antenna gets up to 3 more automatic burns), and replay protection for commands recorded after the build but before the wipe.

**Resolved since v0.1:** EPS address check (only the INA226 is on I2C); bus split (sensors on i2c-3); BUSY no longer means "queue full" (QUEUE_FULL 0x09); RESET_OCCURRED loss on a lost reply (recovery read + uptime rule); gyro range (±500 °/s); TT&C pins, modes and telemetry pages (filled in); ADCS commands (0x20–0x27 defined); sun direction and eclipse from the sun sensors (v0.6), on all six faces with gyro tracking and dead-diode detection (v0.7); gyro zero-rate offset (v0.5: measured at boot, page 3, SET_GYRO_OFFSET).
