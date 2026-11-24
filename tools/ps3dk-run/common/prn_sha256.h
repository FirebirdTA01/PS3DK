/* ps3dk-run: SHA-256 (FIPS 180-4), self-contained (no library dependency). */
#ifndef PRN_SHA256_H
#define PRN_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} prn_sha256;

void prn_sha256_init(prn_sha256 *s);
void prn_sha256_update(prn_sha256 *s, const void *data, size_t n);
void prn_sha256_final(prn_sha256 *s, uint8_t out[32]);

#endif
