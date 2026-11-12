/*
 * icd_link - I2C slave framing for the OBC link (portable, no hardware access).
 *
 * The I2C driver calls the icd_link_isr_* functions from its interrupt.
 * Those functions only copy bytes. The main loop calls icd_link_poll(),
 * which validates the frame (length, CRC), calls the dispatcher, and stages
 * the 32-byte response.
 *
 * Response staging:
 *   - At boot the response is an idle frame (CMD 0x00, STATUS NO_DATA).
 *   - When a command write completes, the ISR stages a BUSY frame that echoes
 *     the command ID, so a read that arrives before processing gets BUSY.
 *   - After processing, the real response replaces BUSY.
 *   - The response stays until the next command. Re-reading returns the same
 *     frame, so the master may re-read after a CRC error without re-sending
 *     (important for commands that must not run twice).
 *   - At the start of each read the ISR copies the frame to a shadow buffer,
 *     so the main loop can never change a frame halfway through a read.
 */
#ifndef ICD_LINK_H
#define ICD_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "cubesat_icd.h"

/* Dispatcher: handle one validated command.
 * req/req_len: payload. resp: room for ICD_MAX_PAYLOAD bytes.
 * Return an ICD_ST_* code. Set *resp_len (0 on error). */
typedef uint8_t (*icd_dispatch_fn)(void *ctx, uint8_t cmd, const uint8_t *req,
                                   uint8_t req_len, uint8_t *resp, uint8_t *resp_len);

typedef struct {
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t length_errors;
    uint32_t overwritten;   /* a new command arrived before the last was processed */
    uint32_t reads;
    uint32_t bus_resets;    /* peripheral re-initialised by the driver */
} icd_link_stats_t;

typedef struct {
    /* ISR-owned receive state */
    uint8_t rx_buf[ICD_CMD_MAX_LEN];
    uint8_t rx_len;
    uint8_t rx_overflow;
    /* ISR -> main loop hand-over */
    uint8_t pend_buf[ICD_CMD_MAX_LEN];
    uint8_t pend_len;
    uint8_t pend_overflow;
    volatile uint8_t pend_ready;
    /* Staged response (written in ISR or under lock) and ISR read shadow */
    uint8_t tx_frame[ICD_RESP_FRAME_LEN];
    uint8_t tx_shadow[ICD_RESP_FRAME_LEN];
    uint8_t tx_idx;
    /* Activity counter for the driver's stuck-bus watchdog */
    volatile uint32_t isr_activity;

    icd_dispatch_fn dispatch;
    void *dispatch_ctx;
    icd_link_stats_t stats;
} icd_link_t;

void icd_link_init(icd_link_t *l, icd_dispatch_fn fn, void *ctx);

/* --- called from the I2C interrupt --- */
void icd_link_isr_write_begin(icd_link_t *l);            /* ADDR matched, master writes */
void icd_link_isr_rx_byte(icd_link_t *l, uint8_t b);
void icd_link_isr_write_end(icd_link_t *l);              /* STOP after a write */
void icd_link_isr_read_begin(icd_link_t *l);             /* ADDR matched, master reads */
uint8_t icd_link_isr_tx_byte(icd_link_t *l);             /* next byte to send */
void icd_link_isr_read_end(icd_link_t *l);               /* master NACK / STOP */
void icd_link_isr_bus_reset(icd_link_t *l);              /* driver reset the peripheral */

/* --- called from the main loop --- */
/* Process a pending command. Returns 1 if a frame was valid (CRC + length OK),
 * 0 if nothing was pending, -1 if a frame was rejected. */
int icd_link_poll(icd_link_t *l);

/* Build a response frame (used internally and by tests). Returns frame length
 * before padding (hdr + payload + crc). out must hold ICD_RESP_FRAME_LEN. */
uint8_t icd_build_response(uint8_t *out, uint8_t cmd, uint8_t status,
                           const uint8_t *payload, uint8_t len);

/* Build a command frame (used by the host tests and the dummy master).
 * Returns total length. out must hold ICD_CMD_MAX_LEN. */
uint8_t icd_build_command(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t len);

#endif
