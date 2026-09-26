/* sheap-key-mutex (SPU side): four of these race to create the key-1
 * mutex, then each adds 1 to a PPU counter `increments` times under it.
 *
 * args: keyed heap EA, counter EA, increments.
 */
#include "common.h"

int main(uint64_t heap, uint64_t counter, uint64_t increments, uint64_t unused)
{
    CellKeySheapMutex mutex;
    uint64_t i;

    (void)unused;
    CHECK(1, cellKeySheapMutexNew(&mutex, heap, 1) == CELL_OK);
    for (i = 0; i < increments && !row_failed; ++i) {
        CHECK(2, cellKeySheapMutexLock(&mutex) == CELL_OK);
        row_put32(counter, row_get32(counter) + 1);
        CHECK(3, cellKeySheapMutexUnlock(&mutex) == CELL_OK);
    }
    if (row_failed != 1)
        cellKeySheapMutexDelete(&mutex);
    row_finish(0);
    return 0;
}
