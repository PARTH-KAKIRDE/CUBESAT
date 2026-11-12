#include "adcs_app.h"

#include <math.h>
#include <string.h>

#include "adcs_hw.h"
#include "bdot.h"
#include "icd_pack.h"

/* Mode behaviour (see ICD.md section 8):
 *   BOOT     self-test, coils asleep. Leaves on its own after init.
 *   SAFE     DRV8833 asleep (nSLEEP low). Sensors still sampled, telemetry live.
 *   DETUMBLE B-dot loop runs autonomously; coils off while the magnetometer samples.
 *   NOMINAL  coils off (pointing law TBD). MTQ_MANUAL is allowed here only.     */

void adcs_default_config(adcs_cfg_t *c)
{
    c->gain = ADCS_BDOT_GAIN_DEFAULT;
    c->exit_rate = ADCS_EXIT_RATE_DEFAULT;
    c->enter_rate = ADCS_ENTER_RATE_DEFAULT;
    c->dwell_s = ADCS_DWELL_S_DEFAULT;
    c->period_ms = ADCS_PERIOD_MS_DEFAULT;
    c->settle_ms = ADCS_SETTLE_MS_DEFAULT;
    c->mag_ofs[0] = c->mag_ofs[1] = c->mag_ofs[2] = 0;
}

static void coils(adcs_t *a, const int16_t d[3])
{
    memcpy(a->duty, d, sizeof a->duty);
    adcs_hw_mtq_set(d);
}

static void coils_off(adcs_t *a)
{
    static const int16_t z[3] = {0, 0, 0};
    coils(a, z);
}

static void enter_mode(adcs_t *a, uint8_t mode, uint8_t reason)
{
    icd_node_set_mode(&a->node, mode, reason);
}

/* ---------------- ICD callbacks ---------------- */

static uint16_t live_faults(void *ctx)
{
    adcs_t *a = (adcs_t *)ctx;
    uint16_t f = 0;
    if (!a->mag_ok) f |= ADCS_FLT_MAG;
    if (!a->imu_ok) f |= ADCS_FLT_IMU;
    if (a->drv_faults & 1u) f |= ADCS_FLT_DRVX;
    if (a->drv_faults & 2u) f |= ADCS_FLT_DRVY;
    if (a->drv_faults & 4u) f |= ADCS_FLT_DRVZ;
    if (!a->sun_ok) f |= ADCS_FLT_SUN_ADC;
    return f;
}

static uint8_t on_set_mode(void *ctx, uint8_t from, uint8_t to)
{
    adcs_t *a = (adcs_t *)ctx;
    (void)from;
    if (to == ICD_MODE_DETUMBLE && !a->mag_ok) return ICD_ST_HW_FAULT;
    a->manual = false;
    a->have_prev = false; /* restart dB/dt after any mode change */
    return ICD_ST_OK;
}

static uint8_t get_telemetry(void *ctx, uint8_t page, uint8_t *out, uint8_t *len)
{
    adcs_t *a = (adcs_t *)ctx;
    uint32_t now = a->node.now_ms;
    switch (page) {
    case ADCS_TLM_PAGE_SENSORS: {
        icd_put_u32(&out[ADCS_TLM0_OFS_UPTIME_MS], now);
        icd_put_i16(&out[ADCS_TLM0_OFS_MAG_X], a->mag[0]);
        icd_put_i16(&out[ADCS_TLM0_OFS_MAG_Y], a->mag[1]);
        icd_put_i16(&out[ADCS_TLM0_OFS_MAG_Z], a->mag[2]);
        icd_put_i16(&out[ADCS_TLM0_OFS_GYRO_X], a->gyro[0]);
        icd_put_i16(&out[ADCS_TLM0_OFS_GYRO_Y], a->gyro[1]);
        icd_put_i16(&out[ADCS_TLM0_OFS_GYRO_Z], a->gyro[2]);
        uint16_t age = ADCS_MAG_AGE_INVALID;
        if (a->mag_valid) {
            uint32_t d = now - a->mag_time;
            age = d >= ADCS_MAG_AGE_INVALID ? (uint16_t)(ADCS_MAG_AGE_INVALID - 1u) : (uint16_t)d;
        }
        icd_put_u16(&out[ADCS_TLM0_OFS_MAG_AGE_MS], age);
        *len = ADCS_TLM0_LEN;
        return ICD_ST_OK;
    }
    case ADCS_TLM_PAGE_SUN_MTQ:
        for (int i = 0; i < 6; i++) icd_put_u16(&out[ADCS_TLM1_OFS_SUN1 + 2 * i], a->sun[i]);
        /* Report the duty the controller is applying this cycle (the coils
         * are briefly switched off around each magnetometer sample). */
        icd_put_i16(&out[ADCS_TLM1_OFS_DUTY_X], a->ctrl_duty[0]);
        icd_put_i16(&out[ADCS_TLM1_OFS_DUTY_Y], a->ctrl_duty[1]);
        icd_put_i16(&out[ADCS_TLM1_OFS_DUTY_Z], a->ctrl_duty[2]);
        *len = ADCS_TLM1_LEN;
        return ICD_ST_OK;
    case ADCS_TLM_PAGE_CTRL: {
        icd_put_i16(&out[ADCS_TLM2_OFS_DBDT_X], a->dbdt[0]);
        icd_put_i16(&out[ADCS_TLM2_OFS_DBDT_Y], a->dbdt[1]);
        icd_put_i16(&out[ADCS_TLM2_OFS_DBDT_Z], a->dbdt[2]);
        icd_put_u16(&out[ADCS_TLM2_OFS_RATE_NORM], a->rate_norm);
        icd_put_i16(&out[ADCS_TLM2_OFS_IMU_TEMP], a->imu_temp);
        uint8_t cf = 0;
        if (a->detumbled) cf |= ADCS_CF_DETUMBLED;
        if (a->ctrl_duty[0] || a->ctrl_duty[1] || a->ctrl_duty[2]) cf |= ADCS_CF_COILS_ACTIVE;
        if (a->manual) cf |= ADCS_CF_MANUAL_ACTIVE;
        if (a->mag_valid && a->mag_ok) cf |= ADCS_CF_MAG_VALID;
        out[ADCS_TLM2_OFS_CTRL_FLAGS] = cf;
        out[ADCS_TLM2_OFS_MODE_REASON] = a->node.mode_reason;
        icd_put_u16(&out[ADCS_TLM2_OFS_OBC_I2C_RESETS], (uint16_t)a->node.link.stats.bus_resets);
        icd_put_u16(&out[ADCS_TLM2_OFS_LOCAL_I2C_ERR], adcs_hw_local_i2c_errors());
        *len = ADCS_TLM2_LEN;
        return ICD_ST_OK;
    }
    default:
        return ICD_ST_BAD_PARAM;
    }
}

static uint8_t app_command(void *ctx, uint8_t cmd, const uint8_t *req, uint8_t n,
                           uint8_t *resp, uint8_t *resp_len)
{
    adcs_t *a = (adcs_t *)ctx;
    adcs_cfg_t *c = &a->cfg;
    *resp_len = 0;
    switch (cmd) {
    case ADCS_CMD_SET_BDOT_GAIN: {
        if (n != ADCS_SET_BDOT_GAIN_LEN) return ICD_ST_BAD_LENGTH;
        uint32_t g = icd_get_u32(req);
        if (g > ADCS_BDOT_GAIN_MAX) return ICD_ST_BAD_PARAM;
        c->gain = g;
        return ICD_ST_OK;
    }
    case ADCS_CMD_SET_DETUMBLE_THRESH: {
        if (n != ADCS_SET_DETUMBLE_THRESH_LEN) return ICD_ST_BAD_LENGTH;
        uint16_t ex = icd_get_u16(&req[0]), en = icd_get_u16(&req[2]), dw = icd_get_u16(&req[4]);
        if (ex == 0 || en <= ex) return ICD_ST_BAD_PARAM;
        c->exit_rate = ex;
        c->enter_rate = en;
        c->dwell_s = dw;
        return ICD_ST_OK;
    }
    case ADCS_CMD_SET_BDOT_TIMING: {
        if (n != ADCS_SET_BDOT_TIMING_LEN) return ICD_ST_BAD_LENGTH;
        uint16_t p = icd_get_u16(&req[0]), s = icd_get_u16(&req[2]);
        if (p < ADCS_PERIOD_MS_MIN || p > ADCS_PERIOD_MS_MAX) return ICD_ST_BAD_PARAM;
        if (s < ADCS_SETTLE_MS_MIN || (uint32_t)s + ADCS_MAG_TIMEOUT_MS > p / 2u) return ICD_ST_BAD_PARAM;
        c->period_ms = p;
        c->settle_ms = s;
        a->have_prev = false;
        return ICD_ST_OK;
    }
    case ADCS_CMD_GET_CONFIG:
        if (n != 0) return ICD_ST_BAD_LENGTH;
        icd_put_u32(&resp[ADCS_CFG_OFS_GAIN], c->gain);
        icd_put_u16(&resp[ADCS_CFG_OFS_EXIT_RATE], c->exit_rate);
        icd_put_u16(&resp[ADCS_CFG_OFS_ENTER_RATE], c->enter_rate);
        icd_put_u16(&resp[ADCS_CFG_OFS_DWELL_S], c->dwell_s);
        icd_put_u16(&resp[ADCS_CFG_OFS_PERIOD_MS], c->period_ms);
        icd_put_u16(&resp[ADCS_CFG_OFS_SETTLE_MS], c->settle_ms);
        icd_put_i16(&resp[ADCS_CFG_OFS_MAG_OFS_X], c->mag_ofs[0]);
        icd_put_i16(&resp[ADCS_CFG_OFS_MAG_OFS_Y], c->mag_ofs[1]);
        icd_put_i16(&resp[ADCS_CFG_OFS_MAG_OFS_Z], c->mag_ofs[2]);
        *resp_len = ADCS_CFG_LEN;
        return ICD_ST_OK;
    case ADCS_CMD_MTQ_MANUAL: {
        if (n != ADCS_MTQ_MANUAL_LEN) return ICD_ST_BAD_LENGTH;
        if (icd_node_mode(&a->node) != ICD_MODE_NOMINAL) return ICD_ST_INVALID_MODE;
        int16_t d[3];
        for (int i = 0; i < 3; i++) {
            d[i] = icd_get_i16(&req[2 * i]);
            if (d[i] > ADCS_DUTY_MAX || d[i] < -ADCS_DUTY_MAX) return ICD_ST_BAD_PARAM;
        }
        uint16_t dur = icd_get_u16(&req[6]);
        if (dur > ADCS_MANUAL_MAX_MS) return ICD_ST_BAD_PARAM;
        if (dur == 0) {
            a->manual = false;
            memset(a->ctrl_duty, 0, sizeof a->ctrl_duty);
            coils_off(a);
            return ICD_ST_OK;
        }
        memcpy(a->manual_duty, d, sizeof d);
        memcpy(a->ctrl_duty, d, sizeof d);
        a->manual = true;
        a->manual_until = a->node.now_ms + dur;
        /* Apply now unless the magnetometer is mid-sample. */
        if (a->phase == PH_ACTUATE) coils(a, d);
        return ICD_ST_OK;
    }
    case ADCS_CMD_SET_MAG_OFFSET:
        if (n != ADCS_SET_MAG_OFFSET_LEN) return ICD_ST_BAD_LENGTH;
        for (int i = 0; i < 3; i++) c->mag_ofs[i] = icd_get_i16(&req[2 * i]);
        a->have_prev = false;
        return ICD_ST_OK;
    default:
        return ICD_ST_UNKNOWN_CMD;
    }
}

static icd_node_app_t s_app = {
    .fw_major = ADCS_FW_MAJOR,
    .fw_minor = ADCS_FW_MINOR,
    .fw_patch = ADCS_FW_PATCH,
    .live_faults = live_faults,
    .on_set_mode = on_set_mode,
    .get_telemetry = get_telemetry,
    .app_command = app_command,
};

/* ---------------- control cycle ---------------- */

static void update_rate(adcs_t *a, uint32_t now)
{
    float s = 0.0f;
    for (int i = 0; i < 3; i++) {
        float dps = (float)a->gyro[i] * 10.0f / (float)ADCS_GYRO_LSB_PER_DPS_X10;
        s += dps * dps;
    }
    float norm = sqrtf(s) * 100.0f;
    a->rate_norm = norm > 65535.0f ? 65535u : (uint16_t)norm;

    if (!a->imu_ok) {
        a->below = false;
        a->detumbled = false;
        return;
    }
    if (a->rate_norm < a->cfg.exit_rate) {
        if (!a->below) {
            a->below = true;
            a->below_since = now;
        }
        if ((uint32_t)(now - a->below_since) >= (uint32_t)a->cfg.dwell_s * 1000u) a->detumbled = true;
    } else {
        a->below = false;
        if (a->rate_norm > a->cfg.enter_rate) a->detumbled = false;
    }
}

static void gyro_task(adcs_t *a, uint32_t now)
{
    if ((uint32_t)(now - a->gyro_time) < ADCS_GYRO_PERIOD_MS) return;
    a->gyro_time = now;
    if (!a->imu_ok) {
        if ((int32_t)(now - a->imu_retry_at) >= 0) {
            a->imu_retry_at = now + ADCS_REINIT_PERIOD_MS;
            if (adcs_hw_imu_init()) {
                a->imu_ok = true;
                a->imu_fails = 0;
            }
        }
        update_rate(a, now);
        return;
    }
    if (adcs_hw_imu_read(a->gyro, &a->imu_temp)) {
        a->imu_fails = 0;
    } else if (++a->imu_fails >= ADCS_FAIL_LIMIT) {
        a->imu_ok = false;
        a->imu_retry_at = now + ADCS_REINIT_PERIOD_MS;
    }
    update_rate(a, now);
}

static void mag_failed(adcs_t *a, uint32_t now)
{
    if (++a->mag_fails >= ADCS_FAIL_LIMIT && a->mag_ok) {
        a->mag_ok = false;
        a->mag_valid = false;
        a->have_prev = false;
        a->mag_retry_at = now + ADCS_REINIT_PERIOD_MS;
        if (icd_node_mode(&a->node) == ICD_MODE_DETUMBLE) enter_mode(a, ICD_MODE_SAFE, ICD_MR_FAULT);
    }
}

static void compute_control(adcs_t *a, uint32_t now)
{
    uint8_t mode = icd_node_mode(&a->node);
    int16_t d[3] = {0, 0, 0};

    if (a->manual && (int32_t)(now - a->manual_until) >= 0) a->manual = false;

    if (mode == ICD_MODE_DETUMBLE) {
        if (a->mag_valid && a->have_prev && a->mag_prev_time != a->mag_time) {
            bdot_compute(a->mag, a->mag_prev, a->mag_time - a->mag_prev_time, a->cfg.gain, d, a->dbdt);
        }
    } else if (mode == ICD_MODE_NOMINAL && a->manual) {
        memcpy(d, a->manual_duty, sizeof d);
    }
    memcpy(a->ctrl_duty, d, sizeof d);
}

/* Once per control cycle: a driver fault that persists for
 * ADCS_DRV_FAULT_CYCLES cycles drops ADCS to SAFE. */
static void drv_cycle_check(adcs_t *a)
{
    if (a->drv_faults) {
        if (++a->drv_fault_cycles >= ADCS_DRV_FAULT_CYCLES &&
            icd_node_mode(&a->node) != ICD_MODE_SAFE) {
            enter_mode(a, ICD_MODE_SAFE, ICD_MR_FAULT);
            a->manual = false;
        }
    } else {
        a->drv_fault_cycles = 0;
    }
}

static void control_task(adcs_t *a, uint32_t now)
{
    switch (a->phase) {
    case PH_ACTUATE:
        if (a->manual && (int32_t)(now - a->manual_until) >= 0) {
            a->manual = false;
            memset(a->ctrl_duty, 0, sizeof a->ctrl_duty);
            coils_off(a);
        }
        if ((uint32_t)(now - a->cycle_start) < a->cfg.period_ms) return;
        a->cycle_start = now;
        drv_cycle_check(a);
        /* Coils off so the magnetometer reads the Earth field, not ours. */
        coils_off(a);
        a->phase = PH_SETTLE;
        /* fall through */
    case PH_SETTLE:
        if ((uint32_t)(now - a->cycle_start) < a->cfg.settle_ms) return;
        if (!a->mag_ok) {
            if ((int32_t)(now - a->mag_retry_at) >= 0) {
                a->mag_retry_at = now + ADCS_REINIT_PERIOD_MS;
                if (adcs_hw_mag_init()) {
                    a->mag_ok = true;
                    a->mag_fails = 0;
                }
            }
        }
        if (a->mag_ok && adcs_hw_mag_start()) {
            a->meas_start = now;
            a->last_mag_poll = now;
            a->phase = PH_MEASURE;
            return;
        }
        a->phase = PH_MEASURE;
        a->meas_start = now - ADCS_MAG_TIMEOUT_MS; /* skip straight to timeout handling */
        /* fall through */
    case PH_MEASURE: {
        uint32_t el = now - a->meas_start;
        int r = 0;
        int16_t raw[3];
        if (el < ADCS_MAG_TIMEOUT_MS) {
            if (el < ADCS_MAG_FIRST_POLL_MS || (uint32_t)(now - a->last_mag_poll) < 2u) return;
            a->last_mag_poll = now;
            r = adcs_hw_mag_poll(raw);
            if (r == 0) return;
        } else {
            r = -1;
        }
        if (r > 0) {
            if (a->mag_valid) {
                memcpy(a->mag_prev, a->mag, sizeof a->mag);
                a->mag_prev_time = a->mag_time;
                a->have_prev = true;
            }
            for (int i = 0; i < 3; i++) a->mag[i] = icd_sat_i16((int32_t)raw[i] - a->cfg.mag_ofs[i]);
            a->mag_time = now;
            a->mag_valid = true;
            a->mag_fails = 0;
        } else if (a->mag_ok) {
            mag_failed(a, now);
        }

        /* Sun sensors are read here too: once per cycle, coils still off. */
        a->sun_ok = adcs_hw_sun_read(a->sun);

        compute_control(a, now);
        uint8_t mode = icd_node_mode(&a->node);
        adcs_hw_mtq_enable(mode == ICD_MODE_DETUMBLE || mode == ICD_MODE_NOMINAL);
        coils(a, a->ctrl_duty);
        a->phase = PH_ACTUATE;
        break;
    }
    }
}

static void health_task(adcs_t *a, uint32_t now)
{
    uint8_t mode = icd_node_mode(&a->node);
    bool drivers_awake = (mode == ICD_MODE_DETUMBLE || mode == ICD_MODE_NOMINAL);

    a->drv_faults = drivers_awake ? adcs_hw_drv_faults() : 0;

    uint16_t e = adcs_hw_local_i2c_errors();
    if (e != a->local_i2c_seen) {
        a->local_i2c_seen = e;
        icd_node_raise_fault(&a->node, ADCS_FLT_LOCAL_I2C);
    }
    uint32_t r = a->node.link.stats.bus_resets;
    if (r != a->obc_resets_seen) {
        a->obc_resets_seen = r;
        icd_node_raise_fault(&a->node, ADCS_FLT_OBC_I2C);
    }

    /* SAFE also means the drivers sleep, whatever happened above. */
    if (icd_node_mode(&a->node) == ICD_MODE_SAFE) {
        if (a->duty[0] || a->duty[1] || a->duty[2]) coils_off(a);
        memset(a->ctrl_duty, 0, sizeof a->ctrl_duty);
        adcs_hw_mtq_enable(false);
    }

    /* Autonomy: if we dropped to SAFE because of a fault, the fault has been
     * gone for 30 s and the OBC is silent, resume detumbling on our own. */
    if (live_faults(a) != 0) {
        a->faults_clear_since = now;
    } else if (icd_node_mode(&a->node) == ICD_MODE_SAFE && a->node.mode_reason == ICD_MR_FAULT &&
               icd_node_master_lost(&a->node) &&
               (uint32_t)(now - a->faults_clear_since) >= ADCS_AUTO_RECOVER_MS) {
        enter_mode(a, ICD_MODE_DETUMBLE, ICD_MR_AUTO);
        a->have_prev = false;
    }
}

void adcs_init(adcs_t *a, uint32_t now)
{
    memset(a, 0, sizeof *a);
    adcs_default_config(&a->cfg);
    s_app.ctx = a;
    icd_node_init(&a->node, &s_app, now);

    /* BOOT: self-test. */
    adcs_hw_init();
    adcs_hw_mtq_enable(false);
    coils_off(a);
    a->imu_ok = adcs_hw_imu_init();
    a->mag_ok = adcs_hw_mag_init();
    a->sun_ok = adcs_hw_sun_read(a->sun);
    a->imu_retry_at = a->mag_retry_at = now + ADCS_REINIT_PERIOD_MS;
    a->local_i2c_seen = adcs_hw_local_i2c_errors();
    a->faults_clear_since = now;
    a->gyro_time = now - ADCS_GYRO_PERIOD_MS;
    a->phase = PH_ACTUATE;
    a->cycle_start = now - a->cfg.period_ms; /* start the first cycle at once */

    /* ICD: after self-test ADCS enters DETUMBLE on its own. Without a
     * magnetometer B-dot is impossible, so it goes to SAFE instead. */
    if (a->mag_ok)
        enter_mode(a, ICD_MODE_DETUMBLE, ICD_MR_AUTO);
    else
        enter_mode(a, ICD_MODE_SAFE, ICD_MR_FAULT);
}

void adcs_step(adcs_t *a, uint32_t now)
{
    icd_node_poll(&a->node, now);
    gyro_task(a, now);
    control_task(a, now);
    health_task(a, now);
}
