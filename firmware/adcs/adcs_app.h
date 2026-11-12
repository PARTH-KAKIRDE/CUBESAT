/* ADCS application: modes, B-dot cycle, faults, ICD commands and telemetry.
 * Portable: talks to hardware only through adcs_hw.h. */
#ifndef ADCS_APP_H
#define ADCS_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "icd_node.h"

#define ADCS_FW_MAJOR 0
#define ADCS_FW_MINOR 2
#define ADCS_FW_PATCH 0

#define ADCS_GYRO_PERIOD_MS      100u
#define ADCS_MAG_TIMEOUT_MS      50u
#define ADCS_MAG_FIRST_POLL_MS   9u
#define ADCS_FAIL_LIMIT          3u     /* consecutive failures -> fault */
#define ADCS_REINIT_PERIOD_MS    5000u
#define ADCS_DRV_FAULT_CYCLES    3u     /* control cycles with nFAULT -> SAFE */
#define ADCS_AUTO_RECOVER_MS     30000u /* fault-SAFE, faults clear, no OBC -> DETUMBLE */

typedef enum { PH_SETTLE = 0, PH_MEASURE, PH_ACTUATE } adcs_phase_t;

typedef struct {
    uint32_t gain;
    uint16_t exit_rate, enter_rate, dwell_s;
    uint16_t period_ms, settle_ms;
    int16_t mag_ofs[3];
} adcs_cfg_t;

typedef struct {
    icd_node_t node;
    adcs_cfg_t cfg;

    /* sensors */
    int16_t mag[3], mag_prev[3];
    uint32_t mag_time, mag_prev_time;
    bool mag_valid, have_prev;
    int16_t gyro[3];
    int16_t imu_temp;
    uint32_t gyro_time;
    uint16_t sun[6];

    /* control */
    adcs_phase_t phase;
    uint32_t cycle_start, meas_start, last_mag_poll;
    int16_t duty[3];          /* what is on the coils now */
    int16_t ctrl_duty[3];     /* last computed / commanded duty */
    int16_t dbdt[3];          /* 0.01 uT/s */
    uint16_t rate_norm;       /* 0.01 dps */
    bool detumbled;
    uint32_t below_since;
    bool below;

    /* manual coil test (NOMINAL only) */
    bool manual;
    int16_t manual_duty[3];
    uint32_t manual_until;

    /* health */
    bool mag_ok, imu_ok, sun_ok;
    uint8_t mag_fails, imu_fails, drv_fault_cycles;
    uint8_t drv_faults;
    uint32_t mag_retry_at, imu_retry_at;
    uint16_t local_i2c_seen;
    uint32_t obc_resets_seen;
    uint32_t faults_clear_since;
} adcs_t;

void adcs_init(adcs_t *a, uint32_t now_ms);
void adcs_step(adcs_t *a, uint32_t now_ms);
void adcs_default_config(adcs_cfg_t *c);

#endif
