/* Exercise the private CRC against an independent bit-at-a-time definition;
 * including the implementation keeps this test out of the public API. */
#include "roll_store.c"
#include <stdio.h>
#include <stdlib.h>
static uint32_t bit_crc(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX; unsigned j;
    while (n--) {
        c ^= *p++;
        for (j = 0; j < 8; ++j) c = (c >> 1) ^ (UINT32_C(0xedb88320) & (0u - (c & 1u)));
    }
    return ~c;
}
static void check(const uint8_t *p, size_t n)
{
    if (crc32(p, n) != bit_crc(p, n)) { fprintf(stderr, "CRC mismatch length=%zu\n", n); exit(1); }
}
int main(void)
{
    size_t i; uint8_t *p = malloc(4194305); uint32_t r = 12345;
    if (!p) return 2;
    if (crc32((const uint8_t *)"123456789", 9) != UINT32_C(0xcbf43926)) return 1;
    for (i = 0; i < 256; ++i) { p[0] = (uint8_t)i; check(p, 1); }
    for (i = 0; i < 1048577; ++i) { r = r * UINT32_C(1664525) + UINT32_C(1013904223); p[i] = (uint8_t)(r >> 24); }
    for (i = 0; i <= 1024; ++i) check(p + 1, i);
    check(p, 4095); check(p + 1, 4096); check(p, 1048577);
    memset(p, 0, 1048577); check(p, 1048577);
    /* All small lengths, unaligned/nonzero prefixes, then both sides of every
     * power of two through the default slot size. Oracle processes real bytes. */
    memset(p, 0, 4194305);
    for (i = 0; i <= 257; ++i) {
        p[0] = (uint8_t)i;
        if (crc32_zeros(crc32(p, 1), i) != bit_crc(p, 1 + i)) return 1;
        p[0] = 0;
    }
    for (i = 1; i <= 4194304; i *= 2) {
        size_t n;
        p[0] = 0xd7;
        for (n = i - 1; n <= i; ++n)
            if (crc32_zeros(crc32(p, 1), n) != bit_crc(p, 1 + n)) return 1;
        p[0] = 0;
        if (crc32_zeros(0, i + 1) != bit_crc(p, i + 1)) return 1;
    }
    /* Independent zlib.crc32 streamed 1 MiB zero buffers after ASCII X.
     * Covers the high length bits without multi-GiB test allocations. */
    {
        const uint32_t lengths[] = {UINT32_C(2147483647), UINT32_C(2147483648),
                                    UINT32_C(2147483649), UINT32_MAX};
        const uint32_t expected[] = {UINT32_C(0xa0057139), UINT32_C(0x8da762f4),
                                     UINT32_C(0x685f7eea), UINT32_C(0xb7b2364b)};
        for (i = 0; i < 4; ++i)
            if (crc32_zeros(UINT32_C(0xb7b2364b), lengths[i]) != expected[i]) return 1;
    }
    free(p); puts("PASS store CRC: IEEE vector, all byte values, unaligned lengths, random and zero slack");
    return 0;
}
