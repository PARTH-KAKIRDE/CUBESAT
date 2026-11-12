/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout).
 * Check value: icd_crc16("123456789", 9) == 0x29B1. */
#ifndef ICD_CRC16_H
#define ICD_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t icd_crc16(const uint8_t *data, size_t len);
uint16_t icd_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
