/* ADCS application: modes, B-dot cycle, faults, ICD commands and telemetry.
 * Portable: talks to hardware only through adcs_hw.h. */
#ifndef ADCS_APP_H
#define ADCS_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "icd_node.h"

#define ADCS_FW_MAJOR 0
#define ADCS_FW_MINOR 5
#define ADCS_FW_PATCH 0

#define ADCS_GYRO_PERIOD_MS      100u
#define ADCS_MAG_TIMEOUT_MS      50u
#define ADCS_MAG_FIRST_POLL_MS   9u
#define ADCS_FAIL_LIMIT          3u     /* consecutive failures -> fault */
#define ADCS_REINIT_PERIOD_MS    5000u
#define ADCS_DRV_FAULT_CYCLES    3u     /* control cycles with nFAULT -> SAFE */
#define ADCS_AUTO_RECOVER_MS     30000u /* fault-SAFE, faults clear, no OBC -> DETUMBLE */
/* If the fault comes back within ADCS_RECOVER_STABLE_MS of an automatic
 * recovery (e.g. a shorted coil driver), the wait before the next attempt
 * doubles, up to ADCS_RECOVER_MAX_MS. A stable stretch resets it to 30 s. */
#define ADCS_RECOVER_STABLE_MS   600000u  /* 10 min */
#define ADCS_RECOVER_MAX_MS      1920000u /* 32 min = 30 s x 64 */
/* Boot gyro calibration: magnetometer samples needed to prove stillness. */
#define ADCS_GYRO_CAL_MIN_MAG    3u

typedef enum { PH_SETTLE = 0, PH_MEASURE, PH_ACTUATE } adcs_phase_t;

typedef struct {
    uint32_t gain;
    uint16_t exit_rate, enter_rate, dwell_s;
    uint16_t period_ms, settle_ms;
    int16_t mag_ofs[3];
    uint16_t sun_lit, sun_full;   /* ADC counts (SET_SUN_CONFIG) */
} adcs_cfg_t;

typedef struct {
    icd_node_t node;
    adcs_cfg_t cfg;

    /* sensors */
    int16_t mag[3], mag_prev[3];
    uint32_t mag_time, mag_prev_time;
    bool mag_valid, have_prev;
    int16_t gyro[3];          /* offset removed (what everything uses) */
    int16_t gyro_raw[3];      /* as read from the MPU-6050 */
    int16_t imu_temp;
    uint32_t gyro_time;
    uint16_t sun[6];
    int16_t sun_vec[3];       /* unit vector x ADCS_SUN_UNIT, body frame */
    uint8_t sun_flags;        /* ADCS_SUNF_* */
    uint8_t sun_lit_faces;    /* ADCS_FACE_* */
    uint16_t sun_max;
    /* gyro tracking and diode health (ICD v0.7) */
    float sun_est[3];         /* tracked Sun direction, unit, body frame */
    bool sun_est_ok;
    uint32_t sun_fix_ms;      /* time of the last measurement it came from */
    uint32_t sun_track_ms;    /* last gyro propagation */
    uint8_t sun_dead;         /* ADCS_FACE_*: detected dead */
    uint8_t sun_disabled;     /* ADCS_FACE_*: switched off by SET_SUN_DISABLED */
    uint8_t sun_suspect[6];   /* cycles each face read dark while it should be lit */

    /* gyro zero-rate offset (ICD v0.5, telemetry page 3) */
    int16_t gyro_ofs[3];
    uint8_t gcal_state;       /* ADCS_GCAL_* */
    uint32_t gcal_start;
    uint8_t gcal_n, gcal_mag_n;
    int32_t gcal_sum[3];
    int16_t gcal_min[3], gcal_max[3];
    int16_t gcal_mag_min[3], gcal_mag_max[3];
    uint16_t gcal_gyro_spread, gcal_mag_spread;

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
    uint32_t recover_delay_ms;  /* ADCS_AUTO_RECOVER_MS, doubled on repeat faults */
    uint32_t last_auto_recover;
    bool auto_recovered;        /* in DETUMBLE because of an automatic recovery */
} adcs_t;

void adcs_init(adcs_t *a, uint32_t now_ms);
void adcs_step(adcs_t *a, uint32_t now_ms);
void adcs_default_config(adcs_cfg_t *c);

#endif
