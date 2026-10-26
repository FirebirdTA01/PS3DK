/* Built at -O3: GCC 16 vectorises the outer loop as an SLP store group of 16
   lanes fed by a splat of rows[y] (vectorisation factor 1).  Before patch
   0082 the load of rows[y] used the optimized explicit realignment: one lvsr
   mask computed for &rows, reused while the load moved 4 bytes per iteration,
   so rows 1 and 2 read the zeroed stack slot next to the table. */
#include <stdint.h>

__attribute__((noinline)) void vect_realign_sink(uint32_t *p, unsigned n)
{
    __asm__ volatile("" : : "r"(p), "r"(n) : "memory");
}

__attribute__((noinline)) void vect_realign_fill(uint32_t out[64])
{
    uint32_t a = 0, b = 0, c = 0;
    uint32_t storage[64];
    const uint32_t rows[] = {0x13ff0000, 0x1300ff00, 0x130000ff, 0x13ffff00};
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 16; ++x) storage[y * 16 + x] = rows[y];
    vect_realign_sink(&a, 1); vect_realign_sink(&b, 1); vect_realign_sink(&c, 1);
    vect_realign_sink(storage, 64);
    for (unsigned i = 0; i < 64; ++i) out[i] = storage[i];
}
