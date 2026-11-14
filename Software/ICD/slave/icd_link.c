#include "icd_link.h"

#include <string.h>

#include "icd_crc16.h"
#include "icd_port.h"

uint8_t icd_build_response(uint8_t *out, uint8_t cmd, uint8_t status,
                           const uint8_t *payload, uint8_t len)
{
    if (len > ICD_MAX_PAYLOAD) len = ICD_MAX_PAYLOAD;
    memset(out, 0, ICD_RESP_FRAME_LEN);
    out[0] = cmd;
    out[1] = status;
    out[2] = len;
    if (len && payload) memcpy(&out[3], payload, len);
    uint16_t crc = icd_crc16(out, (size_t)(3 + len));
    out[3 + len] = (uint8_t)crc;
    out[4 + len] = (uint8_t)(crc >> 8);
    return (uint8_t)(ICD_RESP_HDR_LEN + len + ICD_CRC_LEN);
}

uint8_t icd_build_command(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    if (len > ICD_MAX_PAYLOAD) len = ICD_MAX_PAYLOAD;
    out[0] = cmd;
    out[1] = len;
    if (len && payload) memcpy(&out[2], payload, len);
    uint16_t crc = icd_crc16(out, (size_t)(2 + len));
    out[2 + len] = (uint8_t)crc;
    out[3 + len] = (uint8_t)(crc >> 8);
    return (uint8_t)(ICD_CMD_HDR_LEN + len + ICD_CRC_LEN);
}

void icd_link_init(icd_link_t *l, icd_dispatch_fn fn, void *ctx)
{
    memset(l, 0, sizeof(*l));
    l->dispatch = fn;
    l->dispatch_ctx = ctx;
    icd_build_response(l->tx_frame, ICD_CMD_ID_NONE, ICD_ST_NO_DATA, 0, 0);
    memcpy(l->tx_shadow, l->tx_frame, ICD_RESP_FRAME_LEN);
}

/* ---------------- ISR side: copy bytes only ---------------- */

void icd_link_isr_write_begin(icd_link_t *l)
{
    l->rx_len = 0;
    l->rx_overflow = 0;
    l->isr_activity++;
}

void icd_link_isr_rx_byte(icd_link_t *l, uint8_t b)
{
    if (l->rx_len < ICD_CMD_MAX_LEN)
        l->rx_buf[l->rx_len++] = b;
    else
        l->rx_overflow = 1;
    l->isr_activity++;
}

void icd_link_isr_write_end(icd_link_t *l)
{
    l->isr_activity++;
    if (l->rx_len == 0) return; /* address-only probe (e.g. i2cdetect) */
    if (l->pend_ready) l->stats.overwritten++;
    memcpy(l->pend_buf, l->rx_buf, l->rx_len);
    l->pend_len = l->rx_len;
    l->pend_overflow = l->rx_overflow;
    l->pend_ready = 1;
    /* Until the main loop has processed it, any read returns BUSY. */
    icd_build_response(l->tx_frame, l->rx_buf[0], ICD_ST_BUSY, 0, 0);
}

void icd_link_isr_read_begin(icd_link_t *l)
{
    /* The last command's bytes are already handed over (pend_buf). Forget
     * them here, so that a stray write_end after this read (a driver seeing
     * STOP after the master's NACK) finds rx_len == 0 and does nothing,
     * instead of queueing the old command a second time. */
    l->rx_len = 0;
    l->rx_overflow = 0;
    memcpy(l->tx_shadow, l->tx_frame, ICD_RESP_FRAME_LEN);
    l->tx_idx = 0;
    l->isr_activity++;
    l->stats.reads++;
}

uint8_t icd_link_isr_tx_byte(icd_link_t *l)
{
    l->isr_activity++;
    if (l->tx_idx < ICD_RESP_FRAME_LEN) return l->tx_shadow[l->tx_idx++];
    return 0x00; /* master read past 32 bytes: pad */
}

void icd_link_isr_read_end(icd_link_t *l)
{
    l->tx_idx = 0;
    l->isr_activity++;
}

void icd_link_isr_bus_reset(icd_link_t *l)
{
    l->rx_len = 0;
    l->rx_overflow = 0;
    l->tx_idx = 0;
    l->stats.bus_resets++;
}

/* ---------------- main loop side ---------------- */

int icd_link_poll(icd_link_t *l)
{
    uint8_t cmd[ICD_CMD_MAX_LEN];
    uint8_t len, overflow;

    uint32_t s = icd_port_lock();
    if (!l->pend_ready) {
        icd_port_unlock(s);
        return 0;
    }
    len = l->pend_len;
    overflow = l->pend_overflow;
    memcpy(cmd, l->pend_buf, len);
    l->pend_ready = 0;
    icd_port_unlock(s);

    uint8_t frame[ICD_RESP_FRAME_LEN];
    uint8_t resp[ICD_MAX_PAYLOAD];
    uint8_t resp_len = 0;
    uint8_t status;
    int result;

    /* Never act on a frame with a bad length or bad CRC. */
    if (overflow || len < ICD_CMD_MIN_LEN || cmd[1] > ICD_MAX_PAYLOAD ||
        len != (uint8_t)(cmd[1] + ICD_CMD_HDR_LEN + ICD_CRC_LEN)) {
        status = ICD_ST_BAD_LENGTH;
        l->stats.length_errors++;
        result = -1;
    } else {
        uint8_t plen = cmd[1];
        uint16_t rx_crc = (uint16_t)(cmd[2 + plen] | (cmd[3 + plen] << 8));
        if (icd_crc16(cmd, (size_t)(2 + plen)) != rx_crc) {
            status = ICD_ST_BAD_CRC;
            l->stats.crc_errors++;
            result = -1;
        } else {
            l->stats.frames_ok++;
            status = l->dispatch(l->dispatch_ctx, cmd[0], &cmd[2], plen, resp, &resp_len);
            if (status != ICD_ST_OK || resp_len > ICD_MAX_PAYLOAD) resp_len = 0;
            result = 1;
        }
    }

    icd_build_response(frame, cmd[0], status, resp, resp_len);

    s = icd_port_lock();
    /* If another command arrived while we were working, our answer is stale:
     * keep the BUSY frame the ISR staged for the new command. */
    if (!l->pend_ready) memcpy(l->tx_frame, frame, ICD_RESP_FRAME_LEN);
    icd_port_unlock(s);
    return result;
}
