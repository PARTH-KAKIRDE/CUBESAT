/*
 * ADCS board configuration: sensor -> body axis mapping and coil polarity.
 *
 * The MPU-6050 (J12) and the MMC5983MA (J10) are breakout modules, so their
 * axes may not line up with the coil axes. Fix them here after the bench
 * check in docs/BRINGUP.md (section 2, "Axis check"):
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

/* Sun sensors: BPW34 photodiodes, one per face, on connectors J4..J9 =
 * SS1..SS6 = PA0..PA5 (index 0..5 below). Each channel is an MCP6001
 * transimpedance stage (680 ohm), so counts are proportional to the light
 * on that face, i.e. to cos(angle to the Sun).
 *
 * UNVERIFIED placeholder mapping (ICD open issue 4): set at bring-up. */
#define SUN_CH_PX  0   /* SS1, J4: +X face */
#define SUN_CH_MX  1   /* SS2, J5: -X face */
#define SUN_CH_PY  2   /* SS3, J6: +Y face */
#define SUN_CH_MY  3   /* SS4, J7: -Y face */
#define SUN_CH_PZ  4   /* SS5, J8: +Z face */
/* -Z face (placeholder nadir side). Fitted since fw 0.5.0: with no pointing
 * control every face sees the Sun at times, and a bare face is a blind spot
 * that reads as eclipse. Earth albedo is rejected on all six faces by the
 * lit (albedo) threshold. A build without it: -DSUN_NO_MZ (target adcs_5face). */
#ifndef SUN_NO_MZ
#define SUN_CH_MZ  5   /* SS6, J9: -Z face */
#endif

/* I2C1 sensor addresses */
#define MPU6050_ADDR    0x68  /* AD0 low (GY-521 default) */
#define MMC5983_ADDR    0x30

/* PWM: 16 MHz / 800 = 20 kHz, above hearing and fast vs. the coil L/R. */
#define MTQ_PWM_ARR     799u

#endif
