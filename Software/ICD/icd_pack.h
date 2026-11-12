/* Little-endian field packing helpers. Use these instead of sending structs:
 * the wire format never depends on compiler padding or CPU endianness. */
#ifndef ICD_PACK_H
#define ICD_PACK_H

#include <stdint.h>

static inline void icd_put_u8(uint8_t *p, uint8_t v) { p[0] = v; }
static inline void icd_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void icd_put_i16(uint8_t *p, int16_t v) { icd_put_u16(p, (uint16_t)v); }
static inline void icd_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t icd_get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline int16_t icd_get_i16(const uint8_t *p) { return (int16_t)icd_get_u16(p); }
static inline uint32_t icd_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Saturating conversion helpers for scaled telemetry. */
static inline int16_t icd_sat_i16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}
static inline uint16_t icd_sat_u16(int32_t v)
{
    if (v > 65535) return 65535;
    if (v < 0) return 0;
    return (uint16_t)v;
}

#endif
