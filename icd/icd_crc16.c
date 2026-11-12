#include "icd_crc16.h"
#include "cubesat_icd.h"

/* Bitwise implementation: small, no table, fast enough for 32-byte frames. */
uint16_t icd_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u)
                crc = (uint16_t)((crc << 1) ^ ICD_CRC_POLY);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t icd_crc16(const uint8_t *data, size_t len)
{
    return icd_crc16_update(ICD_CRC_INIT, data, len);
}
