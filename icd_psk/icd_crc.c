#include "icd_crc.h"
#include "icd.h"

/* Bitwise version: no table, so no flash cost, and fast enough for 32-byte frames. */
uint16_t icd_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = ICD_CRC_INIT;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ ICD_CRC_POLY) : (uint16_t)(crc << 1);
    }
    return crc;
}
