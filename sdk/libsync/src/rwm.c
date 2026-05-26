/*
 * rwm.c -- SPU-side cellSyncRwm* runtime.
 *
 * Read-write mutex: multiple concurrent readers OR a single writer.
 * Descriptor (16 bytes, 128-byte aligned at ea_rwm) packs a uint32_t
 * lock word (rlock:16 in upper half, wlock:16 in lower half) plus
 * buffer metadata.  All mutating ops use MFC getllar/putllc on the
 * full 128-byte lockline; element-data DMA goes through a separate
 * aligned transfer buffer.
 *
 * Caller contract: every successful ReadBegin must be paired with
 * exactly one ReadEnd.  ReadEnd on rlock==0 returns
 * CELL_SYNC_ERROR_ABORT.  Nested ReadBegin from the same SPU thread
 * is not supported.
 *
 * Algorithm: Cell BE Handbook, Chapter 19 (MFC Atomic Update Commands).
 */

#include <stdint.h>
#include <string.h>
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cellstatus.h>

static uint64_t __attribute__((aligned(128))) lockline[16];
static uint64_t __attribute__((aligned(128))) xfer_buf[16];

/* MFC size check (same as queue.c). */
static int mfc_legal_size(uint32_t sz)
{
    if (sz == 1 || sz == 2 || sz == 4 || sz == 8 || sz == 16) return 1;
    if (sz <= 128 && (sz & 15) == 0) return 1;
    return 0;
}

/* --- helpers --- */

static uint32_t read_lock(void)
{
    return (uint32_t)(lockline[0] >> 32);
}

static void write_lock(uint32_t lock)
{
    lockline[0] = ((uint64_t)lock << 32) | (lockline[0] & 0xFFFFFFFFu);
}

int cellSyncRwmInitialize(uint64_t ea, uint64_t ptr_buffer,
                          uint32_t buffer_size, unsigned int tag)
{
    if ((ea & 0x7F) != 0 || (ptr_buffer & 0x7F) != 0)
        return CELL_SYNC_ERROR_ALIGN;
    if (tag > 31)
        return CELL_SYNC_ERROR_INVAL;
    if (buffer_size == 0 || buffer_size > 128)
        return CELL_SYNC_ERROR_INVAL;
    if (!mfc_legal_size(buffer_size))
        return CELL_SYNC_ERROR_INVAL;

    lockline[0] = ((uint64_t)0 << 32) | buffer_size;
    lockline[1] = ptr_buffer;

    cellAtomicStore64(lockline, ea, lockline[0]);

    mfc_put(&lockline[1], ea + 8, 8, (unsigned int)tag, 0, 0);
    mfc_write_tag_mask(1u << (unsigned int)tag);
    mfc_read_tag_status_all();

    return CELL_OK;
}

int cellSyncRwmReadBegin(uint64_t ea, void *buffer, unsigned int tag)
{
    uint32_t lock, rlock, wlock;
    uint32_t buffer_size;
    uint64_t buffer_ea;

    if (tag > 31)
        return CELL_SYNC_ERROR_INVAL;

    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        lock = read_lock();
        rlock = lock >> 16;
        wlock = lock & 0xFFFFu;

        if (wlock != 0)     continue;
        if (rlock == 0xFFFFu) continue;

        write_lock(((rlock + 1) << 16) | wlock);
        if (!cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            continue;

        buffer_size = (uint32_t)(lockline[0] & 0xFFFFFFFFu);
        buffer_ea   = lockline[1];

        mfc_get(xfer_buf, buffer_ea, buffer_size, tag, 0, 0);
        mfc_write_tag_mask(1u << tag);
        mfc_read_tag_status_all();
        memcpy(buffer, xfer_buf, buffer_size);
        return CELL_OK;
    }
}

int cellSyncRwmTryReadBegin(uint64_t ea, void *buffer, unsigned int tag)
{
    uint32_t lock, rlock, wlock;
    uint32_t buffer_size;
    uint64_t buffer_ea;

    if (tag > 31)
        return CELL_SYNC_ERROR_INVAL;

    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        lock = read_lock();
        rlock = lock >> 16;
        wlock = lock & 0xFFFFu;

        if (wlock != 0)     return CELL_SYNC_ERROR_AGAIN;
        if (rlock == 0xFFFFu) return CELL_SYNC_ERROR_AGAIN;

        write_lock(((rlock + 1) << 16) | wlock);
        if (!cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            return CELL_SYNC_ERROR_AGAIN;

        buffer_size = (uint32_t)(lockline[0] & 0xFFFFFFFFu);
        buffer_ea   = lockline[1];

        mfc_get(xfer_buf, buffer_ea, buffer_size, tag, 0, 0);
        mfc_write_tag_mask(1u << tag);
        mfc_read_tag_status_all();
        memcpy(buffer, xfer_buf, buffer_size);
        return CELL_OK;
    }
}

int cellSyncRwmReadEnd(uint64_t ea, unsigned int tag)
{
    uint32_t lock, rlock, wlock;

    (void)tag;

    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        lock  = read_lock();
        rlock = lock >> 16;
        wlock = lock & 0xFFFFu;

        if (rlock == 0)
            return CELL_SYNC_ERROR_ABORT;

        write_lock(((rlock - 1) << 16) | wlock);
        if (cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            return CELL_OK;
    }
}

int cellSyncRwmWrite(uint64_t ea, void *buffer, unsigned int tag)
{
    uint32_t lock, rlock, wlock;
    uint32_t buffer_size;
    uint64_t buffer_ea;

    if (tag > 31)
        return CELL_SYNC_ERROR_INVAL;

    /* acquire writer lock */
    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        lock  = read_lock();
        rlock = lock >> 16;
        wlock = lock & 0xFFFFu;

        if (rlock != 0 || wlock != 0)
            continue;

        write_lock(0 | 1);   /* rlock=0, wlock=1 */
        if (cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            break;
    }

    /* DMA write */
    buffer_size = (uint32_t)(lockline[0] & 0xFFFFFFFFu);
    buffer_ea   = lockline[1];
    memcpy(xfer_buf, buffer, buffer_size);
    mfc_put(xfer_buf, buffer_ea, buffer_size, tag, 0, 0);
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();

    /* release writer lock, preserving reader count */
    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        lock  = read_lock();
        rlock = lock >> 16;

        write_lock((rlock << 16) | 0);   /* wlock=0 */
        if (cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            return CELL_OK;
    }
}

int cellSyncRwmTryWrite(uint64_t ea, void *buffer, unsigned int tag)
{
    uint32_t lock, rlock, wlock;
    uint32_t buffer_size;
    uint64_t buffer_ea;

    if (tag > 31)
        return CELL_SYNC_ERROR_INVAL;

    /* try acquire writer lock */
    {
        (void)cellAtomicLockLine64(lockline, ea);
        lock  = read_lock();
        rlock = lock >> 16;
        wlock = lock & 0xFFFFu;

        if (rlock != 0 || wlock != 0)
            return CELL_SYNC_ERROR_AGAIN;

        write_lock(0 | 1);
        if (!cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            return CELL_SYNC_ERROR_AGAIN;
    }

    /* DMA write */
    buffer_size = (uint32_t)(lockline[0] & 0xFFFFFFFFu);
    buffer_ea   = lockline[1];
    memcpy(xfer_buf, buffer, buffer_size);
    mfc_put(xfer_buf, buffer_ea, buffer_size, tag, 0, 0);
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();

    /* release writer lock */
    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        lock  = read_lock();
        rlock = lock >> 16;

        write_lock((rlock << 16) | 0);
        if (cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            return CELL_OK;
    }
}
