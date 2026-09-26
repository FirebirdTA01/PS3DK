/*
 * mutex.c -- SPU-side cellSyncMutex* runtime.
 *
 * The mutex is the same 32-bit ticket word the PPU cellSyncMutex uses
 * (see mutex_ticket.h), so PPU and SPU code can share one mutex.  Every
 * change is a read-modify-write of the word under an MFC line
 * reservation (getllar / putllc); a PPU store to the word's 128-byte line
 * breaks the reservation, so updates from both sides are atomic with
 * respect to each other.
 *
 * The word needs 4-byte alignment.  Lock is FIFO-fair: waiters are
 * admitted in ticket order, whichever processor they run on.
 */

#include <stdint.h>
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cellstatus.h>
#include "mutex_ticket.h"

static uint32_t __attribute__((aligned(128))) lockline[32];

int cellSyncMutexInitialize(uint64_t ea, unsigned int tag)
{
    (void)tag;

    if (ea & 3)
        return CELL_SYNC_ERROR_ALIGN;

    (void)cellAtomicStore32(lockline, ea, 0);
    return CELL_OK;
}

int cellSyncMutexLock(uint64_t ea)
{
    uint32_t word;
    uint16_t ticket;

    if (ea & 3)
        return CELL_SYNC_ERROR_ALIGN;

    do {
        word = cellAtomicLockLine32(lockline, ea);
        ticket = __sync_mutex_next(word);
    } while (!cellAtomicStoreConditional32(lockline, ea,
                                           __sync_mutex_take_ticket(word)));

    /* Wait for our turn; only the holder's Unlock advances current. */
    while (__sync_mutex_current(cellAtomicLockLine32(lockline, ea)) != ticket)
        ;
    return CELL_OK;
}

int cellSyncMutexTryLock(uint64_t ea)
{
    uint32_t word;

    if (ea & 3)
        return CELL_SYNC_ERROR_ALIGN;

    do {
        word = cellAtomicLockLine32(lockline, ea);
        if (!__sync_mutex_is_free(word))
            return CELL_SYNC_ERROR_BUSY;
    } while (!cellAtomicStoreConditional32(lockline, ea,
                                           __sync_mutex_take_ticket(word)));
    return CELL_OK;
}

int cellSyncMutexUnlock(uint64_t ea)
{
    uint32_t word;

    if (ea & 3)
        return CELL_SYNC_ERROR_ALIGN;

    do {
        word = cellAtomicLockLine32(lockline, ea);
    } while (!cellAtomicStoreConditional32(lockline, ea,
                                           __sync_mutex_release(word)));
    return CELL_OK;
}
