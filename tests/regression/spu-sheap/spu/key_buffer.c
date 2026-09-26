/* sheap-key-buffer (SPU side): attach to the keyed buffer the PPU created,
 * fill it with primes and drop the reference.
 *
 * args: keyed heap EA, key, number of primes (multiple of 32).
 */
#include "primes.h"

int main(uint64_t heap, uint64_t key, uint64_t count, uint64_t unused)
{
    CellKeySheapBuffer buffer;

    (void)unused;
    /* Size 0: an attacher's size is its own argument, not the creator's. */
    CHECK(1, cellKeySheapBufferNew(&buffer, heap, (CellSheapKey)key, 0) == CELL_OK);
    if (!row_failed) {
        CHECK(2, cellKeySheapBufferGetSize(&buffer) == 0);
        CHECK(3, cellKeySheapBufferGetEa(&buffer) != 0);
        write_primes(cellKeySheapBufferGetEa(&buffer), (uint32_t)count);
        cellKeySheapBufferDelete(&buffer);
    }
    row_finish(0);
    return 0;
}
