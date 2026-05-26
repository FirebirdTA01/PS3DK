/*
 * mutex.c -- SPU-side cellSyncMutex* runtime.
 *
 * Simple MFC reservation-loop mutex: a single uint64_t at ea_mutex
 * (128-byte aligned), 0 = free, 1 = held.  Single-owner CAS-acquire
 * with getllar/putllc; no owner tracking, no recursion.
 *
 * Algorithm: Cell BE Handbook, Chapter 19 (MFC Atomic Update Commands).
 */

#include <stdint.h>
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cellstatus.h>

static uint64_t __attribute__((aligned(128))) lockline[16];

int cellSyncMutexInitialize(uint64_t ea, unsigned int tag)
{
    (void)tag;

    if (ea & 0x7F)
        return CELL_SYNC_ERROR_ALIGN;

    cellAtomicStore64(lockline, ea, 0);
    return CELL_OK;
}

int cellSyncMutexLock(uint64_t ea)
{
    for (;;) {
        uint64_t state = cellAtomicLockLine64(lockline, ea);
        if (state != 0)
            continue;   /* held -- spin */
        if (cellAtomicStoreConditional64(lockline, ea, 1))
            return CELL_OK;
    }
}

int cellSyncMutexTryLock(uint64_t ea)
{
    uint64_t state = cellAtomicLockLine64(lockline, ea);
    if (state != 0)
        return CELL_SYNC_ERROR_AGAIN;
    if (!cellAtomicStoreConditional64(lockline, ea, 1))
        return CELL_SYNC_ERROR_AGAIN;
    return CELL_OK;
}

int cellSyncMutexUnlock(uint64_t ea)
{
    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        if (cellAtomicStoreConditional64(lockline, ea, 0))
            return CELL_OK;
    }
}
