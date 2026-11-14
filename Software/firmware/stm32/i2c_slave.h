/* I2C3 slave for the OBC link: SCL = PA8 (AF4), SDA = PC9 (AF4).
 * Same pins on ADCS, TT&C and the Nucleo-F446RE (D7 / CN10-1).
 * The ISR only moves bytes in and out of the icd_link buffers. */
#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>
#include "icd_link.h"

void i2c_slave_init(icd_link_t *link, uint8_t addr7);

/* Call from the main loop every pass. Re-initialises the peripheral if the
 * bus has been busy with no ISR progress for 25 ms (stuck clock/data). */
void i2c_slave_watchdog(uint32_t now_ms);

/* Number of times the peripheral was reset (bus error or watchdog). */
uint32_t i2c_slave_reset_count(void);

#endif
