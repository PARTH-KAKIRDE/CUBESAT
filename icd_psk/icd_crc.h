#ifndef ICD_CRC_H
#define ICD_CRC_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/CCITT-FALSE over len bytes. icd_crc16("123456789", 9) == 0x29B1. */
uint16_t icd_crc16(const uint8_t *data, size_t len);

#endif /* ICD_CRC_H */
