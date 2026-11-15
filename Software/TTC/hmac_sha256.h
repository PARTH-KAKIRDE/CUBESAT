/* SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104). Portable C, no allocation.
 * TT&C uses it to check emergency commands (ground_link.h GL_T_ECMD), with
 * the same MAC rule as the OBC's ground/OBC commands. */
#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t buf[64];
    uint8_t n;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const uint8_t *d, size_t n);
void sha256_final(sha256_t *s, uint8_t out[SHA256_LEN]);

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_LEN]);

#endif
