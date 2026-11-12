/*
 * Simulated ADCS hardware: a tumbling 2U CubeSat in a slowly rotating
 * geomagnetic field, three magnetorquers, a gyro, a magnetometer and six
 * sun sensors. Good enough to check that B-dot really removes the spin and
 * that the firmware never samples the magnetometer with a coil on.
 */
#include <math.h>
#include <string.h>

#include "adcs_hw.h"
#include "cubesat_icd.h"
#include "sim_api.h"
#include "sim_internal.h"

#define PI 3.14159265358979323846
#define M_MAX 0.2            /* A m^2 per coil at full duty */
#define ORBIT_S 5400.0
#define COIL_LEAK_UT 20.0    /* field seen by the mag at full duty (why we multiplex) */

static const double J[3] = {0.011, 0.011, 0.0044}; /* kg m^2, 2U ~2.7 kg */

static double w[3];      /* body rate rad/s */
static double q[4];      /* body -> inertial quaternion (w, x, y, z) */
static double t_s;
static int16_t duty[3];
static int enabled;
static uint32_t faults;
static uint32_t rng;
static uint32_t mag_done_at;
static int mag_pending;
static int16_t mag_sample[3];
static uint32_t dirty_samples;

static double frand(void) /* -1 .. 1 */
{
    rng = rng * 1664525u + 1013904223u;
    return ((double)(rng >> 8) / 8388608.0) - 1.0;
}

static void b_inertial(double out[3])
{
    double th = 2.0 * PI * t_s / ORBIT_S;
    out[0] = 25.0 * cos(th);
    out[1] = 8.0;
    out[2] = 45.0 * sin(th);
}

/* v_body = R(q)^T v_inertial */
static void to_body(const double v[3], double o[3])
{
    double a = q[0], b = q[1], c = q[2], d = q[3];
    double R[3][3] = {
        {a * a + b * b - c * c - d * d, 2 * (b * c - a * d), 2 * (b * d + a * c)},
        {2 * (b * c + a * d), a * a - b * b + c * c - d * d, 2 * (c * d - a * b)},
        {2 * (b * d - a * c), 2 * (c * d + a * b), a * a - b * b - c * c + d * d},
    };
    for (int i = 0; i < 3; i++) o[i] = R[0][i] * v[0] + R[1][i] * v[1] + R[2][i] * v[2];
}

void adcs_sim_reset(uint32_t seed)
{
    rng = seed ? seed : 1u;
    t_s = 0;
    q[0] = 1;
    q[1] = q[2] = q[3] = 0;
    w[0] = 10.0 * PI / 180.0;
    w[1] = -8.0 * PI / 180.0;
    w[2] = 6.0 * PI / 180.0;
    memset(duty, 0, sizeof duty);
    enabled = 0;
    faults = 0;
    mag_pending = 0;
    dirty_samples = 0;
}

void adcs_sim_physics_step(double dt)
{
    double bi[3], bb[3], m[3], tau[3], Jw[3];
    b_inertial(bi);
    to_body(bi, bb);
    for (int i = 0; i < 3; i++) {
        m[i] = enabled ? (duty[i] / 1000.0) * M_MAX : 0.0;
        bb[i] *= 1e-6; /* uT -> T */
        Jw[i] = J[i] * w[i];
    }
    tau[0] = m[1] * bb[2] - m[2] * bb[1];
    tau[1] = m[2] * bb[0] - m[0] * bb[2];
    tau[2] = m[0] * bb[1] - m[1] * bb[0];
    double gyro_t[3] = {w[1] * Jw[2] - w[2] * Jw[1], w[2] * Jw[0] - w[0] * Jw[2],
                        w[0] * Jw[1] - w[1] * Jw[0]};
    for (int i = 0; i < 3; i++) w[i] += dt * (tau[i] - gyro_t[i]) / J[i];

    double dq[4] = {
        0.5 * (-q[1] * w[0] - q[2] * w[1] - q[3] * w[2]),
        0.5 * (q[0] * w[0] + q[2] * w[2] - q[3] * w[1]),
        0.5 * (q[0] * w[1] - q[1] * w[2] + q[3] * w[0]),
        0.5 * (q[0] * w[2] + q[1] * w[1] - q[2] * w[0]),
    };
    double n = 0;
    for (int i = 0; i < 4; i++) {
        q[i] += dt * dq[i];
        n += q[i] * q[i];
    }
    n = sqrt(n);
    for (int i = 0; i < 4; i++) q[i] /= n;
    t_s += dt;

    if (mag_pending && sim_time_ms() >= mag_done_at) {
        double b[3];
        b_inertial(bi);
        to_body(bi, b);
        int dirty = 0;
        for (int i = 0; i < 3; i++) {
            double leak = enabled ? (duty[i] / 1000.0) * COIL_LEAK_UT : 0.0;
            if (enabled && duty[i] != 0) dirty = 1;
            double ut = b[i] + leak + 0.05 * frand();
            mag_sample[i] = (int16_t)lround(ut * 1000.0 / ADCS_MAG_NT_PER_LSB);
        }
        if (dirty) dirty_samples++;
        mag_pending = 2; /* ready */
    }
}

/* ---------------- adcs_hw.h implementation ---------------- */

bool adcs_hw_init(void) { return true; }
bool adcs_hw_imu_init(void) { return !(faults & SIM_ADCS_FAIL_IMU); }

bool adcs_hw_imu_read(int16_t gyro[3], int16_t *temp)
{
    if (faults & SIM_ADCS_FAIL_IMU) return false;
    for (int i = 0; i < 3; i++) {
        double dps = w[i] * 180.0 / PI + 0.02 * frand();
        double raw = dps * ADCS_GYRO_LSB_PER_DPS_X10 / 10.0;
        if (raw > 32767) raw = 32767;
        if (raw < -32768) raw = -32768;
        gyro[i] = (int16_t)lround(raw);
    }
    *temp = 2500;
    return true;
}

bool adcs_hw_mag_init(void) { return !(faults & SIM_ADCS_FAIL_MAG); }

bool adcs_hw_mag_start(void)
{
    if (faults & SIM_ADCS_FAIL_MAG) return false;
    mag_pending = 1;
    mag_done_at = sim_time_ms() + 8u;
    return true;
}

int adcs_hw_mag_poll(int16_t mag[3])
{
    if (faults & SIM_ADCS_FAIL_MAG) return -1;
    if (mag_pending != 2) return 0;
    memcpy(mag, mag_sample, sizeof mag_sample);
    mag_pending = 0;
    return 1;
}

void adcs_hw_mtq_enable(bool on) { enabled = on ? 1 : 0; }
void adcs_hw_mtq_set(const int16_t d[3]) { memcpy(duty, d, sizeof duty); }

uint8_t adcs_hw_drv_faults(void)
{
    if (!enabled) return 0;
    return (uint8_t)((faults >> 2) & 7u);
}

bool adcs_hw_sun_read(uint16_t out[6])
{
    static const double sun_i[3] = {1, 0, 0};
    double s[3];
    to_body(sun_i, s);
    for (int i = 0; i < 3; i++) {
        double p = s[i] > 0 ? s[i] : 0, m = s[i] < 0 ? -s[i] : 0;
        out[2 * i] = (uint16_t)(50 + 3000 * p);
        out[2 * i + 1] = (uint16_t)(50 + 3000 * m);
    }
    return true;
}

uint16_t adcs_hw_local_i2c_errors(void) { return 0; }

/* ---------------- sim API ---------------- */

SIM_API void sim_adcs_set_rate_dps(double x, double y, double z)
{
    w[0] = x * PI / 180.0;
    w[1] = y * PI / 180.0;
    w[2] = z * PI / 180.0;
}

SIM_API void sim_adcs_get_rate_dps(double *o)
{
    for (int i = 0; i < 3; i++) o[i] = w[i] * 180.0 / PI;
}

SIM_API void sim_adcs_set_faults(uint32_t mask) { faults = mask; }
SIM_API uint32_t sim_adcs_dirty_mag_samples(void) { return dirty_samples; }
