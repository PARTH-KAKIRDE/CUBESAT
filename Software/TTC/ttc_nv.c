#include "ttc_nv.h"

#include <string.h>

#include "icd_crc16.h"
#include "ttc_hw.h"

#define ERASED   0xFFFFFFFFu
#define MAGIC    0x5Au
#define REC_LEN  8u

static uint16_t rec_crc(uint8_t key, uint32_t value)
{
    uint8_t b[5] = {key, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    return icd_crc16(b, sizeof b);
}

static bool append(ttc_nv_t *nv, uint8_t key, uint32_t value)
{
    if (!nv->ok || nv->next + REC_LEN > nv->size) return false;
    uint32_t at = nv->next;
    nv->next += REC_LEN; /* even a failed write uses the slot up: never write a word twice */
    uint32_t w0 = (uint32_t)MAGIC << 24 | (uint32_t)key << 16 | rec_crc(key, value);
    return ttc_hw_nv_program(at + 4u, value) && ttc_hw_nv_program(at, w0);
}

static void scan(ttc_nv_t *nv)
{
    memset(nv->value, 0, sizeof nv->value);
    nv->records = nv->torn = 0;
    uint32_t at = 0;
    for (; at + REC_LEN <= nv->size; at += REC_LEN) {
        uint32_t w0 = ttc_hw_nv_read(at), w1 = ttc_hw_nv_read(at + 4u);
        if (w0 == ERASED && w1 == ERASED) break;
        uint8_t key = (uint8_t)(w0 >> 16);
        if ((w0 >> 24) == MAGIC && key < TTC_NV_KEYS && (uint16_t)w0 == rec_crc(key, w1)) {
            nv->value[key] = w1;
            nv->records++;
        } else {
            nv->torn++;
        }
    }
    nv->next = at;
}

void ttc_nv_load(ttc_nv_t *nv)
{
    nv->size = ttc_hw_nv_size() & ~(REC_LEN - 1u);
    nv->ok = nv->size >= 4u * REC_LEN * TTC_NV_KEYS;
    if (!nv->ok) {
        memset(nv->value, 0, sizeof nv->value);
        nv->next = nv->size;
        return;
    }
    scan(nv);
    if (nv->next < nv->size / 4u * 3u) return;
    /* Three-quarters full: compact. A power cut between the erase and the
     * rewrite loses the values, so write the most important one first. */
    uint32_t keep[TTC_NV_KEYS];
    memcpy(keep, nv->value, sizeof keep);
    if (!ttc_hw_nv_erase()) {
        nv->ok = false;
        return;
    }
    nv->next = 0;
    static const uint8_t order[TTC_NV_KEYS] = {TTC_NV_ECMD_COUNTER, TTC_NV_DEPLOYED, TTC_NV_AUTO_ATTEMPTS,
                                               TTC_NV_OBC_WDT_MIN, TTC_NV_RESET_REPORT, TTC_NV_INIT};
    for (unsigned i = 0; i < TTC_NV_KEYS; i++)
        if (keep[order[i]] != 0 && !append(nv, order[i], keep[order[i]])) nv->ok = false;
    memcpy(nv->value, keep, sizeof keep);
}

bool ttc_nv_set(ttc_nv_t *nv, uint8_t key, uint32_t value)
{
    if (key >= TTC_NV_KEYS) return false;
    if (nv->value[key] == value) return true;
    nv->value[key] = value; /* RAM copy is used even if the flash write fails */
    return append(nv, key, value);
}
