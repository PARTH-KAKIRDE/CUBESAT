#ifndef ICD_SLAVE_H
#define ICD_SLAVE_H

#include <stddef.h>
#include <stdint.h>

/* ---- protocol logic: plain C, no HAL, unit-testable on a PC ---- */

/* Fill a full 32-byte response frame (header, payload, CRC, zero padding). */
void icd_build_resp(uint8_t *tx, uint8_t cmd, uint8_t status,
                    const uint8_t *payload, uint8_t len);

/* Validate one received command frame (n bytes) and build its response in tx.
 * Returns 1 if the frame had a good length and CRC (counts as master contact),
 * 0 if it was discarded. A discarded frame is never acted on. */
int icd_slave_handle(const uint8_t *rx, size_t n, uint8_t *tx);

/* ---- HAL glue (icd_slave_it.c), STM32 only ---- */
#ifdef HAL_I2C_MODULE_ENABLED
void icd_slave_init(I2C_HandleTypeDef *hi2c);
void icd_slave_poll(void);           /* call from the main loop as often as possible */
int  icd_slave_no_master_contact(void);
#endif

#endif /* ICD_SLAVE_H */
