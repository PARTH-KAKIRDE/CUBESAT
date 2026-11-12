/* B-dot detumble law (portable, unit-tested on the PC).
 *
 *   dB/dt = (B_k - B_{k-1}) / dt          both samples taken with coils off
 *   m     = -K * dB/dt                    one value per coil axis
 *
 * Units: B in mag LSB (0.025 uT), dt in ms, K in ADCS gain units
 * (duty permille per uT/s, x1000). Output duty is permille.
 * If any axis would exceed +-1000, all three are scaled down together so
 * the dipole keeps its direction. */
#ifndef BDOT_H
#define BDOT_H

#include <stdint.h>

/* dbdt_centi: dB/dt per axis in 0.01 uT/s (for telemetry). */
void bdot_compute(const int16_t b_now[3], const int16_t b_prev[3], uint32_t dt_ms,
                  uint32_t gain, int16_t duty_out[3], int16_t dbdt_centi[3]);

#endif
