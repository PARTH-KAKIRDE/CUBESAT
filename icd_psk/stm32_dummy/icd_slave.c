/* Protocol logic for the dummy slave: answers PING and GET_VERSION.
 * No HAL calls here, so the same file builds on the STM32 and on a PC for tests. */
#include <string.h>

#include "icd.h"
#include "icd_crc.h"
#include "icd_slave.h"

/* Firmware version of this build. Real projects override these on the command line. */
#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 0
#endif

void icd_build_resp(uint8_t *tx, uint8_t cmd, uint8_t status,
                    const uint8_t *payload, uint8_t len)
{
    if (len > ICD_MAX_PAYLOAD)
        len = ICD_MAX_PAYLOAD;
    memset(tx, 0, ICD_RESP_LEN);
    tx[ICD_RESP_OFS_ID] = cmd;
    tx[ICD_RESP_OFS_STATUS] = status;
    tx[ICD_RESP_OFS_LEN] = len;
    if (len)
        memcpy(&tx[ICD_RESP_OFS_PAYLOAD], payload, len);
    uint16_t crc = icd_crc16(tx, ICD_RESP_HDR_LEN + len);
    tx[ICD_RESP_HDR_LEN + len] = (uint8_t)(crc & 0xFF);   /* low byte first */
    tx[ICD_RESP_HDR_LEN + len + 1] = (uint8_t)(crc >> 8);
}

int icd_slave_handle(const uint8_t *rx, size_t n, uint8_t *tx)
{
    uint8_t cmd = n ? rx[ICD_CMD_OFS_ID] : ICD_CMD_NONE;

    /* Length first: LEN tells us where the CRC is. */
    if (n < ICD_CMD_MIN_LEN || n > ICD_CMD_MAX_LEN
        || rx[ICD_CMD_OFS_LEN] > ICD_MAX_PAYLOAD
        || n != (size_t)(ICD_CMD_HDR_LEN + rx[ICD_CMD_OFS_LEN] + ICD_CRC_LEN)) {
        icd_build_resp(tx, cmd, ICD_ST_BAD_LENGTH, NULL, 0);
        return 0;
    }
    uint8_t len = rx[ICD_CMD_OFS_LEN];
    uint16_t got = (uint16_t)(rx[n - 2] | (rx[n - 1] << 8));
    if (icd_crc16(rx, n - ICD_CRC_LEN) != got) {
        icd_build_resp(tx, cmd, ICD_ST_BAD_CRC, NULL, 0);
        return 0;
    }

    switch (cmd) {
    case ICD_CMD_PING:
        icd_build_resp(tx, cmd, len ? ICD_ST_BAD_LENGTH : ICD_ST_OK, NULL, 0);
        break;
    case ICD_CMD_GET_VERSION:
        if (len) {
            icd_build_resp(tx, cmd, ICD_ST_BAD_LENGTH, NULL, 0);
        } else {
            const uint8_t v[ICD_GET_VERSION_RESP_LEN] = {
                FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH,
                ICD_VERSION_MAJOR, ICD_VERSION_MINOR};
            icd_build_resp(tx, cmd, ICD_ST_OK, v, sizeof v);
        }
        break;
    default:
        icd_build_resp(tx, cmd, ICD_ST_UNKNOWN_CMD, NULL, 0);
        break;
    }
    return 1;
}
