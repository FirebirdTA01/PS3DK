/* barrier.c -- SPU-side cellSyncBarrier* runtime.
 *
 * Implements Initialize/Notify/Wait/TryNotify/TryWait over MFC atomic
 * primitives (Cell BE Handbook, Chapter 19).
 *
 * Algorithm:
 *   Barrier is a single 32-bit word at ea containing {count, total_count}.
 *   - Initialize sets both fields via atomic store.
 *   - Notify atomically decrements count.  Does NOT re-arm -- this is
 *     a one-shot barrier; the caller re-initializes for next use.
 *   - Wait polls count until it reaches 0.
 *   - Try variants return AGAIN immediately on non-zero conditions
 *     rather than spinning.
 *
 * All atomic ops use a 128-byte aligned local-store lockline buffer
 * as required by the SPU MFC reservation granule.
 */

#include <stdint.h>
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cellstatus.h>
#include <cell/sync/barrier_types.h>

/* 128-byte aligned LS buffer for MFC reservation operations. */
static uint32_t __attribute__((aligned(128))) lockline[32];

int cellSyncBarrierInitialize(uint64_t ea, uint16_t count, unsigned int tag)
{
    CellSyncBarrier b;

    (void)tag;

    b.total_count = count;
    b.count = count;

    cellAtomicStore32(lockline, ea, b.uint_val);
    return CELL_OK;
}

int cellSyncBarrierNotify(uint64_t ea)
{
    CellSyncBarrier b;

    do {
        b.uint_val = cellAtomicLockLine32(lockline, ea);
        if (b.count > 0)
            b.count--;
    } while (!cellAtomicStoreConditional32(lockline, ea, b.uint_val));

    return CELL_OK;
}

int cellSyncBarrierTryNotify(uint64_t ea)
{
    CellSyncBarrier b;

    do {
        b.uint_val = cellAtomicLockLine32(lockline, ea);
        if (b.count == 0)
            return CELL_SYNC_ERROR_AGAIN;
        b.count--;
    } while (!cellAtomicStoreConditional32(lockline, ea, b.uint_val));

    return CELL_OK;
}

int cellSyncBarrierWait(uint64_t ea)
{
    CellSyncBarrier b;

    do {
        b.uint_val = cellAtomicLockLine32(lockline, ea);
    } while (b.count != 0);

    return CELL_OK;
}

int cellSyncBarrierTryWait(uint64_t ea)
{
    CellSyncBarrier b;

    b.uint_val = cellAtomicLockLine32(lockline, ea);
    if (b.count != 0)
        return CELL_SYNC_ERROR_AGAIN;

    return CELL_OK;
}
