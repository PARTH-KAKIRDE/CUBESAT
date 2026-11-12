/* Glue between sim_api.c and the simulated hardware layers. */
#ifndef SIM_INTERNAL_H
#define SIM_INTERNAL_H

#include <stdint.h>

uint32_t sim_time_ms(void);

void adcs_sim_reset(uint32_t seed);
void adcs_sim_physics_step(double dt_s);

void ttc_sim_reset(void);
void ttc_sim_step(void);

#endif
