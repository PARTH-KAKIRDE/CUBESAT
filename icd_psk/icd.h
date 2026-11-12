/*
 * icd.h - CM4 <-> ADCS / TT&C I2C interface, single source of truth.
 *
 * Included by the CM4 code (cm4/icd_link.py parses the #defines below) and by
 * both STM32 projects. Do not redefine any of these values locally.
 * Any change goes through Parth and bumps ICD_VERSION_*.
 *
 * Rules for this file, so the CM4 parser keeps working:
 *   - one "#define NAME value" per line, value is a decimal or 0x hex integer
 *   - no expressions, no casts, no multi-line macros
 */
#ifndef ICD_H
#define ICD_H

/* ---------------- version (returned by GET_VERSION) ---------------- */
#define ICD_VERSION_MAJOR          0
#define ICD_VERSION_MINOR          1

/* ---------------- bus and addressing (section 1) ---------------- */
#define ICD_I2C_SPEED_HZ           100000
#define ICD_ADDR_ADCS              0x32      /* 7-bit */
#define ICD_ADDR_TTC               0x33      /* 7-bit */

/* ---------------- transaction model (section 2) ---------------- */
#define ICD_RESP_LEN               32        /* every response read is exactly this long */
#define ICD_MAX_PAYLOAD            24
#define ICD_CMD_HDR_LEN            2         /* CMD_ID, LEN */
#define ICD_RESP_HDR_LEN           3         /* CMD_ID, STATUS, LEN */
#define ICD_CRC_LEN                2
#define ICD_CMD_MIN_LEN            4         /* header + CRC, no payload */
#define ICD_CMD_MAX_LEN            28        /* header + 24 payload + CRC */
#define ICD_TURNAROUND_MS          5         /* min wait between command write and response read */
#define ICD_BUSY_RETRIES           3         /* extra reads after a BUSY response */
#define ICD_BUSY_RETRY_MS          5

/* Frame byte offsets */
#define ICD_CMD_OFS_ID             0
#define ICD_CMD_OFS_LEN            1
#define ICD_CMD_OFS_PAYLOAD        2
#define ICD_RESP_OFS_ID            0
#define ICD_RESP_OFS_STATUS        1
#define ICD_RESP_OFS_LEN           2
#define ICD_RESP_OFS_PAYLOAD       3

/* ---------------- CRC (section 3) ---------------- */
/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.
 * Sent low byte first. "123456789" -> 0x29B1. Implementation: icd_crc.c */
#define ICD_CRC_POLY               0x1021
#define ICD_CRC_INIT               0xFFFF
#define ICD_CRC_CHECK              0x29B1

/* ---------------- status codes (section 4) ---------------- */
#define ICD_ST_OK                  0x00
#define ICD_ST_BAD_CRC             0x01
#define ICD_ST_UNKNOWN_CMD         0x02
#define ICD_ST_BAD_LENGTH          0x03
#define ICD_ST_BUSY                0x04
#define ICD_ST_INVALID_MODE        0x05
#define ICD_ST_HW_FAULT            0x06
#define ICD_ST_NO_DATA             0x07

/* ---------------- common commands (section 5) ---------------- */
#define ICD_CMD_NONE               0x00      /* CMD_ID of the frame staged at boot, never sent */
#define ICD_CMD_PING               0x01
#define ICD_CMD_GET_VERSION        0x02
#define ICD_CMD_GET_STATUS         0x03
#define ICD_CMD_GET_TELEMETRY      0x04
#define ICD_CMD_SET_MODE           0x05

/* Response payload lengths */
#define ICD_GET_VERSION_RESP_LEN   5         /* fw major, minor, patch, ICD major, minor */
#define ICD_GET_STATUS_RESP_LEN    8

/* GET_STATUS response payload offsets */
#define ICD_STATUS_OFS_MODE        0         /* u8  */
#define ICD_STATUS_OFS_FLAGS       1         /* u8  status_flags */
#define ICD_STATUS_OFS_FAULTS      2         /* u16 fault_flags */
#define ICD_STATUS_OFS_UPTIME_S    4         /* u32 */

/* status_flags bits */
#define ICD_SF_FAULT_ACTIVE        0x01
#define ICD_SF_GROUND_PKT_PENDING  0x02      /* TT&C only */
#define ICD_SF_NO_MASTER_CONTACT   0x04
#define ICD_SF_RESET_OCCURRED      0x08      /* cleared by the next GET_STATUS read */

/* ---------------- subsystem command ranges (section 6) ---------------- */
#define ICD_CMD_ADCS_FIRST         0x20
#define ICD_CMD_ADCS_LAST          0x3F
#define ICD_CMD_TTC_FIRST          0x40
#define ICD_CMD_TTC_LAST           0x5F

/* ADCS commands 0x20-0x3F: TBD by ADCS owner (start with detumble gain and thresholds) */

/* TT&C commands */
#define ICD_CMD_TTC_GET_RX         0x40      /* resp: one ground packet (<=24 B) or NO_DATA */
#define ICD_CMD_TTC_QUEUE_TX       0x41      /* req: bytes to transmit (<=24 B) */

/* ---------------- modes (section 8) ---------------- */
#define ICD_MODE_BOOT              0
#define ICD_MODE_SAFE              1
#define ICD_MODE_DETUMBLE          2
#define ICD_MODE_NOMINAL           3

/* ---------------- fault_flags bits (section 5), defined per owner ---------------- */
/* ADCS (proposed, ADCS owner to confirm) */
#define ICD_ADCS_FF_MAG_FAIL       0x0001
#define ICD_ADCS_FF_IMU_FAIL       0x0002
#define ICD_ADCS_FF_DRV_A_NFAULT   0x0004    /* DRV8833 #1 nFAULT */
#define ICD_ADCS_FF_DRV_B_NFAULT   0x0008    /* DRV8833 #2 nFAULT */
/* TT&C (proposed, TT&C owner to confirm) */
#define ICD_TTC_FF_RADIO_INIT      0x0001
#define ICD_TTC_FF_TX_TIMEOUT      0x0002

/* ---------------- telemetry layout (section 7), little-endian ---------------- */
/* ADCS page 0: attitude sensors, 18 bytes */
#define ICD_ADCS_TLM0_LEN          18
#define ICD_ADCS_TLM0_UPTIME_MS    0         /* u32, ms since boot (wraps after 49.7 days) */
#define ICD_ADCS_TLM0_MAG_X        4         /* i16, 0.025 uT/LSB (value = B_uT * 40) */
#define ICD_ADCS_TLM0_MAG_Y        6
#define ICD_ADCS_TLM0_MAG_Z        8
#define ICD_ADCS_TLM0_GYRO_X       10        /* i16, raw MPU-6050, +/-250 dps, 131 LSB/dps */
#define ICD_ADCS_TLM0_GYRO_Y       12
#define ICD_ADCS_TLM0_GYRO_Z       14
#define ICD_ADCS_TLM0_MAG_AGE_MS   16        /* u16, ms since last valid mag sample, saturates at 65535 */

/* ADCS page 1: sun sensors and coil currents, 18 bytes */
#define ICD_ADCS_TLM1_LEN          18
#define ICD_ADCS_TLM1_SUN_0        0         /* u16 x6, raw 12-bit ADC (0..4095), offsets 0,2,..,10 */
#define ICD_ADCS_TLM1_MTQ_I_X      12        /* u16, coil current, units/scale TBD by ADCS owner */
#define ICD_ADCS_TLM1_MTQ_I_Y      14
#define ICD_ADCS_TLM1_MTQ_I_Z      16

/* TT&C pages: TBD by TT&C owner (radio state, last RX RSSI, TX count, ...) */

/* ---------------- timing and failure (section 9) ---------------- */
#define ICD_POLL_PERIOD_MS         1000
#define ICD_COMM_FAULT_AFTER       3         /* consecutive failed transactions */
#define ICD_NO_MASTER_TIMEOUT_S    30

#endif /* ICD_H */
