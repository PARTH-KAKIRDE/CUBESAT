/*
 * cubesat_icd.h - Inter-Subsystem ICD v0.4, OBC (Raspberry Pi 4B) <-> ADCS / TT&C.
 *
 * ONE SOURCE OF TRUTH. The Python constants are generated from this file
 * (tools/gen_icd_py.py -> python/icd_defs.py) and both STM32 projects include
 * it. Do not redefine any of these values locally. Any change goes through
 * Parth and bumps ICD_VERSION_MINOR (compatible) or ICD_VERSION_MAJOR
 * (breaking). v0.3 merges the v0.1 draft into v0.2 (see README.md).
 * v0.4 adds the EPS GET_STATE ground command (ground_link.h).
 * v0.8 splits the TT&C radio frequency into downlink and uplink (SET_RADIO /
 * GET_RADIO 9 -> 13 bytes), moves the defaults to 433 MHz (uplink above
 * downlink), makes CR 4/8 the default, and adds Selective Repeat file
 * downlink (ground_link.h GL_VERSION 3).
 * v0.9 moves the TT&C defaults inside India's licence-free 433.05-434.79 MHz
 * band (433.3 down / 433.8 up, 10 dBm), adds TT&C automatic antenna deployment
 * with a flash flag, QUEUE_TX_PART for long (up to 255-byte) FILE packets,
 * DEPLOY_RESET (bench only), a radio-setting confirm/revert timer, and the
 * emergency commands that TT&C executes itself (ground_link.h GL_VERSION 4).
 *
 * Rules for this file (the Python generator depends on them):
 *   - Only "#define NAME VALUE" with integer values (dec or hex, optional u/U).
 *   - No enums, no expressions other than a plain number in parentheses.
 *
 * All multi-byte values on the wire are little-endian.
 * See ICD.md for the prose description of every item.
 */
#ifndef CUBESAT_ICD_H
#define CUBESAT_ICD_H

/* ------------------------------------------------------------------ */
/* Version (returned by GET_VERSION)                                   */
/* ------------------------------------------------------------------ */
#define ICD_VERSION_MAJOR            0
#define ICD_VERSION_MINOR            9

/* ------------------------------------------------------------------ */
/* 1. Bus and addressing                                               */
/* ------------------------------------------------------------------ */
#define ICD_I2C_SPEED_HZ             100000
#define ICD_ADDR_ADCS                0x32
#define ICD_ADDR_TTC                 0x33
/* Other devices the OBC sees (on the separate sensor bus): */
#define ICD_ADDR_TSL2591             0x29
#define ICD_ADDR_INA226_EPS          0x40
/* VEML6075 UV sensor (fixed address, sensor bus). NOT FITTED YET.
 * To use it: delete the leading "// " on the line below, run
 * tools/gen_icd_py.py, bump ICD_VERSION_MINOR, and uncomment its row in
 * ICD.md section 1. Keep it a one-line "//" comment: the generator skips
 * lines that do not start with #define, but it does not understand
 * multi-line comment blocks, so a #define inside one would still be taken. */
// #define ICD_ADDR_VEML6075            0x10
/* Second INA226 (battery current), spliced into the battery cable between
 * the pack and EPS J4 (no PCB change), A0 strapped to VCC. NOT FITTED YET.
 * To use it: delete the leading "// " below, run tools/gen_icd_py.py, and
 * uncomment the "# BATT: " lines in OBC/EPS/EPS.py. Same one-line rule as above. */
// #define ICD_ADDR_INA226_BATT         0x41

/* ------------------------------------------------------------------ */
/* 2./3. Transaction model and frame format                            */
/* ------------------------------------------------------------------ */
#define ICD_MAX_PAYLOAD              24
#define ICD_CMD_HDR_LEN              2    /* CMD_ID, LEN               */
#define ICD_RESP_HDR_LEN             3    /* CMD_ID, STATUS, LEN       */
#define ICD_CRC_LEN                  2
#define ICD_CMD_MIN_LEN              4    /* hdr + crc, no payload     */
#define ICD_CMD_MAX_LEN              28   /* hdr + 24 + crc            */
#define ICD_RESP_FRAME_LEN           32   /* always read exactly 32    */
#define ICD_TURNAROUND_MS            5    /* write -> read minimum gap */
#define ICD_BUSY_RETRIES             3
#define ICD_BUSY_RETRY_MS            5
#define ICD_COMM_FAULT_THRESHOLD     3    /* consecutive failures      */
#define ICD_NO_MASTER_TIMEOUT_MS     30000
#define ICD_CMD_ID_NONE              0x00 /* echo in idle frame        */

/* Frame byte offsets (from v0.1) */
#define ICD_CMD_OFS_ID               0
#define ICD_CMD_OFS_LEN              1
#define ICD_CMD_OFS_PAYLOAD          2
#define ICD_RESP_OFS_ID              0
#define ICD_RESP_OFS_STATUS          1
#define ICD_RESP_OFS_LEN             2
#define ICD_RESP_OFS_PAYLOAD         3

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout */
#define ICD_CRC_POLY                 0x1021
#define ICD_CRC_INIT                 0xFFFF
#define ICD_CRC_CHECK                0x29B1 /* CRC of ASCII "123456789" */

/* ------------------------------------------------------------------ */
/* 4. Status codes                                                     */
/* ------------------------------------------------------------------ */
#define ICD_ST_OK                    0x00
#define ICD_ST_BAD_CRC               0x01
#define ICD_ST_UNKNOWN_CMD           0x02
#define ICD_ST_BAD_LENGTH            0x03
#define ICD_ST_BUSY                  0x04
#define ICD_ST_INVALID_MODE          0x05
#define ICD_ST_HW_FAULT              0x06
#define ICD_ST_NO_DATA               0x07
#define ICD_ST_BAD_PARAM             0x08 /* added v0.2: value out of range */
#define ICD_ST_QUEUE_FULL            0x09 /* added v0.2: TT&C TX queue full */

/* ------------------------------------------------------------------ */
/* 5. Common commands (both slaves)                                    */
/* ------------------------------------------------------------------ */
#define ICD_CMD_PING                 0x01
#define ICD_CMD_GET_VERSION          0x02
#define ICD_CMD_GET_STATUS           0x03
#define ICD_CMD_GET_TELEMETRY        0x04
#define ICD_CMD_SET_MODE             0x05

#define ICD_GET_VERSION_RESP_LEN     5    /* fw maj, min, patch, icd maj, min */
#define ICD_GET_STATUS_RESP_LEN      8
#define ICD_STATUS_OFS_MODE          0    /* u8  */
#define ICD_STATUS_OFS_FLAGS         1    /* u8  */
#define ICD_STATUS_OFS_FAULTS        2    /* u16 */
#define ICD_STATUS_OFS_UPTIME_S      4    /* u32 */

/* status_flags bits */
#define ICD_SF_FAULT_ACTIVE          0x01
#define ICD_SF_GROUND_PKT_PENDING    0x02 /* TT&C only */
#define ICD_SF_NO_MASTER_CONTACT     0x04
#define ICD_SF_RESET_OCCURRED        0x08 /* cleared by the next GET_STATUS */
#define ICD_SF_RESERVED_MASK         0xF0 /* bits 4-7 reserved, sent as 0 (from v0.1) */
/* One-shot flags and sticky faults are cleared when the slave PROCESSES a
 * GET_STATUS, not when the Pi receives the reply. If that reply is lost, the
 * Pi recovers it with a plain re-read before its next command (the slave holds
 * the response until a new command arrives), and also treats uptime_s going
 * backwards as a reset. See ICD.md section 5. */

/* ------------------------------------------------------------------ */
/* 8. Modes                                                            */
/* ------------------------------------------------------------------ */
#define ICD_MODE_BOOT                0
#define ICD_MODE_SAFE                1
#define ICD_MODE_DETUMBLE            2
#define ICD_MODE_NOMINAL             3
#define ICD_MODE_COUNT               4

/* Why the slave is in its current mode (both slaves) */
#define ICD_MR_BOOT                  0
#define ICD_MR_COMMAND               1
#define ICD_MR_FAULT                 2
#define ICD_MR_AUTO                  3

/* ------------------------------------------------------------------ */
/* 6. Subsystem command ranges (from v0.1)                             */
/* ------------------------------------------------------------------ */
#define ICD_CMD_ADCS_FIRST           0x20
#define ICD_CMD_ADCS_LAST            0x3F
#define ICD_CMD_TTC_FIRST            0x40
#define ICD_CMD_TTC_LAST             0x5F

/* ------------------------------------------------------------------ */
/* 9. Polling (from v0.1)                                              */
/* ------------------------------------------------------------------ */
#define ICD_POLL_PERIOD_MS           1000 /* GET_STATUS + fast telemetry */
#define ICD_SLOW_POLL_MS             10000 /* slow telemetry pages      */

/* ------------------------------------------------------------------ */
/* 6./7. ADCS (0x20 - 0x3F)                                            */
/* ------------------------------------------------------------------ */
#define ADCS_CMD_SET_BDOT_GAIN       0x20 /* req: gain u32                        */
#define ADCS_CMD_SET_DETUMBLE_THRESH 0x21 /* req: exit u16, enter u16, dwell_s u16 */
#define ADCS_CMD_SET_BDOT_TIMING     0x22 /* req: period_ms u16, settle_ms u16     */
#define ADCS_CMD_GET_CONFIG          0x23 /* resp: see ADCS_CFG_OFS_*              */
#define ADCS_CMD_MTQ_MANUAL          0x24 /* req: duty x,y,z i16, duration_ms u16  */
#define ADCS_CMD_SET_MAG_OFFSET      0x25 /* req: x,y,z i16 (mag LSB)              */
#define ADCS_CMD_SET_GYRO_OFFSET     0x26 /* req: x,y,z i16 (gyro LSB), v0.5       */
#define ADCS_CMD_SET_SUN_CONFIG      0x27 /* req: lit u16, full u16 (ADC counts), v0.6 */
#define ADCS_CMD_SET_SUN_DISABLED    0x28 /* req: face mask u8 to ignore, v0.7      */

#define ADCS_SET_BDOT_GAIN_LEN       4
#define ADCS_SET_DETUMBLE_THRESH_LEN 6
#define ADCS_SET_BDOT_TIMING_LEN     4
#define ADCS_MTQ_MANUAL_LEN          8
#define ADCS_SET_MAG_OFFSET_LEN      6
#define ADCS_SET_GYRO_OFFSET_LEN     6
#define ADCS_SET_SUN_CONFIG_LEN      4
#define ADCS_SET_SUN_DISABLED_LEN    1

/* B-dot gain unit: duty[permille] = -(gain/1000) * dB/dt[uT/s]          */
#define ADCS_BDOT_GAIN_MAX           10000000
#define ADCS_BDOT_GAIN_DEFAULT       300000
/* Rate thresholds in 0.01 deg/s                                       */
#define ADCS_EXIT_RATE_DEFAULT       200  /* 2.00 dps: detumbled below this    */
#define ADCS_ENTER_RATE_DEFAULT      500  /* 5.00 dps: re-enter detumble above */
#define ADCS_DWELL_S_DEFAULT         60
#define ADCS_PERIOD_MS_DEFAULT       1000 /* B-dot control period            */
#define ADCS_SETTLE_MS_DEFAULT       20   /* coils off before mag sample     */
#define ADCS_PERIOD_MS_MIN           100
#define ADCS_PERIOD_MS_MAX           10000
#define ADCS_SETTLE_MS_MIN           5
#define ADCS_DUTY_MAX                1000 /* permille */
#define ADCS_MANUAL_MAX_MS           10000
/* Gyro zero-rate offset (v0.5). ADCS measures it for ADCS_GYRO_CAL_MS after
 * every boot and keeps it only if the board was still: every gyro axis and
 * every magnetometer axis moved less than the spread limits below.        */
#define ADCS_GYRO_OFS_MAX            1310 /* |offset| LSB = 20 dps (MPU-6050 ZRO limit) */
#define ADCS_GYRO_CAL_MS             5000
#define ADCS_GYRO_CAL_MAX_SPREAD     33   /* gyro LSB, max - min per axis (0.5 dps)  */
#define ADCS_GYRO_CAL_MAX_MAG_SPREAD 20   /* mag LSB, max - min per axis (0.5 uT)    */
#define ADCS_GYRO_CAL_MIN_SAMPLES    40   /* of the 50 gyro reads in the window    */
/* Sun sensors (v0.6, v0.7): BPW34 photodiodes, one on every face (v0.7;
 * v0.6 had none on nadir).
 * full = ADC counts with the Sun straight onto a diode (cos 0 = 1).
 * lit  = the albedo threshold: a face at or above it counts as sunlit, so
 *        set it above the Earth-albedo glow. Below ADCS_SUN_NOISE a face
 *        counts as dark.                                                */
#define ADCS_SUN_FULL_DEFAULT        3000 /* TBD: measure in sunlight          */
#define ADCS_SUN_LIT_DEFAULT         1200 /* TBD: 0.4 x full, above albedo     */
#define ADCS_SUN_FULL_MIN            100
#define ADCS_SUN_NOISE               60   /* counts                           */
#define ADCS_SUN_UNIT                10000 /* sun vector: 10000 = 1.0          */
/* Dead diodes and gyro tracking (v0.7). With all diodes working, the
 * per-axis readings (brighter side / full) form a vector of length 1. If
 * it is shorter than ADCS_SUN_DEFICIT_PERMILLE (squared, permille) while
 * the Sun is up, light is missing: a dark face whose axis can hold the
 * missing part, on the side the gyro-tracked Sun favours, is suspected.
 * Suspected (dark while the tracked Sun puts >= ADCS_SUN_DEAD_PERMILLE x
 * full on it) for ADCS_SUN_DEAD_CYCLES cycles: dead. The tracked direction
 * is trusted for that within ADCS_SUN_TRUST_MS of a measurement, and for
 * HIDDEN for ADCS_SUN_TRACK_MAX_MS (gyro drift). Needs `full` within ~10%. */
#define ADCS_SUN_DEFICIT_PERMILLE    750
#define ADCS_SUN_DEAD_PERMILLE       300
#define ADCS_SUN_DEAD_CYCLES         5
#define ADCS_SUN_TRUST_MS            60000
#define ADCS_SUN_TRACK_MAX_MS        300000

/* GET_CONFIG response layout (20 bytes) */
#define ADCS_CFG_OFS_GAIN            0    /* u32 */
#define ADCS_CFG_OFS_EXIT_RATE       4    /* u16 0.01 dps */
#define ADCS_CFG_OFS_ENTER_RATE      6    /* u16 0.01 dps */
#define ADCS_CFG_OFS_DWELL_S         8    /* u16 s */
#define ADCS_CFG_OFS_PERIOD_MS       10   /* u16 ms */
#define ADCS_CFG_OFS_SETTLE_MS       12   /* u16 ms */
#define ADCS_CFG_OFS_MAG_OFS_X       14   /* i16 mag LSB */
#define ADCS_CFG_OFS_MAG_OFS_Y       16
#define ADCS_CFG_OFS_MAG_OFS_Z       18
#define ADCS_CFG_LEN                 20

/* ADCS fault_flags bits */
#define ADCS_FLT_MAG                 0x0001 /* MMC5983MA no response / bad id */
#define ADCS_FLT_IMU                 0x0002 /* MPU-6050 no response / bad id  */
#define ADCS_FLT_DRVX                0x0004 /* DRV8833 X nFAULT low           */
#define ADCS_FLT_DRVY                0x0008
#define ADCS_FLT_DRVZ                0x0010
#define ADCS_FLT_LOCAL_I2C           0x0020 /* sensor bus (I2C1) error        */
#define ADCS_FLT_OBC_I2C             0x0040 /* OBC link peripheral was reset  */
#define ADCS_FLT_SUN_ADC             0x0080 /* ADC conversion timeout         */
#define ADCS_FLT_SUN_DIODE           0x0100 /* a sun diode reads dark while the gyro-tracked Sun is on it (v0.7) */

/* Scales */
#define ADCS_MAG_NT_PER_LSB          25   /* 0.025 uT / LSB  (+-819 uT) */
#define ADCS_GYRO_FS_DPS             500  /* MPU-6050 range +-500 dps   */
#define ADCS_GYRO_LSB_PER_DPS_X10    655  /* 65.5 LSB per dps           */
#define ADCS_MAG_AGE_INVALID         0xFFFF

/* Telemetry page 0: attitude sensors (18 bytes) */
#define ADCS_TLM_PAGE_SENSORS        0
#define ADCS_TLM0_OFS_UPTIME_MS      0    /* u32 ms since boot, wraps after 49.7 days */
#define ADCS_TLM0_OFS_MAG_X          4    /* i16 x 0.025 uT */
#define ADCS_TLM0_OFS_MAG_Y          6
#define ADCS_TLM0_OFS_MAG_Z          8
#define ADCS_TLM0_OFS_GYRO_X         10   /* i16 65.5 LSB/dps, offset removed (v0.5) */
#define ADCS_TLM0_OFS_GYRO_Y         12
#define ADCS_TLM0_OFS_GYRO_Z         14
#define ADCS_TLM0_OFS_MAG_AGE_MS     16   /* u16, 0xFFFF = never valid */
#define ADCS_TLM0_LEN                18

/* Telemetry page 1: sun sensors + coil drive (18 bytes)
 * NOTE: each DRV8833 AISEN goes to ground through a 1 ohm sense resistor
 * (driver current limit only); it is not connected to the STM32, so coil
 * current cannot be measured. The commanded duty is reported instead.  */
#define ADCS_TLM_PAGE_SUN_MTQ        1
#define ADCS_TLM1_OFS_SUN1           0    /* u16 raw 12-bit ADC, SS1..SS6 */
#define ADCS_TLM1_OFS_SUN2           2
#define ADCS_TLM1_OFS_SUN3           4
#define ADCS_TLM1_OFS_SUN4           6
#define ADCS_TLM1_OFS_SUN5           8
#define ADCS_TLM1_OFS_SUN6           10
#define ADCS_TLM1_OFS_DUTY_X         12   /* i16 permille, + = AOUT1 high */
#define ADCS_TLM1_OFS_DUTY_Y         14
#define ADCS_TLM1_OFS_DUTY_Z         16
#define ADCS_TLM1_LEN                18

/* Telemetry page 2: controller state (16 bytes) */
#define ADCS_TLM_PAGE_CTRL           2
#define ADCS_TLM2_OFS_DBDT_X         0    /* i16 0.01 uT/s */
#define ADCS_TLM2_OFS_DBDT_Y         2
#define ADCS_TLM2_OFS_DBDT_Z         4
#define ADCS_TLM2_OFS_RATE_NORM      6    /* u16 0.01 dps */
#define ADCS_TLM2_OFS_IMU_TEMP       8    /* i16 0.01 degC */
#define ADCS_TLM2_OFS_CTRL_FLAGS     10   /* u8 ADCS_CF_* */
#define ADCS_TLM2_OFS_MODE_REASON    11   /* u8 ICD_MR_* */
#define ADCS_TLM2_OFS_OBC_I2C_RESETS 12   /* u16 */
#define ADCS_TLM2_OFS_LOCAL_I2C_ERR  14   /* u16 */
#define ADCS_TLM2_LEN                16

#define ADCS_CF_DETUMBLED            0x01 /* rate below exit threshold for dwell */
#define ADCS_CF_COILS_ACTIVE         0x02
#define ADCS_CF_MANUAL_ACTIVE        0x04
#define ADCS_CF_MAG_VALID            0x08

/* Telemetry page 3: gyro offset calibration (12 bytes, v0.5) */
#define ADCS_TLM_PAGE_GYRO_CAL       3
#define ADCS_TLM3_OFS_GYRO_OFS_X     0    /* i16 LSB, subtracted from every gyro read */
#define ADCS_TLM3_OFS_GYRO_OFS_Y     2
#define ADCS_TLM3_OFS_GYRO_OFS_Z     4
#define ADCS_TLM3_OFS_CAL_STATE      6    /* u8 ADCS_GCAL_*                 */
#define ADCS_TLM3_OFS_CAL_SAMPLES    7    /* u8 gyro reads averaged         */
#define ADCS_TLM3_OFS_GYRO_SPREAD    8    /* u16 gyro LSB, largest max - min */
#define ADCS_TLM3_OFS_MAG_SPREAD     10   /* mag LSB, largest max - min     */
#define ADCS_TLM3_LEN                12

#define ADCS_GCAL_NONE               0    /* no offset: IMU missing at boot           */
#define ADCS_GCAL_RUNNING            1    /* boot window still open                  */
#define ADCS_GCAL_AUTO               2    /* measured at boot, board was still       */
#define ADCS_GCAL_REJECTED           3    /* board moved (or no mag): offset left 0  */
#define ADCS_GCAL_COMMANDED          4    /* set with SET_GYRO_OFFSET                */

/* Telemetry page 4: sun direction and sunlit/eclipse (16 bytes, v0.6) */
#define ADCS_TLM_PAGE_SUN            4
#define ADCS_TLM4_OFS_SUN_X          0    /* i16 unit vector x ADCS_SUN_UNIT, body frame */
#define ADCS_TLM4_OFS_SUN_Y          2
#define ADCS_TLM4_OFS_SUN_Z          4
#define ADCS_TLM4_OFS_SUN_FLAGS      6    /* u8 ADCS_SUNF_*                    */
#define ADCS_TLM4_OFS_LIT_FACES      7    /* u8 ADCS_FACE_* at or above lit    */
#define ADCS_TLM4_OFS_FITTED_FACES   8    /* u8 ADCS_FACE_* with a diode       */
#define ADCS_TLM4_OFS_DEAD_FACES     9    /* u8 ADCS_FACE_* ignored: dead or disabled (v0.7; v0.6: 0) */
#define ADCS_TLM4_OFS_SUN_MAX        10   /* u16 counts, brightest fitted face */
#define ADCS_TLM4_OFS_LIT_THRESHOLD  12   /* in use (SET_SUN_CONFIG)           */
#define ADCS_TLM4_OFS_FULL_SCALE     14
#define ADCS_TLM4_LEN                16

/* Face bits (body frame) */
#define ADCS_FACE_PX                 0x01
#define ADCS_FACE_MX                 0x02
#define ADCS_FACE_PY                 0x04
#define ADCS_FACE_MY                 0x08
#define ADCS_FACE_PZ                 0x10
#define ADCS_FACE_MZ                 0x20

#define ADCS_SUNF_LIT                0x01 /* some fitted face >= lit: not eclipse   */
#define ADCS_SUNF_VECTOR_VALID       0x02 /* sun vector computed this cycle         */
#define ADCS_SUNF_BLIND_SIDE         0x04 /* sun on a side with no working diode: */
                                          /* that component from |s| = 1         */
#define ADCS_SUNF_HIDDEN             0x08 /* v0.7: all dark, but the tracked Sun is */
                                          /* behind a dead/missing diode: unknown, */
                                          /* NOT eclipse                          */
#define ADCS_SUNF_PREDICTED          0x10 /* v0.7: the vector is the gyro-tracked one */

/* ------------------------------------------------------------------ */
/* 6./7. TT&C (0x40 - 0x5F)                                            */
/* ------------------------------------------------------------------ */
#define TTC_CMD_GET_RX               0x40 /* resp: one ground packet or NO_DATA */
#define TTC_CMD_QUEUE_TX             0x41 /* req: 1..24 bytes to transmit      */
#define TTC_CMD_SET_BEACON           0x42 /* req: 0..24 bytes beacon content   */
#define TTC_CMD_SET_BEACON_PERIOD    0x43 /* req: period_s u16 (0 = off)       */
#define TTC_CMD_SET_RADIO            0x44 /* req: see TTC_RADIO_OFS_*          */
#define TTC_CMD_GET_RADIO            0x45 /* resp: see TTC_RADIO_OFS_*         */
#define TTC_CMD_BURN_ARM             0x46 /* req: key u32                      */
#define TTC_CMD_BURN_FIRE            0x47 /* req: duration_ms u16              */
#define TTC_CMD_QUEUE_TX_PART        0x48 /* v0.9 req: TTC_TXP_* (long packets) */
#define TTC_CMD_DEPLOY_RESET         0x49 /* v0.9 req: key u32, RBF must be IN  */

#define TTC_RX_QUEUE_DEPTH           8
#define TTC_TX_QUEUE_DEPTH           8
#define TTC_BEACON_PERIOD_DEFAULT_S  30
#define TTC_BEACON_PERIOD_SAFE_MIN_S 60
#define TTC_BEACON_PERIOD_MIN_S      10
#define TTC_BURN_ARM_KEY             0x4E525542 /* "BURN" little-endian */
#define TTC_BURN_ARM_WINDOW_MS       10000
#define TTC_BURN_MAX_MS              8000

/* v0.9 QUEUE_TX_PART: builds one packet of up to TTC_TX_MAX_LEN bytes from
 * several I2C transactions. OFFSET 0 starts a new packet (drops any half-built
 * one); every other part must start exactly where the previous one ended. The
 * part with FLAG_LAST queues the packet (QUEUE_FULL drops it: start again).
 * Refused in SAFE (only bulk data needs it). */
#define TTC_TX_MAX_LEN               255  /* LoRa maximum payload */
#define TTC_TXP_OFS_FLAGS            0    /* u8 TTC_TXP_F_* */
#define TTC_TXP_OFS_OFFSET           1    /* u8 byte offset of this part */
#define TTC_TXP_OFS_DATA             2
#define TTC_TXP_MAX_DATA             22   /* 24 - 2 */
#define TTC_TXP_F_LAST               0x01

/* v0.9 automatic antenna deployment (TT&C boot procedure, ICD.md section 8).
 * Runs only while the flash flag says "not deployed" and fewer than
 * MAX_ATTEMPTS burns were ever made: the RBF plug must be out for DELAY_S
 * without a break, then BURN_MS burns (stopping early on the deploy switch),
 * RETRY_S apart. The switch reading "deployed" for 2 s sets the flag, and
 * TT&C never burns on its own again. DEPLOY_RESET (key = TTC_DEPLOY_RESET_KEY)
 * clears the flag and the count, and only with the RBF plug IN (bench). */
#define TTC_AUTODEPLOY_DELAY_S       60
#define TTC_AUTODEPLOY_BURN_MS       5000
#define TTC_AUTODEPLOY_RETRY_S       60
#define TTC_AUTODEPLOY_MAX_ATTEMPTS  3    /* in the satellite's whole life */
#define TTC_DEPLOYED_CONFIRM_MS      2000
#define TTC_DEPLOY_DEBOUNCE_MS       50   /* switch must read deployed this long to stop a burn */
#define TTC_DEPLOY_RESET_KEY         0x52414552 /* "REAR" little-endian */

/* v0.9 radio settings confirm/revert: settings from SET_RADIO are kept only
 * if a valid ground packet arrives on them within CONFIRM_MS; otherwise TT&C
 * goes back to the last settings that were confirmed (or the defaults). */
#define TTC_RADIO_CONFIRM_MS         600000

/* v0.9 emergency priority. ECMD ACKs and EVENT reports wait in their own
 * queue, sent before beacons and OBC packets. After every packet TT&C stays
 * in receive for at least LISTEN_GAP_MS before the next non-priority one, and
 * holds any transmission while the SX1278 is receiving (at most RX_HOLD_MAX_MS),
 * so the ground can get an emergency command in even during a file burst. */
#define TTC_PRIO_DEPTH               4
#define TTC_LISTEN_GAP_MS            150
#define TTC_RX_HOLD_MAX_MS           1000

/* Radio config layout (SET_RADIO request / GET_RADIO response, 13 bytes).
 * v0.8: two frequencies. TT&C receives on FREQ_UP and retunes to FREQ_DOWN
 * for every transmission (the SX1278 is half duplex, so it is never on both).
 * FREQ_UP must be higher than FREQ_DOWN. */
#define TTC_RADIO_OFS_FREQ_DOWN_HZ   0    /* u32 satellite TX / ground RX */
#define TTC_RADIO_OFS_SF             4    /* u8 7..12 */
#define TTC_RADIO_OFS_BW             5    /* u8 TTC_BW_* */
#define TTC_RADIO_OFS_CR             6    /* u8 5..8 means 4/5..4/8 */
#define TTC_RADIO_OFS_POWER_DBM      7    /* i8 2..17 */
#define TTC_RADIO_OFS_SYNC           8    /* u8 sync word */
#define TTC_RADIO_OFS_FREQ_UP_HZ     9    /* u32 satellite RX / ground TX, v0.8 */
#define TTC_RADIO_LEN                13

#define TTC_BW_62K5                  0
#define TTC_BW_125K                  1
#define TTC_BW_250K                  2
#define TTC_BW_500K                  3

#define TTC_FREQ_MIN_HZ              433050000 /* v0.9: bottom of the 433.05-434.79 MHz band */
#define TTC_FREQ_MAX_HZ              438000000
/* v0.9: both inside 433.05-434.04 MHz (the part of India's licence-free band
 * that allows 125 kHz), 500 kHz apart, >= 180 kHz from either edge. */
#define TTC_FREQ_DOWN_DEFAULT_HZ     433300000 /* downlink (sat TX) */
#define TTC_FREQ_UP_DEFAULT_HZ       433800000 /* uplink (sat RX), above the downlink */
#define TTC_SF_DEFAULT               9
#define TTC_BW_DEFAULT               1
#define TTC_CR_DEFAULT               8    /* v0.8: 4/8, strongest FEC */
#define TTC_POWER_DEFAULT_DBM        10   /* v0.9: 10 mW, the licence-free limit (ERP) */
#define TTC_SYNC_DEFAULT             0x12

/* TT&C fault_flags bits */
#define TTC_FLT_RADIO_INIT           0x0001 /* SX1278 version reg wrong / no SPI */
#define TTC_FLT_RADIO_TX_TIMEOUT     0x0002
#define TTC_FLT_TEMP_SENSOR          0x0004 /* TMP102 no response */
#define TTC_FLT_RX_OVERFLOW          0x0008 /* ground packet dropped, queue full */
#define TTC_FLT_TX_OVERFLOW          0x0010
#define TTC_FLT_OBC_I2C              0x0020 /* OBC link peripheral was reset */
#define TTC_FLT_LOCAL_I2C            0x0040
#define TTC_FLT_RADIO_REVERTED       0x0080 /* v0.9: unconfirmed SET_RADIO undone */
#define TTC_FLT_NV                   0x0100 /* v0.9: flash flag store write failed */

/* Radio state */
#define TTC_RS_OFF                   0
#define TTC_RS_RX                    1
#define TTC_RS_TX                    2

/* Deploy flags (TLM0) */
#define TTC_DF_DEPLOY_SW             0x01 /* raw pin level, 1 = high */
#define TTC_DF_RBF_IN                0x02 /* raw pin level, 1 = high (RBF removed) */
#define TTC_DF_ARMED                 0x04
#define TTC_DF_BURNING               0x08
#define TTC_DF_BURN_DONE             0x10
#define TTC_DF_DEPLOYED_FLAG         0x20 /* v0.9: flash flag set, no more auto burns */
#define TTC_DF_AUTO_ACTIVE           0x40 /* v0.9: automatic deployment still to run */

/* Telemetry page 0: radio housekeeping (24 bytes) */
#define TTC_TLM_PAGE_RADIO           0
#define TTC_TLM0_OFS_UPTIME_MS       0    /* u32 ms since boot, wraps after 49.7 days */
#define TTC_TLM0_OFS_RADIO_STATE     4    /* u8 TTC_RS_* */
#define TTC_TLM0_OFS_RX_QUEUE        5    /* u8 packets waiting for OBC */
#define TTC_TLM0_OFS_TX_QUEUE        6    /* u8 */
#define TTC_TLM0_OFS_DEPLOY_FLAGS    7    /* u8 TTC_DF_* */
#define TTC_TLM0_OFS_LAST_RSSI       8    /* i16 dBm */
#define TTC_TLM0_OFS_LAST_SNR        10   /* i8 0.25 dB */
#define TTC_TLM0_OFS_MODE_REASON     11   /* u8 ICD_MR_* */
#define TTC_TLM0_OFS_RX_COUNT        12   /* u16 */
#define TTC_TLM0_OFS_TX_COUNT        14   /* u16 */
#define TTC_TLM0_OFS_RX_CRC_ERR      16   /* u16 */
#define TTC_TLM0_OFS_BOARD_TEMP      18   /* i16 0.01 degC */
#define TTC_TLM0_OFS_BEACON_PERIOD   20   /* u16 s */
#define TTC_TLM0_OFS_OBC_I2C_RESETS  22   /* u16 */
#define TTC_TLM0_LEN                 24

/* Telemetry page 1: counters (12 bytes) */
#define TTC_TLM_PAGE_COUNTERS        1
#define TTC_TLM1_OFS_TX_TIMEOUTS     0    /* u16 */
#define TTC_TLM1_OFS_RX_DROPPED      2    /* u16 */
#define TTC_TLM1_OFS_TX_DROPPED      4    /* u16 */
#define TTC_TLM1_OFS_BEACON_COUNT    6    /* u16 */
#define TTC_TLM1_OFS_BURN_COUNT      8    /* u8 */
#define TTC_TLM1_OFS_AUTO_ATTEMPTS   9    /* u8 v0.9: automatic burns ever made */
#define TTC_TLM1_OFS_LAST_BURN_MS    10   /* u16 */
#define TTC_TLM1_LEN                 12

/* ------------------------------------------------------------------ */
/* Dummy test slave (Nucleo-F446RE), used by the link tests only        */
/* ------------------------------------------------------------------ */
#define DUMMY_FW_MAJOR               0
#define DUMMY_FW_MINOR               1
#define DUMMY_FW_PATCH               0
/* Telemetry page 0: link counters (24 bytes) */
#define DUMMY_TLM0_OFS_UPTIME_MS     0    /* u32 */
#define DUMMY_TLM0_OFS_FRAMES_OK     4    /* u32 */
#define DUMMY_TLM0_OFS_CRC_ERRORS    8    /* u32 */
#define DUMMY_TLM0_OFS_LEN_ERRORS    12   /* u32 */
#define DUMMY_TLM0_OFS_OVERWRITTEN   16   /* u32 */
#define DUMMY_TLM0_OFS_BUS_RESETS    20   /* u16 */
#define DUMMY_TLM0_OFS_READS_LO      22   /* u16, low 16 bits of read count */
#define DUMMY_TLM0_LEN               24

#endif /* CUBESAT_ICD_H */
