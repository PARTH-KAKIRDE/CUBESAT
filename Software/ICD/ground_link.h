/*
 * ground_link.h - packet format on the LoRa link (satellite <-> ground station).
 *
 * Shared by TT&C firmware (uplink filter + its own beacon), the OBC (command
 * decoding, ACKs, OBC beacon) and the ground station tool. Same generator
 * rules as cubesat_icd.h: plain integer #defines only.
 *
 * Every packet: SAT_ID(1) | TYPE(1) | SEQ(1) | body ... | CRC16(2)
 *   - max GL_MAX_LEN bytes in total (the ICD TTC_QUEUE_TX / GET_RX limit).
 *     Only FILE packets may be longer (up to GL_DOWN_MAX_LEN, v4): the OBC
 *     hands them to TT&C in parts (TTC_CMD_QUEUE_TX_PART).
 *   - CRC-16/CCITT-FALSE (same as the I2C link) over every byte before it,
 *     low byte first. LoRa has its own CRC; this one also protects the path
 *     through the TT&C queues and rejects other users on the frequency.
 *   - CMD packets (ground -> sat) are authenticated (since GL_VERSION 2):
 *       ... args | COUNTER u32 | MAC(4) | CRC16
 *     MAC = first 4 bytes of HMAC-SHA256(key, every byte before the MAC).
 *     COUNTER must be higher than the last one the OBC accepted, so a recorded
 *     command cannot be replayed. A retransmission (same packet) is answered
 *     from the OBC's ACK cache and is not executed again. The key is a secret
 *     file on the Pi and the ground PC, never in this repo.
 *     Downlink packets (ACK, TLM, beacons) are not authenticated.
 *   - TT&C only filters on SAT_ID, length and CRC; the OBC checks the MAC.
 *   - ECMD packets (v4) have the CMD layout but TYPE = GL_T_ECMD and a
 *     separate key that only TT&C and the ground hold. TT&C checks MAC and
 *     counter itself, executes them without the OBC (OBC hung, ADCS stuck)
 *     and sends the ACK itself, with GL_TGT_EMERGENCY set in TARGET. Its
 *     counter is kept in TT&C flash, so a reboot does not reopen replays.
 *   - No bit stuffing: LoRa delimits frames itself (preamble, sync word,
 *     explicit header with the length), so no byte value is reserved.
 *     Errors are handled by LoRa FEC (CR 4/8 default), the LoRa CRC, this
 *     CRC16, the uplink MAC and the Selective Repeat file downlink (v3).
 *   - Every authenticated CMD gets exactly one ACK (plus TLM parts). Beacons
 *     and FILE blocks are not acknowledged one by one: beacons are broadcast,
 *     FILE blocks are acknowledged in groups by GL_OBC_FILE_ACK.
 */
#ifndef GROUND_LINK_H
#define GROUND_LINK_H

#define GL_VERSION                  4    /* 2: authenticated CMD, 3: file downlink, 4: ECMD, long FILE */
#define GL_SAT_ID                   0xC5
#define GL_MAX_LEN                  24
#define GL_HDR_LEN                  3
#define GL_CRC_LEN                  2
#define GL_DOWN_MAX_LEN             255  /* v4: longest FILE packet (LoRa limit) */

#define GL_OFS_SAT                  0
#define GL_OFS_TYPE                 1
#define GL_OFS_SEQ                  2

/* Packet types */
#define GL_T_CMD                    0x01 /* ground -> sat */
#define GL_T_ACK                    0x02 /* sat -> ground, answer to a CMD */
#define GL_T_TLM                    0x03 /* sat -> ground, extra data for a CMD */
#define GL_T_FILE                   0x04 /* sat -> ground, one block of a file (v3) */
#define GL_T_ECMD                   0x05 /* ground -> TT&C, emergency command (v4) */
#define GL_T_EVENT                  0x06 /* TT&C -> ground, an emergency action finished (v4) */
#define GL_T_BEACON_OBC             0x10 /* OBC housekeeping, sent by TT&C */
#define GL_T_BEACON_TTC             0x11 /* TT&C's own beacon when OBC is silent */

/* Targets */
#define GL_TGT_OBC                  0
#define GL_TGT_ADCS                 1    /* forwarded as an ICD command (allowlist in ICD.md) */
#define GL_TGT_TTC                  2    /* forwarded as an ICD command (allowlist in ICD.md) */
#define GL_TGT_EPS                  3    /* handled by the OBC EPS driver   */
#define GL_TGT_EMERGENCY            0x80 /* v4: set in an ACK TARGET when TT&C answered an ECMD */

/* CMD body */
#define GL_CMD_OFS_TARGET           3
#define GL_CMD_OFS_CMD              4
#define GL_CMD_OFS_LEN              5
#define GL_CMD_OFS_ARGS             6
#define GL_AUTH_CTR_LEN             4    /* u32 counter, right after the args */
#define GL_AUTH_MAC_LEN             4    /* truncated HMAC-SHA256, before the CRC */
#define GL_CMD_MAX_ARGS             8    /* 24 - 6 - 4 - 4 - 2 */

/* ACK body */
#define GL_ACK_OFS_TARGET           3
#define GL_ACK_OFS_CMD              4
#define GL_ACK_OFS_STATUS           5    /* ICD_ST_* or GL_ST_* */
#define GL_ACK_OFS_LEN              6    /* total response length (may be > data here) */
#define GL_ACK_OFS_DATA             7
#define GL_ACK_MAX_DATA             15   /* rest follows in GL_T_TLM parts */

/* TLM body (continuation of a long response) */
#define GL_TLM_OFS_TARGET           3
#define GL_TLM_OFS_CMD              4
#define GL_TLM_OFS_OFFSET           5    /* byte offset into the response */
#define GL_TLM_OFS_DATA             6
#define GL_TLM_MAX_DATA             16

/* Extra statuses used only on the ground link */
#define GL_ST_BAD_PACKET            0x80
#define GL_ST_COMM_FAULT            0x81 /* OBC could not reach the slave */
#define GL_ST_REFUSED               0x82 /* OBC safety check or allowlist said no */

/* OBC commands (target GL_TGT_OBC) */
#define GL_OBC_PING                 0x01
#define GL_OBC_GET_HK               0x02 /* response: GL_HK_* layout */
#define GL_OBC_SET_MODE             0x03 /* args: mode u8 (ICD_MODE_*) */
#define GL_OBC_CAPTURE_IMAGE        0x10 /* Pi camera (CSI) snapshot saved on the OBC */
#define GL_OBC_GET_SLAVE_TLM        0x11 /* args: target u8, page u8 */
#define GL_OBC_REBOOT               0x1F /* args: key u32 = GL_REBOOT_KEY */
#define GL_OBC_FILE_START           0x20 /* v3, args: image u16 (0 = newest) [, variant u8 v4]; resp: GL_FS_* */
#define GL_OBC_FILE_ACK             0x21 /* v3, args: GL_FA_* (base + bitmap); resp: sent u8 */
#define GL_OBC_FILE_ABORT           0x22 /* v3, args: xfer u8 */
#define GL_REBOOT_KEY               0x544F4F42 /* "BOOT" little-endian */

/* EPS commands (target GL_TGT_EPS) */
#define GL_EPS_GET                  0x01 /* response: GL_EPS_OFS_* layout */
#define GL_EPS_GET_STATE            0x02 /* response: GL_EPSS_OFS_* layout (v0.4); resets the min/max */
#define GL_EPS_PYRO_ARM             0x20 /* args: key u32 = GL_PYRO_KEY */
#define GL_EPS_PYRO_FIRE            0x21 /* args: duration_ms u16 */
#define GL_PYRO_KEY                 0x4F525950 /* "PYRO" little-endian */

/* File downlink, Selective Repeat ARQ (v3).
 *
 * FILE packet: SAT_ID | TYPE=FILE | SEQ=xfer id | BLOCK u16 | DATA | CRC16
 * Every block holds B bytes (B from the FILE_START answer, 1..248); block k
 * is file bytes k*B .. k*B+B-1 and only the last block is shorter. v3 had
 * B = 17 always (24-byte packets); v4 lets the OBC pick a larger B (default
 * 200) to cut the per-packet overhead about 3x.
 *
 *   ground  FILE_START(image)  ->  sat  ACK (GL_FS_*), then blocks 0..W-1
 *   ground  FILE_ACK(base, bitmap) -> sat  ACK (sent u8), then every block in
 *           [base, base+W) whose bitmap bit is 0. Bit i = block base+i was received.
 *           base = the lowest block the ground does not have yet.
 *   ...until FILE_ACK with base = blocks: ACK, sent = 0, transfer closed.
 *
 * The ground sends FILE_ACK when the burst has ended (the radio is half duplex)
 * or when its timer runs out. A FILE_ACK is a request, not a retransmission:
 * it always gets a fresh counter, so the OBC acts on it again (resends the
 * missing blocks) instead of answering from its ACK cache. */
#define GL_FILE_OFS_BLOCK           3    /* u16 */
#define GL_FILE_OFS_DATA            5
#define GL_FILE_SHORT_DATA          17   /* 24 - 5 - 2: the block size that fits one QUEUE_TX */
#define GL_FILE_MAX_DATA            248  /* v4: 255 - 5 - 2 */
#define GL_FILE_WINDOW_MAX          32   /* bits in the FILE_ACK bitmap */
#define GL_FILE_MAX_BLOCKS          65535

/* FILE_START variant (v4, optional third argument byte; absent = full) */
#define GL_FV_FULL                  0    /* the JPEG as captured */
#define GL_FV_THUMB                 1    /* its embedded EXIF thumbnail (rpicam --thumb) */

/* FILE_START response, 13 bytes (fits one ACK) */
#define GL_FS_OFS_XFER              0    /* u8 transfer id, = SEQ of the FILE packets */
#define GL_FS_OFS_SIZE              1    /* u32 bytes */
#define GL_FS_OFS_BLOCKS            5    /* u16 */
#define GL_FS_OFS_CRC32             7    /* u32 CRC-32 (zlib) of the whole file */
#define GL_FS_OFS_WINDOW            11   /* u8 W, blocks per burst (1..32) */
#define GL_FS_OFS_BLOCK_LEN         12   /* u8 B, bytes per block (v4) */
#define GL_FS_LEN                   13

/* FILE_ACK arguments, 7 bytes (fits GL_CMD_MAX_ARGS) */
#define GL_FA_OFS_XFER              0    /* u8 */
#define GL_FA_OFS_BASE              1    /* u16 */
#define GL_FA_OFS_BITMAP            3    /* u32, bit i = block base+i received */
#define GL_FA_LEN                   7

/* Emergency commands (GL_T_ECMD, v4), executed by TT&C itself.
 * Same layout and MAC rule as CMD, with the TT&C emergency key. Reset and
 * power commands take a key u32 so a bit error can never trigger them. */
#define GL_E_PING                   0x01 /* TGT_TTC; resp: GL_EP_* */
#define GL_E_TTC_RESET              0x02 /* TGT_TTC, args: key u32 = GL_REBOOT_KEY; resets after the ACK */
#define GL_E_RADIO_DEFAULTS         0x03 /* TGT_TTC; back to the default radio settings after the ACK */
#define GL_E_BEACON_PERIOD          0x04 /* TGT_TTC, args: period_s u16 (0 = off, else >= 10) */
#define GL_E_TX_SILENCE             0x05 /* TGT_TTC, args: minutes u16 (0 = end); only ECMD ACKs go out */
#define GL_E_GET_EVENTS             0x06 /* TGT_TTC; resp: the last GL_EVL_COUNT events, newest first */
#define GL_E_BURN                   0x10 /* TGT_TTC, args: duration_ms u16; RBF must be out, any mode */
#define GL_E_OBC_RESET              0x01 /* TGT_OBC, args: key u32 = GL_REBOOT_KEY; pulses the Pi RUN line */
#define GL_E_OBC_POWER_CYCLE        0x02 /* TGT_OBC, args: key u32, off_s u8 (1..60); holds GLOBAL_EN low */
#define GL_E_OBC_WATCHDOG           0x03 /* TGT_OBC, args: minutes u16 (0 = off, else 5..1440), kept in flash */
#define GL_E_ADCS_RESET             0x01 /* TGT_ADCS, args: key u32 = GL_REBOOT_KEY; pulses ADCS NRST */

#define GL_E_OBC_RESET_PULSE_MS     200
#define GL_E_ADCS_RESET_PULSE_MS    20
#define GL_E_POWER_OFF_MAX_S        60
#define GL_E_SILENCE_MAX_MIN        1440
#define GL_E_WDT_MIN_MIN            5
#define GL_E_WDT_MAX_MIN            1440
#define GL_E_WDT_MAX_ACTIONS        3    /* resets/power cycles per OBC outage */

/* ACK data of an accepted command whose action finishes later (resets,
 * power cycle, burn, radio defaults): 1 byte GL_EA_REPORT_FOLLOWS. The
 * EVENT packet with the same TARGET/CMD reports the outcome. Commands that
 * act at once (PING, BEACON_PERIOD, TX_SILENCE, OBC_WATCHDOG, GET_EVENTS)
 * are done when their ACK says OK. */
#define GL_EA_REPORT_FOLLOWS        1

/* EVENT packet (GL_T_EVENT), 18 bytes: SAT | 0x06 | SEQ = event no low byte | ... | CRC16.
 * Sent by TT&C ahead of all normal traffic when an emergency action ends, or
 * when TT&C acts on its own (watchdog, automatic deployment, radio revert,
 * boot). Not acknowledged: E_GET_EVENTS re-reads the last ones, and the
 * event number shows gaps. */
#define GL_EV_OFS_NO                3    /* u16 event number since TT&C boot (1, 2, ...) */
#define GL_EV_OFS_SOURCE            5    /* u8 GL_EVS_* */
#define GL_EV_OFS_TARGET            6    /* u8 GL_TGT_* acted on */
#define GL_EV_OFS_CMD               7    /* u8 GL_E_* that finished, or GL_EV_* */
#define GL_EV_OFS_RESULT            8    /* u8 ICD_ST_* / GL_ST_* outcome */
#define GL_EV_OFS_ECMD_SEQ          9    /* u8 SEQ of the ECMD that asked for it (0 if autonomous) */
#define GL_EV_OFS_DETAIL            10   /* u16 per event: ms burned / held, s until the OBC came back, ... */
#define GL_EV_OFS_UPTIME_S          12   /* u32 TT&C uptime */
#define GL_EV_LEN                   18

#define GL_EVS_ECMD                 1    /* asked for by an emergency command */
#define GL_EVS_AUTO                 2    /* TT&C decided by itself */

/* EVENT CMD values that are not GL_E_* commands */
#define GL_EV_OBC_BACK              0x80 /* TGT_OBC: the OBC talked again after a reset/power cycle; detail = s */
#define GL_EV_OBC_NOT_BACK          0x81 /* TGT_OBC: still silent GL_EV_OBC_WAIT_S later; result COMM_FAULT */
#define GL_EV_DEPLOYED              0x82 /* TGT_TTC: antenna-deployed flag set; detail = TTC_DF_* */
#define GL_EV_RADIO_REVERTED        0x83 /* TGT_TTC: unconfirmed SET_RADIO undone; detail = revert count */
#define GL_EV_SILENCE_END           0x84 /* TGT_TTC: E_TX_SILENCE time ran out */
#define GL_EV_BOOT                  0x85 /* TGT_TTC: TT&C started (not by E_TTC_RESET) */
#define GL_EV_NV_BLANK              0x86 /* TGT_TTC: flag store was blank (first boot or wiped) */
#define GL_EV_OBC_WAIT_S            300

/* E_GET_EVENTS response: GL_EVL_COUNT entries of 5 bytes, newest first, 0 = none */
#define GL_EVL_COUNT                3
#define GL_EVL_ENTRY_LEN            5    /* no u16, target u8, cmd u8, result u8 */

/* E_PING response, 15 bytes (one ACK) */
#define GL_EP_OFS_UPTIME_S          0    /* u32 TT&C uptime */
#define GL_EP_OFS_MODE              4    /* u8 TT&C mode */
#define GL_EP_OFS_FAULTS            5    /* u16 TTC_FLT_* (live + sticky) */
#define GL_EP_OFS_OBC_SILENT_S      7    /* u16 s since the OBC last talked to TT&C (saturates) */
#define GL_EP_OFS_DEPLOY_FLAGS      9    /* u8 TTC_DF_* */
#define GL_EP_OFS_AUTO_ATTEMPTS     10   /* u8 */
#define GL_EP_OFS_WDT_MIN           11   /* u16 OBC watchdog setting, 0 = off */
#define GL_EP_OFS_WDT_ACTIONS       13   /* u8 resets/power cycles done in this outage */
#define GL_EP_OFS_SILENCE_MIN       14   /* u8 minutes of radio silence left (saturates) */
#define GL_EP_LEN                   15

/* OBC beacon (GL_T_BEACON_OBC), body offsets, 22 bytes total */
#define GL_BO_OFS_UPTIME_S          3    /* u32 */
#define GL_BO_OFS_OBC_MODE          7    /* u8 */
#define GL_BO_OFS_VBATT_MV          8    /* u16 */
#define GL_BO_OFS_IBUS_MA           10   /* i16 */
#define GL_BO_OFS_ADCS_MODE         12   /* u8 */
#define GL_BO_OFS_TTC_MODE          13   /* u8 */
#define GL_BO_OFS_ADCS_RATE         14   /* u16 0.01 dps */
#define GL_BO_OFS_COMM_FLAGS        16   /* u8 GL_CF_* */
#define GL_BO_OFS_CPU_TEMP          17   /* i8 degC */
#define GL_BO_OFS_LUX               18   /* u16 lux (saturated) */
#define GL_BO_LEN                   22

#define GL_CF_ADCS_COMM_FAULT       0x01
#define GL_CF_TTC_COMM_FAULT        0x02
#define GL_CF_EPS_FAULT             0x04
#define GL_CF_ADCS_FAULT_ACTIVE     0x08
#define GL_CF_TTC_FAULT_ACTIVE      0x10
#define GL_CF_LOW_BATTERY           0x20

/* TT&C beacon (GL_T_BEACON_TTC), body offsets, 20 bytes total */
#define GL_BT_OFS_UPTIME_S          3    /* u32 */
#define GL_BT_OFS_MODE              7    /* u8 */
#define GL_BT_OFS_STATUS_FLAGS      8    /* u8 ICD_SF_* */
#define GL_BT_OFS_FAULTS            9    /* u16 TTC_FLT_* */
#define GL_BT_OFS_RX_COUNT          11   /* u16 */
#define GL_BT_OFS_TEMP              13   /* i16 0.01 degC */
#define GL_BT_OFS_LAST_RSSI         15   /* i16 dBm */
#define GL_BT_OFS_OBC_CONTACT       17   /* u8 1 = OBC seen in the last 30 s */
#define GL_BT_LEN                   20

/* OBC housekeeping (GL_OBC_GET_HK response), 16 bytes */
#define GL_HK_OFS_UPTIME_S          0    /* u32 */
#define GL_HK_OFS_OBC_MODE          4    /* u8 */
#define GL_HK_OFS_COMM_FLAGS        5    /* u8 GL_CF_* */
#define GL_HK_OFS_VBATT             6    /* u16 mV */
#define GL_HK_OFS_IBUS              8    /* i16 mA */
#define GL_HK_OFS_PBUS              10   /* u16 mW */
#define GL_HK_OFS_CPU_TEMP          12   /* i8 degC */
#define GL_HK_OFS_DISK_FREE         13   /* u8 percent */
#define GL_HK_OFS_LUX               14   /* u16 lux */
#define GL_HK_LEN                   16

/* EPS reading (GL_EPS_GET response), 8 bytes */
#define GL_EPS_OFS_VBATT            0    /* u16 mV */
#define GL_EPS_OFS_IBUS             2    /* i16 mA */
#define GL_EPS_OFS_PBUS             4    /* u16 mW */
#define GL_EPS_OFS_LOW_BATT         6    /* u8 1 = low battery */
#define GL_EPS_OFS_PYRO_FIRES       7    /* u8 pyro fire count since OBC start */
#define GL_EPS_LEN                  8

/* EPS power state (GL_EPS_GET_STATE response), 24 bytes. Added in ICD v0.4.
 * The ACK carries 15 bytes, one TLM part the other 9. */
#define GL_EPSS_OFS_VBATT           0    /* u16 mV, battery bus (VBUS) */
#define GL_EPSS_OFS_ILOAD           2    /* i16 mA, INA226 #1: VBUS -> VBUS_DIST (whole satellite) */
#define GL_EPSS_OFS_IBATT           4    /* i16 mA, INA226 #2: + discharging, - charging; GL_EPSS_I16_NA if not fitted */
#define GL_EPSS_OFS_PSOLAR          6    /* u16 mW delivered by the charger; GL_EPSS_U16_NA without INA226 #2 */
#define GL_EPSS_OFS_SOC             8    /* u8 percent; GL_EPSS_U8_NA = unknown */
#define GL_EPSS_OFS_STATE           9    /* u8 GL_PS_* */
#define GL_EPSS_OFS_FLAGS           10   /* u8 GL_EF_* */
#define GL_EPSS_OFS_SUN             11   /* u8 GL_SUNF_* */
#define GL_EPSS_OFS_VMIN            12   /* u16 mV, lowest since the last GET_STATE */
#define GL_EPSS_OFS_VMAX            14   /* u16 mV, highest since the last GET_STATE */
#define GL_EPSS_OFS_IMAX            16   /* u16 mA, highest load since the last GET_STATE */
#define GL_EPSS_OFS_E_LOAD          18   /* u32 mWh used by the load since OBC start */
#define GL_EPSS_OFS_STATE_AGE       22   /* u16 s in the current power state, saturates at 65535 */
#define GL_EPSS_LEN                 24

#define GL_EPSS_I16_NA              0x7FFF /* field not available */
#define GL_EPSS_U16_NA              0xFFFF
#define GL_EPSS_U8_NA               0xFF

/* Power state (GL_EPSS_OFS_STATE) */
#define GL_PS_UNKNOWN               0    /* no fresh sun data and no battery current */
#define GL_PS_ECLIPSE               1    /* dark: the battery powers everything */
#define GL_PS_SUN_CHARGING          2    /* sunlit, battery charging */
#define GL_PS_SUN_FULL              3    /* sunlit, battery at the full voltage */
#define GL_PS_SUN_DISCHARGING       4    /* sunlit, but the load is more than the panels give */

/* EPS flags (GL_EPSS_OFS_FLAGS) */
#define GL_EF_INA_LOAD_OK           0x01 /* INA226 #1 answering and plausible */
#define GL_EF_INA_BATT_FITTED       0x02 /* INA226 #2 enabled in the OBC build */
#define GL_EF_INA_BATT_OK           0x04 /* INA226 #2 answering */
#define GL_EF_CLIPPED               0x08 /* a shunt reading hit full scale: current under-read */
#define GL_EF_VBUS_IMPLAUSIBLE      0x10 /* bus voltage too low to be running: sensor/wiring fault */
#define GL_EF_LOW_BATT              0x20
#define GL_EF_CRITICAL_BATT         0x40
#define GL_EF_SOC_FROM_VOLTAGE      0x80 /* SOC estimated from voltage (rough); else charge counting */

/* Sun detection (GL_EPSS_OFS_SUN) */
#define GL_SUNF_ADCS_FRESH          0x01 /* ADCS sun sensors read recently */
#define GL_SUNF_TSL_FRESH           0x02 /* payload TSL2591 read recently */
#define GL_SUNF_LIT                 0x04 /* decided: sunlit */

#endif
