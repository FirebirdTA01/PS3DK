/* Prime table written to main memory one 128-byte line (32 words) at a
 * time, shared by the allocate and key-buffer rows. */
#ifndef SPU_SHEAP_PRIMES_H
#define SPU_SHEAP_PRIMES_H

#include "common.h"

static int is_prime(uint32_t n)
{
    uint32_t d;

    if (n < 2)
        return 0;
    for (d = 2; d * d <= n; ++d)
        if (n % d == 0)
            return 0;
    return 1;
}

/* Write the first `count` primes (count a multiple of 32) to ea. */
static void write_primes(uint64_t ea, uint32_t count)
{
    static uint32_t line[32] __attribute__((aligned(128)));
    uint32_t n = 1, i;

    for (i = 0; i < count; ++i) {
        do
            ++n;
        while (!is_prime(n));
        line[i & 31] = n;
        if ((i & 31) == 31)
            row_put(line, ea + 4 * (i - 31), 128);
    }
}

#endif /* SPU_SHEAP_PRIMES_H */
