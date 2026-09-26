/* sheap-p3-contention (SPU side): six of these run at once against a keyed
 * heap while the PPU also takes the shared mutex.
 *
 * Per thread:
 *   BarrierNew(key 7, 6 participants), Notify, Wait    -> all six lined up
 *   SemaphoreNew(key 6, 1)
 *   `rounds` times:
 *     MutexNew(key 5) (attaches to the PPU's mutex), Lock, counter[0] += 1,
 *     Unlock, MutexDelete
 *     P(semaphore), counter[32] += 1, V(semaphore)
 *   SemaphoreDelete, BarrierDelete
 *
 * args: keyed heap EA, counters EA (two words 128 bytes apart), rounds,
 *       result block EA (thread 0 only, else 0).
 */
#include "common.h"

int main(uint64_t heap, uint64_t counters, uint64_t rounds, uint64_t ea_result)
{
    CellKeySheapBarrier barrier;
    CellKeySheapSemaphore semaphore;
    CellKeySheapMutex mutex;
    uint64_t i;

    CHECK(1, cellKeySheapBarrierNew(&barrier, heap, 7, 6) == CELL_OK);
    if (!row_failed) {
        CHECK(2, cellKeySheapBarrierNotify(&barrier) == CELL_OK);
        CHECK(3, cellKeySheapBarrierWait(&barrier) == CELL_OK);
    }
    CHECK(4, cellKeySheapSemaphoreNew(&semaphore, heap, 6, 1) == CELL_OK);

    for (i = 0; i < rounds && !row_failed; ++i) {
        CHECK(5, cellKeySheapMutexNew(&mutex, heap, 5) == CELL_OK);
        if (row_failed)
            break;
        CHECK(6, cellKeySheapMutexLock(&mutex) == CELL_OK);
        row_put32(counters, row_get32(counters) + 1);
        CHECK(7, cellKeySheapMutexUnlock(&mutex) == CELL_OK);
        cellKeySheapMutexDelete(&mutex);

        cellKeySheapSemaphoreP(&semaphore);
        row_put32(counters + 128, row_get32(counters + 128) + 1);
        cellKeySheapSemaphoreV(&semaphore);
    }

    cellKeySheapSemaphoreDelete(&semaphore);
    cellKeySheapBarrierDelete(&barrier);
    row_result[0] = i;
    row_finish(ea_result);
    return 0;
}
