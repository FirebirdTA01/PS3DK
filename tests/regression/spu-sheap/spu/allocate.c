/* sheap-allocate (SPU side): allocate a work area from a heap the PPU
 * initialised, build a prime table in it, read the table back and return
 * its last entry.
 *
 * args: heap EA, number of primes (multiple of 32), answer word EA.
 */
#include "primes.h"

static uint32_t back[32] __attribute__((aligned(128)));

int main(uint64_t heap, uint64_t count, uint64_t ea_answer, uint64_t unused)
{
    uint64_t work;
    uint32_t i, last = 0;

    (void)unused;
    work = cellSheapAllocate(heap, count * 4);
    CHECK(1, work != 0);
    if (!row_failed) {
        write_primes(work, (uint32_t)count);
        for (i = 0; i < count; i += 32) {
            row_get(back, work + 4 * i, 128);
            last = back[31];
        }
        row_put32(ea_answer, last);
        CHECK(2, cellSheapFree(heap, work) == CELL_OK);
    }
    row_finish(0);
    return 0;
}
