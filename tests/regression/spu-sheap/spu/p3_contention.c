/* sheap-p3-contention (SPU side): six of these run at once against a keyed
 * heap while the PPU also takes the shared mutex.
 *
 * Per thread:
 *   BarrierNew(key 7, 6 participants), Notify, Wait    -> all six lined up
 *   SemaphoreNew(key 6, 1)
 *   announce readiness (shared[READY] += 1), wait for shared[START]
 *   `rounds` times:
 *     MutexNew(key 5) (attaches to the PPU's mutex), Lock, shared[MUTEX] += 1,
 *     Unlock, MutexDelete
 *     P(semaphore), shared[SEMAPHORE] += 1, V(semaphore)
 *   SemaphoreDelete, BarrierDelete
 *
 * args: keyed heap EA, shared words EA (four words, one per 128-byte
 *       line), rounds, this thread's result block EA.
 * result: values[0] = rounds completed, values[1] = 1 once START was seen.
 */
#include <cell/atomic.h>
#include "common.h"

/* Word offsets into the PPU's shared block, one 128-byte line each. */
#define MUTEX_COUNTER     0
#define SEMAPHORE_COUNTER 128
#define READY             256
#define START             384

static uint32_t atomic_line[32] __attribute__((aligned(128)));

int main(uint64_t heap, uint64_t shared, uint64_t rounds, uint64_t ea_result)
{
    CellKeySheapBarrier barrier;
    CellKeySheapSemaphore semaphore;
    CellKeySheapMutex mutex;
    uint64_t i = 0;

    CHECK(1, cellKeySheapBarrierNew(&barrier, heap, 7, 6) == CELL_OK);
    if (!row_failed) {
        CHECK(2, cellKeySheapBarrierNotify(&barrier) == CELL_OK);
        CHECK(3, cellKeySheapBarrierWait(&barrier) == CELL_OK);
    }
    CHECK(4, cellKeySheapSemaphoreNew(&semaphore, heap, 6, 1) == CELL_OK);

    /* Tell the PPU this thread is ready, then wait for its go. */
    (void)cellAtomicIncr32(atomic_line, shared + READY);
    while (row_get32(shared + START) == 0)
        ;
    row_result[1] = 1;

    for (; i < rounds && !row_failed; ++i) {
        CHECK(5, cellKeySheapMutexNew(&mutex, heap, 5) == CELL_OK);
        if (row_failed)
            break;
        CHECK(6, cellKeySheapMutexLock(&mutex) == CELL_OK);
        row_put32(shared + MUTEX_COUNTER, row_get32(shared + MUTEX_COUNTER) + 1);
        CHECK(7, cellKeySheapMutexUnlock(&mutex) == CELL_OK);
        cellKeySheapMutexDelete(&mutex);

        cellKeySheapSemaphoreP(&semaphore);
        row_put32(shared + SEMAPHORE_COUNTER, row_get32(shared + SEMAPHORE_COUNTER) + 1);
        cellKeySheapSemaphoreV(&semaphore);
    }

    cellKeySheapSemaphoreDelete(&semaphore);
    cellKeySheapBarrierDelete(&barrier);
    row_result[0] = i;
    row_finish(ea_result);
    return 0;
}
