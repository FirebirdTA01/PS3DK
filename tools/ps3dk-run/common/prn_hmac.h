/* ps3dk-run: HMAC-SHA-256 (RFC 2104 over FIPS 180-4), self-contained (no library dependency). */
#ifndef PRN_HMAC_H
#define PRN_HMAC_H

#include <stddef.h>
#include <stdint.h>

#include "prn_sha256.h"

typedef struct {
    prn_sha256 inner, outer;
} prn_hmac;

void prn_hmac_init(prn_hmac *h, const uint8_t *key, size_t keylen);
void prn_hmac_update(prn_hmac *h, const void *data, size_t n);
void prn_hmac_final(prn_hmac *h, uint8_t out[32]);
/* one-shot over the concatenation of up to four parts (NULL/0 parts skipped) */
void prn_hmac4(const uint8_t key[32], const void *a, size_t an, const void *b, size_t bn, const void *c, size_t cn,
               const void *d, size_t dn, uint8_t out[32]);
/* constant-time compare of two 32-byte tags: 0 when equal */
int prn_tag_diff(const uint8_t a[32], const uint8_t b[32]);

#endif
