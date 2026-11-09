/*
 * ground_link.h - packet format on the LoRa link (satellite <-> ground station).
 *
 * Shared by TT&C firmware (uplink filter + its own beacon), the OBC (command
 * decoding, ACKs, OBC beacon) and the ground station tool. Same generator
 * rules as cubesat_icd.h: plain integer #defines only.
 *
 * Every packet: SAT_ID(1) | TYPE(1) | SEQ(1) | body ... | CRC16(2)
 *   - max GL_MAX_LEN bytes in total (the ICD TTC_QUEUE_TX / GET_RX limit)
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
 */
#ifndef GROUND_LINK_H
#define GROUND_LINK_H

#define GL_VERSION                  2    /* 2: authenticated CMD packets */
#define GL_SAT_ID                   0xC5
#define GL_MAX_LEN                  24
#define GL_HDR_LEN                  3
#define GL_CRC_LEN                  2

#define GL_OFS_SAT                  0
#define GL_OFS_TYPE                 1
#define GL_OFS_SEQ                  2

/* Packet types */
#define GL_T_CMD                    0x01 /* ground -> sat */
#define GL_T_ACK                    0x02 /* sat -> ground, answer to a CMD */
#define GL_T_TLM                    0x03 /* sat -> ground, extra data for a CMD */
#define GL_T_BEACON_OBC             0x10 /* OBC housekeeping, sent by TT&C */
#define GL_T_BEACON_TTC             0x11 /* TT&C's own beacon when OBC is silent */

/* Targets */
#define GL_TGT_OBC                  0
#define GL_TGT_ADCS                 1    /* forwarded as an ICD command (allowlist in ICD.md) */
#define GL_TGT_TTC                  2    /* forwarded as an ICD command (allowlist in ICD.md) */
#define GL_TGT_EPS                  3    /* handled by the OBC EPS driver   */

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
#define GL_OBC_CAPTURE_IMAGE        0x10 /* camera snapshot saved on the OBC */
#define GL_OBC_GET_SLAVE_TLM        0x11 /* args: target u8, page u8 */
#define GL_OBC_REBOOT               0x1F /* args: key u32 = GL_REBOOT_KEY */
#define GL_REBOOT_KEY               0x544F4F42 /* "BOOT" little-endian */

/* EPS commands (target GL_TGT_EPS) */
#define GL_EPS_GET                  0x01 /* response: GL_EPS_OFS_* layout */
#define GL_EPS_PYRO_ARM             0x20 /* args: key u32 = GL_PYRO_KEY */
#define GL_EPS_PYRO_FIRE            0x21 /* args: duration_ms u16 */
#define GL_PYRO_KEY                 0x4F525950 /* "PYRO" little-endian */

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

#endif
