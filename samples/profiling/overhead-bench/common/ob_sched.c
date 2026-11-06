#include "ob_sched.h"

uint64_t ob_rng(uint64_t *x)
{
    if (*x == 0)
        *x = 0x9E3779B97F4A7C15ull;
    *x ^= *x >> 12;
    *x ^= *x << 25;
    *x ^= *x >> 27;
    return *x * 0x2545F4914F6CDD1Dull;
}

/* Fisher-Yates over [a, a+n) with an unbiased bound (rejection) */
static void shuffle(ob_block *a, unsigned long n, uint64_t *st)
{
    for (unsigned long i = n; i > 1; --i) {
        uint64_t lim = UINT64_MAX - UINT64_MAX % i, v;
        do
            v = ob_rng(st);
        while (v >= lim);
        unsigned long j = (unsigned long)(v % i);
        ob_block t = a[i - 1];
        a[i - 1] = a[j];
        a[j] = t;
    }
}

long ob_schedule(uint64_t seed, const uint16_t *cases, const int *cold, unsigned ncases, unsigned r, ob_block *out,
                 unsigned long cap)
{
    unsigned long need = 0, k = 0;
    for (unsigned c = 0; c < ncases; ++c)
        need += cold[c] ? (unsigned long)OB_TUS * OB_COLD_SITES : (unsigned long)OB_TUS * (1u + r);
    if (need > cap)
        return -1;
    uint64_t st = seed;
    for (unsigned c = 0; c < ncases; ++c) /* the warmups */
        if (!cold[c])
            for (unsigned t = 0; t < OB_TUS; ++t) {
                ob_block b = {cases[c], (uint8_t)t, OB_BLK_WARM, 0};
                out[k++] = b;
            }
    unsigned long nwarm = k;
    shuffle(out, nwarm, &st);
    for (unsigned c = 0; c < ncases; ++c)
        for (unsigned t = 0; t < OB_TUS; ++t) {
            unsigned m = cold[c] ? OB_COLD_SITES : r;
            for (unsigned i = 0; i < m; ++i) {
                ob_block b = {cases[c], (uint8_t)t, (uint8_t)(cold[c] ? OB_BLK_COLD : OB_BLK_TIMED), (uint16_t)i};
                out[k++] = b;
            }
        }
    shuffle(out + nwarm, k - nwarm, &st);
    return (long)k;
}

uint64_t ob_sched_digest(const ob_block *b, unsigned long n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned long i = 0; i < n; ++i) {
        uint32_t v[4] = {b[i].c, b[i].tu, b[i].kind, b[i].rep};
        for (int j = 0; j < 4; ++j)
            for (int q = 0; q < 4; ++q) {
                h ^= (v[j] >> (8 * q)) & 0xFFu;
                h *= 0x100000001B3ull;
            }
    }
    return h;
}

long ob_gpu_schedule(uint64_t seed, unsigned r, ob_block *out, unsigned long cap)
{
    unsigned long need = 1ul + (unsigned long)OB_G_CASES * r, k = 0;
    if (need > cap)
        return -1;
    uint64_t st = seed ^ OB_GPU_SALT;
    ob_block w = {OB_G_G1, 0, OB_BLK_WARM, 0};
    out[k++] = w;
    for (unsigned c = 0; c < OB_G_CASES; ++c)
        for (unsigned i = 0; i < r; ++i) {
            ob_block b = {(uint16_t)c, 0, OB_BLK_TIMED, (uint16_t)i};
            out[k++] = b;
        }
    shuffle(out + 1, k - 1, &st);
    return (long)k;
}
