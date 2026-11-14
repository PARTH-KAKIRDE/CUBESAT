#include "bdot.h"

#include "cubesat_icd.h"
#include "icd_pack.h"

static float fabs_f(float x) { return x < 0.0f ? -x : x; }

void bdot_compute(const int16_t b_now[3], const int16_t b_prev[3], uint32_t dt_ms,
                  uint32_t gain, int16_t duty_out[3], int16_t dbdt_centi[3])
{
    float duty[3];
    float maxabs = 0.0f;
    if (dt_ms == 0) dt_ms = 1;

    for (int i = 0; i < 3; i++) {
        float d_lsb = (float)((int32_t)b_now[i] - (int32_t)b_prev[i]);
        /* uT/s = d_lsb * 0.025 uT / (dt_ms / 1000) */
        float dbdt = d_lsb * ((float)ADCS_MAG_NT_PER_LSB / 1000.0f) * 1000.0f / (float)dt_ms;
        dbdt_centi[i] = icd_sat_i16((int32_t)(dbdt * 100.0f));
        duty[i] = -((float)gain / 1000.0f) * dbdt;
        if (fabs_f(duty[i]) > maxabs) maxabs = fabs_f(duty[i]);
    }
    float scale = 1.0f;
    if (maxabs > (float)ADCS_DUTY_MAX) scale = (float)ADCS_DUTY_MAX / maxabs;
    for (int i = 0; i < 3; i++) {
        float v = duty[i] * scale;
        duty_out[i] = (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }
}
