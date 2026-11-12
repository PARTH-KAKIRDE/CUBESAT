/*
 * ADCS board configuration: sensor -> body axis mapping and coil polarity.
 *
 * The MPU-6050 (J12) and the MMC5983MA (J10) are breakout modules, so their
 * axes may not line up with the coil axes. Fix them here after the bench
 * check in docs/BRINGUP.md ("ADCS axis check"):
 *
 *   body[i] = SIGN[i] * sensor[MAP[i]]
 *
 * MTQ_SIGN flips a coil if positive duty produces a -axis dipole
 * (check with a compass: +duty on X should point the compass N pole to +X).
 */
#ifndef ADCS_BOARD_CONFIG_H
#define ADCS_BOARD_CONFIG_H

#define MAG_MAP    {0, 1, 2}
#define MAG_SIGN   {1, 1, 1}
#define GYRO_MAP   {0, 1, 2}
#define GYRO_SIGN  {1, 1, 1}
#define MTQ_SIGN   {1, 1, 1}

/* I2C1 sensor addresses */
#define MPU6050_ADDR    0x68  /* AD0 low (GY-521 default) */
#define MMC5983_ADDR    0x30

/* PWM: 16 MHz / 800 = 20 kHz, above hearing and fast vs. the coil L/R. */
#define MTQ_PWM_ARR     799u

#endif
