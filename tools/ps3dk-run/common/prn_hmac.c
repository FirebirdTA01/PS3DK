/* ps3dk-run: HMAC-SHA-256 (RFC 2104 over FIPS 180-4), self-contained (no library dependency). */
#include "prn_hmac.h"

#include <string.h>

void prn_hmac_init(prn_hmac *h, const uint8_t *key, size_t keylen)
{
    uint8_t k[64], pad[64];
    memset(k, 0, sizeof k);
    if (keylen > 64) {
        prn_sha256 s;
        prn_sha256_init(&s);
        prn_sha256_update(&s, key, keylen);
        prn_sha256_final(&s, k);
    } else {
        memcpy(k, key, keylen);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    prn_sha256_init(&h->inner);
    prn_sha256_update(&h->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    prn_sha256_init(&h->outer);
    prn_sha256_update(&h->outer, pad, 64);
    memset(k, 0, sizeof k);
    memset(pad, 0, sizeof pad);
}

void prn_hmac_update(prn_hmac *h, const void *data, size_t n) { prn_sha256_update(&h->inner, data, n); }

void prn_hmac_final(prn_hmac *h, uint8_t out[32])
{
    uint8_t ih[32];
    prn_sha256_final(&h->inner, ih);
    prn_sha256_update(&h->outer, ih, 32);
    prn_sha256_final(&h->outer, out);
}

void prn_hmac4(const uint8_t key[32], const void *a, size_t an, const void *b, size_t bn, const void *c, size_t cn,
               const void *d, size_t dn, uint8_t out[32])
{
    prn_hmac h;
    prn_hmac_init(&h, key, 32);
    if (a && an) prn_hmac_update(&h, a, an);
    if (b && bn) prn_hmac_update(&h, b, bn);
    if (c && cn) prn_hmac_update(&h, c, cn);
    if (d && dn) prn_hmac_update(&h, d, dn);
    prn_hmac_final(&h, out);
}

int prn_tag_diff(const uint8_t a[32], const uint8_t b[32])
{
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= a[i] ^ b[i];
    return d != 0;
}
