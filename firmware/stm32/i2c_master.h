/* Blocking I2C1 master for the local sensor bus: SCL = PB6, SDA = PB7 (AF4).
 * ADCS: MPU-6050 (0x68) + MMC5983MA (0x30). TT&C: TMP102 (0x48).
 * Every call has a timeout; the bus is recovered (9 clocks + STOP) on failure. */
#ifndef I2C_MASTER_H
#define I2C_MASTER_H

#include <stdbool.h>
#include <stdint.h>

void i2cm_init(void);
bool i2cm_write(uint8_t addr7, const uint8_t *data, uint8_t len);
bool i2cm_write_reg(uint8_t addr7, uint8_t reg, uint8_t value);
bool i2cm_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len);
uint16_t i2cm_error_count(void);

#endif
