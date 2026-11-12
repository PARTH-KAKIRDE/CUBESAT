/*
 * ADCS hardware interface. Everything above this line is portable C.
 *   hw_stm32.c - real board (STM32F446, MPU-6050, MMC5983MA, DRV8833 x3, 6 sun sensors)
 *   firmware/host/adcs_hw_sim.c - rigid-body simulation for PC tests
 *
 * All vectors are in the satellite BODY frame (+X, +Y, +Z = coil axes).
 * The sensor-to-body axis mapping lives in board_config.h, inside hw_stm32.c.
 */
#ifndef ADCS_HW_H
#define ADCS_HW_H

#include <stdbool.h>
#include <stdint.h>

bool adcs_hw_init(void);                 /* GPIO, PWM, ADC, sensor I2C bus */

bool adcs_hw_imu_init(void);             /* true if MPU-6050 found and configured */
/* gyro in raw LSB (65.5 LSB/dps, +-500 dps), temperature in 0.01 degC */
bool adcs_hw_imu_read(int16_t gyro[3], int16_t *temp_centi);

bool adcs_hw_mag_init(void);             /* true if MMC5983MA found */
bool adcs_hw_mag_start(void);            /* trigger one measurement (~8 ms) */
/* 1 = sample ready (mag in 0.025 uT LSB), 0 = not ready yet, -1 = error */
int adcs_hw_mag_poll(int16_t mag[3]);

void adcs_hw_mtq_enable(bool on);        /* DRV8833 nSLEEP for all three drivers */
void adcs_hw_mtq_set(const int16_t duty[3]); /* permille -1000..1000 per axis */
uint8_t adcs_hw_drv_faults(void);        /* bit0 X, bit1 Y, bit2 Z: nFAULT low */

bool adcs_hw_sun_read(uint16_t out[6]);  /* raw 12-bit ADC, SS1..SS6 */
uint16_t adcs_hw_local_i2c_errors(void);

#endif
