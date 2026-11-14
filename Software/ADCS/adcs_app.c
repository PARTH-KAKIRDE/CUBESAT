#include "adcs_app.h"

#include <math.h>
#include <string.h>

#include "adcs_hw.h"
#include "bdot.h"
#include "board_config.h"
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
    c->sun_lit = ADCS_SUN_LIT_DEFAULT;
    c->sun_full = ADCS_SUN_FULL_DEFAULT;
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

/* ---------------- sun sensors (ICD v0.6, v0.7) ----------------
 * One BPW34 per face. A face reads ~ full * cos(angle to the Sun), and ~0
 * when the Sun is behind it. Along an axis with a working diode on both
 * sides, (+face - -face) / full is that axis' component of the Sun direction.
 *
 * A face is AVAILABLE if its diode is fitted (board_config.h), not detected
 * dead and not disabled by command. If an axis has only one available side
 * and that side is dark while the Sun is visible, the Sun is on the other
 * side and the component comes from |s| = 1 (flag BLIND_SIDE). Of an
 * opposite pair only the brighter side is used: the Sun can light only one,
 * so the other reading is albedo or noise.
 *
 * lit is the albedo threshold: only a face at or above it counts as sunlit,
 * so the Earth's glow (below it) cannot fake daylight.
 *
 * Gyro tracking: between measurements the last Sun direction is turned with
 * the gyro (sun_track()). It serves two purposes:
 *   - HIDDEN: everything is dark, but the tracked Sun is behind a face with
 *     no working diode. That is "unknown", not eclipse.
 *   - dead diodes: in daylight the per-axis readings should add up to a
 *     unit vector. If light is missing, the dark face whose axis can hold
 *     it, on the side the tracked Sun favours, is suspected; still dark
 *     after 5 cycles while the tracked Sun lights it, it is marked dead
 *     (fault SUN_DIODE). Any face that reads light again is trusted again. */
typedef struct {
    uint8_t ch, bit;
    int8_t axis, sign;
} sun_face_t;

#define SUN_NOT_FITTED 0xFFu
#ifndef SUN_CH_MZ
#define SUN_CH_MZ_OR_NONE SUN_NOT_FITTED
#else
#define SUN_CH_MZ_OR_NONE SUN_CH_MZ
#endif

/* All six faces, in ADCS_FACE_* bit order. */
static const sun_face_t s_faces[6] = {
    {SUN_CH_PX, ADCS_FACE_PX, 0, +1}, {SUN_CH_MX, ADCS_FACE_MX, 0, -1},
    {SUN_CH_PY, ADCS_FACE_PY, 1, +1}, {SUN_CH_MY, ADCS_FACE_MY, 1, -1},
    {SUN_CH_PZ, ADCS_FACE_PZ, 2, +1}, {SUN_CH_MZ_OR_NONE, ADCS_FACE_MZ, 2, -1},
};

static uint8_t sun_fitted_faces(void)
{
    uint8_t m = 0;
    for (int i = 0; i < 6; i++)
        if (s_faces[i].ch != SUN_NOT_FITTED) m |= s_faces[i].bit;
    return m;
}

static uint8_t sun_available(const adcs_t *a) { return sun_fitted_faces() & ~(a->sun_dead | a->sun_disabled); }

/* cos(angle) between the tracked Sun and face i's outward normal. */
static float est_cos(const adcs_t *a, int i) { return (float)s_faces[i].sign * a->sun_est[s_faces[i].axis]; }

/* Gyro step, every gyro read: a direction fixed in space turns by -w in the
 * body frame, ds/dt = -(w x s) = s x w. */
static void sun_track(adcs_t *a, uint32_t now)
{
    uint32_t dt_ms = now - a->sun_track_ms;
    a->sun_track_ms = now;
    if (!a->sun_est_ok) return;
    if (!a->imu_ok) { /* cannot follow the rotation any more */
        a->sun_est_ok = false;
        return;
    }
    float k = (float)dt_ms * 1e-3f * 10.0f / (float)ADCS_GYRO_LSB_PER_DPS_X10 * 3.14159265f / 180.0f;
    float w[3] = {a->gyro[0] * k, a->gyro[1] * k, a->gyro[2] * k}; /* rad turned this step */
    float *s = a->sun_est;
    float n[3] = {s[0] + (s[1] * w[2] - s[2] * w[1]), s[1] + (s[2] * w[0] - s[0] * w[2]),
                  s[2] + (s[0] * w[1] - s[1] * w[0])};
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.0f)
        for (int i = 0; i < 3; i++) s[i] = n[i] / len;
}

/* Sun direction from the faces in mask avail. false if it cannot be found. */
static bool sun_measure(const adcs_t *a, uint8_t avail, float v[3], bool *blind, float *len2)
{
    int unknown = -1, n_unknown = 0;
    float sign_unknown = 0.0f, known2 = 0.0f;
    *blind = false;
    for (int axis = 0; axis < 3; axis++) {
        const sun_face_t *p = &s_faces[2 * axis], *m = &s_faces[2 * axis + 1];
        bool hp = avail & p->bit, hm = avail & m->bit;
        float cp = hp && a->sun[p->ch] >= ADCS_SUN_NOISE ? (float)a->sun[p->ch] / (float)a->cfg.sun_full : 0.0f;
        float cm = hm && a->sun[m->ch] >= ADCS_SUN_NOISE ? (float)a->sun[m->ch] / (float)a->cfg.sun_full : 0.0f;
        /* The Sun can light only one face of an opposite pair, so the dimmer
         * one is Earth albedo or noise: take the brighter side, not the
         * difference (which would subtract the albedo on the far face). */
        v[axis] = cp >= cm ? cp : -cm;
        bool known = (hp && hm) || cp > 0.0f || cm > 0.0f; /* one-sided but that side is lit: also known */
        if (!known) {
            n_unknown++;
            unknown = axis;
            sign_unknown = hp ? -1.0f : hm ? +1.0f : 0.0f; /* dark working side: Sun on the other */
        } else {
            known2 += v[axis] * v[axis];
        }
    }
    float rest = known2 < 1.0f ? sqrtf(1.0f - known2) : 0.0f;
    if (n_unknown == 1) {
        if (sign_unknown == 0.0f && rest > 0.2f) return false; /* no diode either side: sign unknown */
        v[unknown] = sign_unknown * rest;
        *blind = true; /* that component was inferred, even if it came out 0 */
    } else if (n_unknown > 1 && rest > 0.2f) {
        return false; /* two components unknown */
    }
    *len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; /* ~1 when all the light is accounted for */
    float len = sqrtf(*len2);
    if (len < 0.05f) return false;
    for (int i = 0; i < 3; i++) v[i] /= len;
    return true;
}

static void suspect_face(adcs_t *a, int i)
{
    if (++a->sun_suspect[i] >= ADCS_SUN_DEAD_CYCLES) {
        a->sun_dead |= s_faces[i].bit;
        a->sun_suspect[i] = 0;
        icd_node_raise_fault(&a->node, ADCS_FLT_SUN_DIODE);
    }
}

static void sun_update(adcs_t *a, uint32_t now)
{
    uint8_t fitted = sun_fitted_faces(), lit = 0, suspect = 0;
    for (int i = 0; i < 6; i++) {
        if (!(fitted & s_faces[i].bit)) continue;
        uint16_t raw = a->sun[s_faces[i].ch];
        if (raw >= ADCS_SUN_NOISE) a->sun_suspect[i] = 0; /* it sees light: not broken */
        if (raw >= a->cfg.sun_lit) {
            lit |= s_faces[i].bit;
            a->sun_dead &= (uint8_t)~s_faces[i].bit; /* a "dead" diode that works again */
        }
        if (a->sun_suspect[i]) suspect |= s_faces[i].bit;
    }
    lit &= (uint8_t)~a->sun_disabled;
    uint8_t avail = sun_available(a) & (uint8_t)~suspect; /* a suspect face is not trusted meanwhile */
    uint16_t mx = 0;
    for (int i = 0; i < 6; i++)
        if ((sun_available(a) & s_faces[i].bit) && a->sun[s_faces[i].ch] > mx) mx = a->sun[s_faces[i].ch];

    a->sun_lit_faces = lit;
    a->sun_max = mx;
    a->sun_flags = 0;
    memset(a->sun_vec, 0, sizeof a->sun_vec);
    if (!a->sun_ok) return; /* ADC fault: nothing known */

    bool trusted = a->sun_est_ok && (uint32_t)(now - a->sun_fix_ms) < ADCS_SUN_TRUST_MS;
    bool fresh = a->sun_est_ok && (uint32_t)(now - a->sun_fix_ms) < ADCS_SUN_TRACK_MAX_MS;
    float v[3], len2;
    bool blind;
    if (lit & avail) {
        a->sun_flags = ADCS_SUNF_LIT;
        if (!sun_measure(a, avail, v, &blind, &len2)) return;
        if (!blind && trusted && len2 * 1000.0f < (float)ADCS_SUN_DEFICIT_PERMILLE) {
            /* Light is missing: put it on the dark face whose axis can hold it,
             * on the side the tracked Sun favours (a dead diode facing the Sun). */
            float len = sqrtf(len2), miss = sqrtf(1.0f - len2), best = 0.8f;
            int pick = -1;
            float vb[3] = {0, 0, 0};
            for (int i = 0; i < 6; i++) {
                const sun_face_t *f = &s_faces[i];
                if (!(avail & f->bit) || a->sun[f->ch] >= ADCS_SUN_NOISE || v[f->axis] != 0.0f) continue;
                float ve[3] = {v[0] * len, v[1] * len, v[2] * len};
                ve[f->axis] = (float)f->sign * miss;
                float dot = ve[0] * a->sun_est[0] + ve[1] * a->sun_est[1] + ve[2] * a->sun_est[2];
                if (dot > best) { best = dot; pick = i; memcpy(vb, ve, sizeof ve); }
            }
            if (pick >= 0) {
                suspect_face(a, pick);
                memcpy(v, vb, sizeof vb); /* already unit length */
                blind = true;
            }
        } else if (trusted) {
            /* Faces already suspected: still dark where the tracked Sun lights them? */
            for (int i = 0; i < 6; i++)
                if ((suspect & s_faces[i].bit) && a->sun[s_faces[i].ch] < ADCS_SUN_NOISE &&
                    est_cos(a, i) * 1000.0f >= (float)ADCS_SUN_DEAD_PERMILLE)
                    suspect_face(a, i);
        }
        a->sun_flags |= ADCS_SUNF_VECTOR_VALID | (blind ? ADCS_SUNF_BLIND_SIDE : 0);
        for (int i = 0; i < 3; i++) a->sun_vec[i] = icd_sat_i16((int32_t)lroundf(v[i] * (float)ADCS_SUN_UNIT));
        memcpy(a->sun_est, v, sizeof v); /* a new fix for the tracker */
        a->sun_est_ok = true;
        a->sun_fix_ms = now;
        return;
    }
    /* Nothing lit. Eclipse, unless the tracked Sun sits where no working
     * diode can see it (it would light no available face). */
    if (fresh) {
        bool visible = false;
        for (int i = 0; i < 6; i++)
            if ((avail & s_faces[i].bit) && est_cos(a, i) * (float)a->cfg.sun_full >= (float)a->cfg.sun_lit) visible = true;
        if (!visible) {
            a->sun_flags = ADCS_SUNF_HIDDEN | ADCS_SUNF_PREDICTED | ADCS_SUNF_VECTOR_VALID;
            for (int i = 0; i < 3; i++)
                a->sun_vec[i] = icd_sat_i16((int32_t)lroundf(a->sun_est[i] * (float)ADCS_SUN_UNIT));
        }
    }
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
    if (a->sun_dead) f |= ADCS_FLT_SUN_DIODE;
    return f;
}

static uint8_t on_set_mode(void *ctx, uint8_t from, uint8_t to)
{
    adcs_t *a = (adcs_t *)ctx;
    (void)from;
    if (to == ICD_MODE_DETUMBLE && !a->mag_ok) return ICD_ST_HW_FAULT;
    a->manual = false;
    a->have_prev = false; /* restart dB/dt after any mode change */
    /* Stop whatever the old mode was driving now, not at the end of the
     * control cycle (NOMINAL means coils off; DETUMBLE recomputes next cycle). */
    memset(a->ctrl_duty, 0, sizeof a->ctrl_duty);
    coils_off(a);
    /* A commanded mode change ends any automatic-recovery episode. */
    a->auto_recovered = false;
    a->recover_delay_ms = ADCS_AUTO_RECOVER_MS;
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
    case ADCS_TLM_PAGE_SUN:
        icd_put_i16(&out[ADCS_TLM4_OFS_SUN_X], a->sun_vec[0]);
        icd_put_i16(&out[ADCS_TLM4_OFS_SUN_Y], a->sun_vec[1]);
        icd_put_i16(&out[ADCS_TLM4_OFS_SUN_Z], a->sun_vec[2]);
        out[ADCS_TLM4_OFS_SUN_FLAGS] = a->sun_flags;
        out[ADCS_TLM4_OFS_LIT_FACES] = a->sun_lit_faces;
        out[ADCS_TLM4_OFS_FITTED_FACES] = sun_fitted_faces();
        out[ADCS_TLM4_OFS_DEAD_FACES] = (uint8_t)((a->sun_dead | a->sun_disabled) & sun_fitted_faces());
        icd_put_u16(&out[ADCS_TLM4_OFS_SUN_MAX], a->sun_max);
        icd_put_u16(&out[ADCS_TLM4_OFS_LIT_THRESHOLD], a->cfg.sun_lit);
        icd_put_u16(&out[ADCS_TLM4_OFS_FULL_SCALE], a->cfg.sun_full);
        *len = ADCS_TLM4_LEN;
        return ICD_ST_OK;
    case ADCS_TLM_PAGE_GYRO_CAL:
        icd_put_i16(&out[ADCS_TLM3_OFS_GYRO_OFS_X], a->gyro_ofs[0]);
        icd_put_i16(&out[ADCS_TLM3_OFS_GYRO_OFS_Y], a->gyro_ofs[1]);
        icd_put_i16(&out[ADCS_TLM3_OFS_GYRO_OFS_Z], a->gyro_ofs[2]);
        out[ADCS_TLM3_OFS_CAL_STATE] = a->gcal_state;
        out[ADCS_TLM3_OFS_CAL_SAMPLES] = a->gcal_n;
        icd_put_u16(&out[ADCS_TLM3_OFS_GYRO_SPREAD], a->gcal_gyro_spread);
        icd_put_u16(&out[ADCS_TLM3_OFS_MAG_SPREAD], a->gcal_mag_spread);
        *len = ADCS_TLM3_LEN;
        return ICD_ST_OK;
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
    case ADCS_CMD_SET_SUN_CONFIG: {
        if (n != ADCS_SET_SUN_CONFIG_LEN) return ICD_ST_BAD_LENGTH;
        uint16_t lit = icd_get_u16(&req[0]), full = icd_get_u16(&req[2]);
        if (full < ADCS_SUN_FULL_MIN || full > 4095u || lit <= ADCS_SUN_NOISE || lit >= full) return ICD_ST_BAD_PARAM;
        c->sun_lit = lit;
        c->sun_full = full;
        sun_update(a, a->node.now_ms); /* telemetry reflects the new values at once */
        return ICD_ST_OK;
    }
    case ADCS_CMD_SET_SUN_DISABLED:
        if (n != ADCS_SET_SUN_DISABLED_LEN) return ICD_ST_BAD_LENGTH;
        if (req[0] & ~0x3Fu) return ICD_ST_BAD_PARAM;
        a->sun_disabled = req[0];
        a->sun_dead = 0; /* re-learn from scratch */
        memset(a->sun_suspect, 0, sizeof a->sun_suspect);
        sun_update(a, a->node.now_ms);
        return ICD_ST_OK;
    case ADCS_CMD_SET_GYRO_OFFSET: {
        if (n != ADCS_SET_GYRO_OFFSET_LEN) return ICD_ST_BAD_LENGTH;
        int16_t g[3];
        for (int i = 0; i < 3; i++) {
            g[i] = icd_get_i16(&req[2 * i]);
            if (g[i] > ADCS_GYRO_OFS_MAX || g[i] < -ADCS_GYRO_OFS_MAX) return ICD_ST_BAD_PARAM;
        }
        /* Overrides (and ends) the boot measurement. */
        memcpy(a->gyro_ofs, g, sizeof g);
        a->gcal_state = ADCS_GCAL_COMMANDED;
        return ICD_ST_OK;
    }
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

/* ---------------- gyro zero-rate offset (ICD v0.5) ----------------
 * For ADCS_GYRO_CAL_MS after boot every gyro read is averaged. The mean is
 * kept as the offset only if the board was still the whole time:
 *   - every gyro axis moved by at most ADCS_GYRO_CAL_MAX_SPREAD (no handling),
 *   - every magnetometer axis by at most ADCS_GYRO_CAL_MAX_MAG_SPREAD. A steady
 *     rotation does not change the gyro reading, but it does turn the Earth's
 *     field in the body frame, so the magnetometer catches it (0.5 dps over
 *     the 4 s between the first and last sample moves a 30 uT field ~1 uT).
 * Booting while tumbling (in orbit, after a reset) therefore ends REJECTED
 * with the offset left at 0; the OBC then re-sends the last good value. */
static void spread_update(int16_t mn[3], int16_t mx[3], const int16_t v[3], bool first)
{
    for (int i = 0; i < 3; i++) {
        if (first || v[i] < mn[i]) mn[i] = v[i];
        if (first || v[i] > mx[i]) mx[i] = v[i];
    }
}

static uint16_t spread_of(const int16_t mn[3], const int16_t mx[3])
{
    int32_t w = 0;
    for (int i = 0; i < 3; i++)
        if ((int32_t)mx[i] - mn[i] > w) w = (int32_t)mx[i] - mn[i];
    return w > 65535 ? 65535u : (uint16_t)w;
}

static void gcal_finish(adcs_t *a)
{
    a->gcal_gyro_spread = a->gcal_n ? spread_of(a->gcal_min, a->gcal_max) : 0xFFFFu;
    a->gcal_mag_spread = a->gcal_mag_n ? spread_of(a->gcal_mag_min, a->gcal_mag_max) : 0xFFFFu;
    int16_t ofs[3] = {0, 0, 0};
    bool ok = a->gcal_n >= ADCS_GYRO_CAL_MIN_SAMPLES && a->gcal_mag_n >= ADCS_GYRO_CAL_MIN_MAG &&
              a->gcal_gyro_spread <= ADCS_GYRO_CAL_MAX_SPREAD &&
              a->gcal_mag_spread <= ADCS_GYRO_CAL_MAX_MAG_SPREAD;
    for (int i = 0; ok && i < 3; i++) {
        int32_t sum = a->gcal_sum[i], n = a->gcal_n;
        int32_t m = (sum >= 0 ? sum + n / 2 : sum - n / 2) / n; /* rounded mean */
        if (m > ADCS_GYRO_OFS_MAX || m < -ADCS_GYRO_OFS_MAX) ok = false;
        else ofs[i] = (int16_t)m;
    }
    if (ok) memcpy(a->gyro_ofs, ofs, sizeof ofs);
    a->gcal_state = ok ? ADCS_GCAL_AUTO : ADCS_GCAL_REJECTED;
}

static void gyro_task(adcs_t *a, uint32_t now)
{
    if ((uint32_t)(now - a->gyro_time) < ADCS_GYRO_PERIOD_MS) return;
    a->gyro_time = now;
    if (a->gcal_state == ADCS_GCAL_RUNNING && (uint32_t)(now - a->gcal_start) >= ADCS_GYRO_CAL_MS)
        gcal_finish(a);
    if (!a->imu_ok) {
        a->sun_est_ok = false; /* no gyro: the tracked Sun direction is lost */
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
    if (adcs_hw_imu_read(a->gyro_raw, &a->imu_temp)) {
        a->imu_fails = 0;
        if (a->gcal_state == ADCS_GCAL_RUNNING && a->gcal_n < 255u) {
            spread_update(a->gcal_min, a->gcal_max, a->gyro_raw, a->gcal_n == 0);
            for (int i = 0; i < 3; i++) a->gcal_sum[i] += a->gyro_raw[i];
            a->gcal_n++;
        }
        for (int i = 0; i < 3; i++) a->gyro[i] = icd_sat_i16((int32_t)a->gyro_raw[i] - a->gyro_ofs[i]);
        sun_track(a, now);
    } else if (++a->imu_fails >= ADCS_FAIL_LIMIT) {
        a->imu_ok = false;
        a->imu_retry_at = now + ADCS_REINIT_PERIOD_MS;
    }
    update_rate(a, now);
}

/* Drop to SAFE on a fault. If this fault arrives soon after an automatic
 * recovery, the fault is probably still there (it only looked clear while
 * the drivers slept), so wait twice as long before the next attempt. */
static void fault_safe(adcs_t *a, uint32_t now)
{
    if (a->auto_recovered && (uint32_t)(now - a->last_auto_recover) < ADCS_RECOVER_STABLE_MS) {
        a->recover_delay_ms *= 2u;
        if (a->recover_delay_ms > ADCS_RECOVER_MAX_MS) a->recover_delay_ms = ADCS_RECOVER_MAX_MS;
    }
    a->auto_recovered = false;
    a->manual = false;
    enter_mode(a, ICD_MODE_SAFE, ICD_MR_FAULT);
}

static void mag_failed(adcs_t *a, uint32_t now)
{
    if (++a->mag_fails >= ADCS_FAIL_LIMIT && a->mag_ok) {
        a->mag_ok = false;
        a->mag_valid = false;
        a->have_prev = false;
        a->mag_retry_at = now + ADCS_REINIT_PERIOD_MS;
        if (icd_node_mode(&a->node) == ICD_MODE_DETUMBLE) fault_safe(a, now);
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
static void drv_cycle_check(adcs_t *a, uint32_t now)
{
    if (a->drv_faults) {
        if (++a->drv_fault_cycles >= ADCS_DRV_FAULT_CYCLES &&
            icd_node_mode(&a->node) != ICD_MODE_SAFE)
            fault_safe(a, now);
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
        drv_cycle_check(a, now);
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
            /* Raw values: the OBC sends SET_MAG_OFFSET right after boot, inside
             * the window, and the offset jump must not look like movement. */
            if (a->gcal_state == ADCS_GCAL_RUNNING && a->gcal_mag_n < 255u) {
                spread_update(a->gcal_mag_min, a->gcal_mag_max, raw, a->gcal_mag_n == 0);
                a->gcal_mag_n++;
            }
        } else if (a->mag_ok) {
            mag_failed(a, now);
        }

        /* Sun sensors are read here too: once per cycle, coils still off. */
        a->sun_ok = adcs_hw_sun_read(a->sun);
        sun_update(a, now);

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

    /* Autonomy: if we dropped to SAFE because of a fault, the faults that stop
     * B-dot have been gone for recover_delay_ms (30 s, longer after repeats)
     * and the OBC is silent, resume detumbling on our own. Only the
     * magnetometer and the coil drivers matter here: B-dot needs neither the
     * gyro nor the sun sensors, so a dead IMU or sun ADC must not keep ADCS
     * in SAFE. (In SAFE the drivers sleep and read 0; a still-broken driver
     * shows up again after the restart and the wait doubles.) */
    if (!a->mag_ok || a->drv_faults) {
        a->faults_clear_since = now;
    } else if (icd_node_mode(&a->node) == ICD_MODE_SAFE && a->node.mode_reason == ICD_MR_FAULT &&
               icd_node_master_lost(&a->node) &&
               (uint32_t)(now - a->faults_clear_since) >= a->recover_delay_ms) {
        enter_mode(a, ICD_MODE_DETUMBLE, ICD_MR_AUTO);
        a->have_prev = false;
        a->auto_recovered = true;
        a->last_auto_recover = now;
    }
    /* A long enough clean run after an automatic recovery resets the wait. */
    if (a->auto_recovered && icd_node_mode(&a->node) != ICD_MODE_SAFE &&
        (uint32_t)(now - a->last_auto_recover) >= ADCS_RECOVER_STABLE_MS) {
        a->auto_recovered = false;
        a->recover_delay_ms = ADCS_AUTO_RECOVER_MS;
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
    a->sun_track_ms = now;
    sun_update(a, now);
    a->imu_retry_at = a->mag_retry_at = now + ADCS_REINIT_PERIOD_MS;
    a->local_i2c_seen = adcs_hw_local_i2c_errors();
    a->faults_clear_since = now;
    a->recover_delay_ms = ADCS_AUTO_RECOVER_MS;
    /* Gyro offset: measured over the first ADCS_GYRO_CAL_MS (see gcal_finish). */
    a->gcal_state = a->imu_ok ? ADCS_GCAL_RUNNING : ADCS_GCAL_NONE;
    a->gcal_start = now;
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
