/* TT&C non-volatile flags: an append-only journal in one flash sector.
 *
 * Each record is two 32-bit words, written once into erased flash:
 *   w1 = value, then w0 = 0x5A | key | CRC-16 of (key, value)
 * The last good record of a key wins. A record cut short by a power loss
 * fails its CRC and is skipped. Nothing is ever erased while running: when
 * the sector is three-quarters full, ttc_nv_load() (boot, before the I2C
 * slave starts) erases it and writes back the current values. At a few
 * records per mission the 128 KB sector (16384 records) never fills.
 *
 * Used for what must survive a reset or power loss: the antenna-deployed flag
 * and burn count (automatic deployment), the emergency-command counter
 * (replay protection) and the OBC watchdog setting. */
#ifndef TTC_NV_H
#define TTC_NV_H

#include <stdbool.h>
#include <stdint.h>

#define TTC_NV_DEPLOYED       0   /* 1 = antenna confirmed deployed */
#define TTC_NV_AUTO_ATTEMPTS  1   /* automatic burns ever made */
#define TTC_NV_ECMD_COUNTER   2   /* last accepted emergency-command counter */
#define TTC_NV_OBC_WDT_MIN    3   /* OBC watchdog, minutes (0 = off) */
#define TTC_NV_RESET_REPORT   4   /* E_TTC_RESET to report after the reboot: 0x80000000 | ECMD seq */
#define TTC_NV_INIT           5   /* 1 once the store was ever written: absent = blank or wiped */
#define TTC_NV_KEYS           6

typedef struct {
    uint32_t value[TTC_NV_KEYS];
    uint32_t next;      /* byte offset of the next free record */
    uint32_t size;      /* sector size in bytes */
    bool ok;            /* the store can be written */
    uint16_t records;   /* good records found at load */
    uint16_t torn;      /* bad records skipped at load */
} ttc_nv_t;

void ttc_nv_load(ttc_nv_t *nv);
/* Store a value (no write if unchanged). False if the flash write failed. */
bool ttc_nv_set(ttc_nv_t *nv, uint8_t key, uint32_t value);
static inline uint32_t ttc_nv_get(const ttc_nv_t *nv, uint8_t key) { return nv->value[key]; }

#endif
