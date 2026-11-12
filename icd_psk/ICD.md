# Inter-Subsystem ICD v0.1: CM4 ↔ ADCS / TT&C I2C link

**Status:** draft v0.1, 2026-11-09, owner Parth. Values here are mirrored in `icd.h`; if they disagree, `icd.h` wins and this document is fixed.
**TBD** = filled in by the named subsystem owner before freeze. **Proposed** = drafted here, owner to confirm.

## 0. Scope
- Covers only the I2C link CM4 (OBC, master) ↔ ADCS (STM32F446) and CM4 ↔ TT&C (STM32F446).
- **Not covered:** the ADCS local sensor bus (STM32 I2C1 → MPU-6050, MMC5983MA). The CM4 sees magnetometer data only through ADCS telemetry. EPS and payload have no STM32; the CM4 drives them directly (EPS over I2C/GPIO, TSL2591 over I2C at 0x29, camera over USB).
- PCBs are in manufacturing. Only wire patches to existing pins are allowed.

## 1. Bus and addressing
- CM4 is the only master. Slaves never speak first; the CM4 polls. 100 kHz. 7-bit addresses.

| Address | Device | Bus | Notes |
|---|---|---|---|
| 0x29 | TSL2591 light sensor | CM4 I2C-1 (GPIO2/3) | Fixed. Reportedly also ACKs 0x28; confirm with `i2cdetect` and keep 0x28 free |
| **0x32** | ADCS STM32 | TBD | Proposed |
| **0x33** | TT&C STM32 | TBD | Proposed |
| TBD | EPS charger(s) | TBD | **EPS owner:** list every I2C part, its address from the datasheet, and any address-select pins |
| TBD | EPS monitor(s) / MPPT | TBD | as above |

- **Before freeze:** the EPS owner fills in the rows above. On a clash, move the STM32 addresses; never the fixed-address chips. 0x32/0x33 are outside the reserved ranges (0x00–0x07, 0x78–0x7F).
- **Bus split (TBD, OBC owner, from the schematic):** if existing pins allow, put ADCS/TT&C on a different CM4 I2C bus from EPS and the TSL2591 (the CM4 can enable extra controllers with `dtoverlay=i2c3`…`i2c6` on their GPIOs), so a misbehaving sensor cannot hang the STM32 link. If not, all devices share I2C-1.
- **Slave pins:** ADCS = I2C3, SCL PA8, SDA PC9. TT&C = **TBD (TT&C owner)**.
- Pull-ups: one set per bus, to 3.3 V. **TBD (OBC owner):** confirm where they are fitted.

## 2. Transaction model
1. CM4 writes one command frame (START, addr+W, frame, STOP).
2. CM4 waits **≥ 5 ms**.
3. CM4 reads **exactly 32 bytes** (START, addr+R, 32 bytes, STOP).
- If the response is not ready, the slave returns STATUS = BUSY. The CM4 re-reads up to **3 more times, 5 ms apart**. It never re-sends the command, so a retry can't run an actuator command twice.
- The slave always has a complete, valid 32-byte frame staged. At boot this is `CMD_ID=0x00, STATUS=BUSY`. When a command arrives, the slave stages BUSY for that command until the real response is ready.
- In the I2C interrupt the slave only copies bytes in or out of buffers. Parsing happens in the main loop. Over-long writes go to a sink and over-long reads get 0x00, so the slave never holds SCL low.
- Response budget: the slave main loop must answer within about 20 ms (5 ms + 3 × 5 ms retries), so no blocking call in the slave main loop may exceed that.

## 3. Frame format (little-endian)
```
Command  (CM4 → slave): CMD_ID(1) | LEN(1) | PAYLOAD(0..24) | CRC16(2)                      4..28 bytes
Response (slave → CM4): CMD_ID(1) | STATUS(1) | LEN(1) | PAYLOAD(0..24) | CRC16(2) | 0x00 pad  = 32 bytes
```
- LEN = payload bytes. CRC covers every byte before it and is sent low byte first.
- **CRC-16/CCITT-FALSE:** poly 0x1021, init 0xFFFF, no reflection, no final XOR. `"123456789"` → **0x29B1**. Both sides run this self-test first.
- **Slave validation order:** (1) frame length 4..28, LEN ≤ 24, and frame length = 4 + LEN, else BAD_LENGTH; (2) CRC, else BAD_CRC. A frame failing either is **discarded and never acted on**, and doesn't count as master contact. Then the command-specific payload length is checked (BAD_LENGTH).
- **CM4 validation:** 32 bytes read, LEN ≤ 24, CRC good, CMD_ID echo matches the command sent. Anything else is a failed transaction.

## 4. Status codes
0x00 OK · 0x01 BAD_CRC · 0x02 UNKNOWN_CMD · 0x03 BAD_LENGTH · 0x04 BUSY · 0x05 INVALID_MODE · 0x06 HW_FAULT · 0x07 NO_DATA

## 5. Common commands (both slaves)
| ID | Name | Request | Response payload |
|---|---|---|---|
| 0x01 | PING | — | — |
| 0x02 | GET_VERSION | — | fw major u8, minor u8, patch u8, ICD major u8, ICD minor u8 (5 B) |
| 0x03 | GET_STATUS | — | mode u8 @0, status_flags u8 @1, fault_flags u16 @2, uptime_s u32 @4 (8 B) |
| 0x04 | GET_TELEMETRY | page u8 | page data (≤ 24 B). Unknown page → NO_DATA (proposed) |
| 0x05 | SET_MODE | mode u8 | — (INVALID_MODE if not allowed, see §8) |

**status_flags:** bit0 FAULT_ACTIVE · bit1 GROUND_PKT_PENDING (TT&C only) · bit2 NO_MASTER_CONTACT · bit3 RESET_OCCURRED (set on any boot or watchdog reset, cleared by the next GET_STATUS read). Bits 4–7 reserved, sent as 0.
CM4 rule: also treat `uptime_s` going backwards as a reset, so a reset isn't missed if the GET_STATUS response that cleared bit3 was lost.

**fault_flags** (one bit per fault):
| Bit | ADCS (proposed, ADCS owner confirm) | TT&C (proposed, TT&C owner confirm) |
|---|---|---|
| 0 | Magnetometer fail | Radio init fail |
| 1 | IMU fail | Radio TX timeout |
| 2 | DRV8833 #1 nFAULT | TBD |
| 3 | DRV8833 #2 nFAULT | TBD |
| 4–15 | TBD | TBD |

## 6. Subsystem commands
- **ADCS 0x20–0x3F: TBD (ADCS owner).** Start with: set B-dot gain, set detumble entry/exit rate thresholds. For each: ID, payload layout, units/scale, allowed modes.
- **TT&C 0x40–0x5F:**
  - 0x40 TTC_GET_RX: no request payload. Response: one received ground packet (≤ 24 B), or STATUS = NO_DATA if none. GROUND_PKT_PENDING stays set while more are queued.
  - 0x41 TTC_QUEUE_TX: payload = bytes to transmit (1..24). Response: none. Queue-full behaviour: see open issue 3.
  - Others TBD (TT&C owner).
- EM ground packets are ≤ 24 bytes, so no chunking.
- **Ground command path:** TT&C receives → CM4 reads it (TTC_GET_RX) → CM4 decides → CM4 sends any command to ADCS. TT&C never talks to ADCS.

## 7. Telemetry layout (scaled integers only, no floats; serialize field by field or use packed structs, never compiler-padded structs)
**ADCS page 0: attitude sensors (18 B)**
| Offset | Field | Type | Units / scale |
|---|---|---|---|
| 0 | uptime_ms | u32 | ms since boot (wraps at 49.7 days; CM4 must handle the wrap) |
| 4, 6, 8 | mag X, Y, Z | i16 | **0.025 µT/LSB** (value = B[µT] × 40), covers the MMC5983MA's full ±800 µT range (proposed) |
| 10, 12, 14 | gyro X, Y, Z | i16 | raw MPU-6050, ±250 dps range, **131 LSB/dps** (proposed; if detumble needs more range, change FS_SEL and this scale together) |
| 16 | mag_age_ms | u16 | ms since the last valid mag sample, saturates at 65535 |

ADCS time-multiplexes the magnetometer with the MTQ coils, so mag data goes stale while coils are on. The CM4 logs mag_age_ms with every sample and must not treat a stale reading as fresh. **TBD (ADCS owner):** define the "stale" threshold.

**ADCS page 1: sun sensors and coil currents (18 B)**
| Offset | Field | Type | Units / scale |
|---|---|---|---|
| 0, 2, …, 10 | sun 0–5 | u16 | raw 12-bit ADC, 0..4095. **TBD (ADCS owner):** which face each index is |
| 12, 14, 16 | MTQ current X, Y, Z | u16 | **TBD (ADCS owner):** units, scale, sign convention |

**TT&C pages: TBD (TT&C owner).** Suggested page 0: radio state u8, last RX RSSI i16 (dBm), RX packet count u16, TX packet count u16, TX queue depth u8, beacon count u16.

## 8. Modes
BOOT = 0 · SAFE = 1 · DETUMBLE = 2 · NOMINAL = 3
- The CM4 cannot request BOOT (INVALID_MODE). The CM4 can request SAFE from any mode. SAFE ↔ DETUMBLE ↔ NOMINAL in any order. Anything else returns INVALID_MODE.
- A slave may drop itself to SAFE on a fault (and sets FAULT_ACTIVE).
- **ADCS autonomy:** after the boot self-test, ADCS enters DETUMBLE on its own and runs B-dot without waiting for the CM4.

| Mode | ADCS (ADCS owner fill in / confirm) | TT&C (TT&C owner fill in) |
|---|---|---|
| BOOT | Self-test of IMU, mag, DRV8833s; then → DETUMBLE (or SAFE on failure) | Radio init; then → TBD |
| SAFE | MTQ drivers asleep (nSLEEP low). Sensors still read, telemetry still served | TBD (beacon only?) |
| DETUMBLE | B-dot loop on mag, coils time-multiplexed with mag reads | TBD |
| NOMINAL | TBD | TBD |

## 9. Timing and failure behaviour
- CM4 polls GET_STATUS from each slave at 1 Hz, and GET_TELEMETRY from ADCS at 1 Hz.
- Failed transaction = I/O error (NACK, timeout), bad response frame, echo mismatch, or still BUSY after retries.
- **3 consecutive failed transactions:** the CM4 marks that subsystem COMM_FAULT, logs it, and keeps polling. One good transaction clears it.
- **No valid transaction for 30 s:** the slave sets NO_MASTER_CONTACT and keeps its current mode. ADCS keeps detumbling; TT&C keeps beaconing. Cleared by the next valid frame.
- **Any bus error on the slave** (BERR, ARLO, OVR): reset the I2C peripheral and re-arm listen mode.
- Slaves cannot interrupt the CM4. Faults and pending ground packets are signalled only through status_flags, so worst-case alert latency is one poll period (1 s). No alert wire in v0.1.

## 10. One source of truth
- `icd.h` holds the addresses, command IDs, status codes, modes, flag bits and telemetry offsets. Both STM32 projects include it; the CM4 code parses it at start-up (`cm4/icd_link.py`). Nobody redefines these locally.
- `icd.h` carries the ICD version (ICD_VERSION_MAJOR/MINOR), returned by GET_VERSION. The CM4 warns when a slave reports a different version.
- Any change goes through Parth and bumps the version.

## 11. Debugging
- ADCS: SWD header available.
- TT&C: no SWD. **TT&C owner:** lean UART log from day one: boot reason, mode changes, every rejected frame with its status, bus resets. Keep it non-blocking (DMA or ring buffer), so logging never delays the I2C main-loop work.

## 12. Verification (deliverables 3–5)
| Item | Status |
|---|---|
| CRC16 C and Python, 0x29B1 | Passing on PC |
| Dummy slave (Nucleo-F446RE): PING, GET_VERSION | Written; protocol logic tested on PC; needs build + run on the Nucleo |
| CM4 test script at 100 kHz | Written (`cm4/test_link.py`); needs a run on the CM4 |
| (a) corrupted CRC rejected | `test_link.py badcrc` — to run |
| (b) slave unplugged: times out, recovers | `test_link.py unplug` — to run |
| (c) 10,000 PINGs, zero errors | `test_link.py soak` — to run. If frames drop: i2c-gpio overlay (README) |

## 13. Open issues
1. **EPS address check** not done: needs the EPS part list (§1).
2. **Bus split** depends on which CM4 pins reach the STM32 boards (§1).
3. **BUSY is overloaded:** if TTC_QUEUE_TX uses BUSY for "queue full", the CM4 can't tell it from "response not ready" and will retry-read. Proposal: add status 0x08 QUEUE_FULL in v0.2, or return HW_FAULT for now.
4. **uptime_ms wrap** at 49.7 days (page 0). Acceptable if the CM4 unwraps it; otherwise switch to a u32 in 10 ms units.
5. **RESET_OCCURRED** clears on read, so a lost response loses the flag; mitigated by the uptime_s rule (§5).
6. **Gyro range** ±250 dps may be too small for the post-deployment tumble rate. ADCS owner to confirm.
