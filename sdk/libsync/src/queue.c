/*
 * queue.c -- SPU-side cellSyncQueue* runtime.
 *
 * Bounded FIFO ring buffer with MFC reservation-loop concurrency.
 * Descriptor (CellSyncQueue, 32 bytes, 128-byte aligned at ea_queue)
 * has a packed 64-bit head word: rlock(8) | index(24) | wlock(8) |
 * size(24).  Mutating operations use getllar/putllc on the full
 * 128-byte lockline; element-data DMA occurs after the head-word
 * reservation succeeds.
 *
 * Algorithm: Cell BE Handbook, Chapter 19 (MFC Atomic Update Commands).
 */

#include <stdint.h>
#include <string.h>
#include <spu_mfcio.h>
#include <spu_intrinsics.h>
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cellstatus.h>
#include <cell/sync/queue_types.h>

/* 128-byte aligned LS buffers for descriptor reservation + element DMA. */
static uint64_t __attribute__((aligned(128))) lockline[16];
static uint64_t __attribute__((aligned(128))) data_lockline[16];

/* --- helpers: read/write full descriptor lockline from/to EA --- */

static uint64_t read_head(uint64_t ea_queue)
{
    (void)cellAtomicLockLine64(lockline, ea_queue);
    return lockline[0];
}

static int write_head(uint64_t ea_queue, uint64_t head)
{
    lockline[0] = head;
    return cellAtomicStoreConditional64(lockline, ea_queue, head);
}

/* read immutable descriptor fields from the lockline (after getllar). */
static uint32_t read_depth(void)   { return (uint32_t)lockline[1]; }
static uint32_t read_bsize(void)   { return (uint32_t)(lockline[1] >> 32); }
static uint64_t read_buffer_ea(void) { return lockline[2]; }
static uint32_t read_tag(void)     { return (uint32_t)lockline[3]; }

/* --- MFC size validation --- */
/* Valid element sizes: 1, 2, 4, 8, 16, or multiples of 16 up to 128. */
static int mfc_legal_size(uint32_t sz)
{
    if (sz == 1 || sz == 2 || sz == 4 || sz == 8 || sz == 16) return 1;
    if (sz <= 128 && (sz & 15) == 0) return 1;
    return 0;
}

/* ================================================================
 * Public API
 * ================================================================ */

int cellSyncQueueInitialize(uint64_t ea, uint64_t ptr_buffer,
                            uint32_t buffer_size, unsigned int depth,
                            unsigned int tag)
{
    if ((ea & 0x7F) != 0 || (ptr_buffer & 0x7F) != 0)
        return CELL_SYNC_ERROR_ALIGN;
    if (tag > 31)
        return CELL_SYNC_ERROR_INVAL;
    if (depth == 0 || depth > 0xFFFFFFu)
        return CELL_SYNC_ERROR_INVAL;
    if (buffer_size > CELL_SYNC_QUEUE_MAX_ELEMENT_SIZE)
        return CELL_SYNC_ERROR_INVAL;
    if (!mfc_legal_size(buffer_size))
        return CELL_SYNC_ERROR_INVAL;

    /* Build the full descriptor in the lockline, then DMA the whole
     * 32 bytes to ea (32 is an MFC-legal transfer size).  Head is
     * atomically published via cellAtomicStore64 first so any
     * concurrent getllar sees a coherent head; the body fields are
     * read-only after init so no race window. */
    lockline[0] = 0;                     /* head: all-zero */
    lockline[1] = (uint64_t)buffer_size << 32 | depth;
    lockline[2] = ptr_buffer;
    lockline[3] = tag;

    cellAtomicStore64(lockline, ea, 0);

    mfc_put(lockline, ea, 32, (unsigned int)tag, 0, 0);
    mfc_write_tag_mask(1u << (unsigned int)tag);
    mfc_read_tag_status_all();

    return CELL_OK;
}

int cellSyncQueuePush(uint64_t ea, const void *buf)
{
    uint64_t head;
    uint8_t  rlock, wlock;
    uint32_t index, size;
    uint32_t depth, buffer_size, tag;
    uint64_t buffer_ea;

    for (;;) {
        head = read_head(ea);
        rlock = _cellSyncQueueGetRlock(head);
        index = _cellSyncQueueGetIndex(head);
        wlock = _cellSyncQueueGetWlock(head);
        size  = _cellSyncQueueGetSize(head);

        depth       = read_depth();
        buffer_size = read_bsize();
        buffer_ea   = read_buffer_ea();
        tag         = read_tag();

        if (size >= depth)   continue;   /* full -- spin */
        if (wlock != 0)      continue;   /* another writer holding lock */

        head = _cellSyncQueueMakeHead(rlock, index, 1, size);
        if (!write_head(ea, head)) continue;

        /* Reservation succeeded -- DMA element data. */
        {
            uint64_t slot_ea = buffer_ea
                             + (uint64_t)(index % depth) * buffer_size;
            memcpy(data_lockline, buf, buffer_size);
            mfc_put(data_lockline, slot_ea, buffer_size, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
        }

        /* Commit: advance index + size, release wlock. */
        for (;;) {
            head = read_head(ea);
            rlock = _cellSyncQueueGetRlock(head);
            index = _cellSyncQueueGetIndex(head);
            wlock = _cellSyncQueueGetWlock(head);
            size  = _cellSyncQueueGetSize(head);

            head = _cellSyncQueueMakeHead(rlock, index + 1, 0, size + 1);
            if (write_head(ea, head)) return CELL_OK;
        }
    }
}

int cellSyncQueueTryPush(uint64_t ea, const void *buf)
{
    uint64_t head;
    uint8_t  rlock, wlock;
    uint32_t index, size;
    uint32_t depth, buffer_size, tag;
    uint64_t buffer_ea;

    for (;;) {
        head = read_head(ea);
        rlock = _cellSyncQueueGetRlock(head);
        index = _cellSyncQueueGetIndex(head);
        wlock = _cellSyncQueueGetWlock(head);
        size  = _cellSyncQueueGetSize(head);

        depth       = read_depth();
        buffer_size = read_bsize();
        buffer_ea   = read_buffer_ea();
        tag         = read_tag();

        if (size >= depth)   return CELL_SYNC_ERROR_AGAIN;
        if (wlock != 0)      return CELL_SYNC_ERROR_AGAIN;

        head = _cellSyncQueueMakeHead(rlock, index, 1, size);
        if (!write_head(ea, head)) return CELL_SYNC_ERROR_AGAIN;

        /* DMA element data. */
        {
            uint64_t slot_ea = buffer_ea
                             + (uint64_t)(index % depth) * buffer_size;
            memcpy(data_lockline, buf, buffer_size);
            mfc_put(data_lockline, slot_ea, buffer_size, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
        }

        /* Commit. */
        for (;;) {
            head = read_head(ea);
            rlock = _cellSyncQueueGetRlock(head);
            index = _cellSyncQueueGetIndex(head);
            wlock = _cellSyncQueueGetWlock(head);
            size  = _cellSyncQueueGetSize(head);

            head = _cellSyncQueueMakeHead(rlock, index + 1, 0, size + 1);
            if (write_head(ea, head)) return CELL_OK;
        }
    }
}

int cellSyncQueuePop(uint64_t ea, void *buf)
{
    uint64_t head;
    uint8_t  rlock, wlock;
    uint32_t index, size;
    uint32_t depth, buffer_size, tag;
    uint64_t buffer_ea;

    for (;;) {
        head = read_head(ea);
        rlock = _cellSyncQueueGetRlock(head);
        index = _cellSyncQueueGetIndex(head);
        wlock = _cellSyncQueueGetWlock(head);
        size  = _cellSyncQueueGetSize(head);

        depth       = read_depth();
        buffer_size = read_bsize();
        buffer_ea   = read_buffer_ea();
        tag         = read_tag();

        if (size == 0)       continue;   /* empty -- spin */
        if (rlock != 0)      continue;   /* another reader holding lock */

        head = _cellSyncQueueMakeHead(1, index, wlock, size);
        if (!write_head(ea, head)) continue;

        /* DMA element data from ring buffer. */
        {
            uint32_t read_index = (index - size) % depth;
            uint64_t slot_ea = buffer_ea
                             + (uint64_t)read_index * buffer_size;
            mfc_get(data_lockline, slot_ea, buffer_size, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
            memcpy(buf, data_lockline, buffer_size);
        }

        /* Commit: decrement size, release rlock. */
        for (;;) {
            head = read_head(ea);
            rlock = _cellSyncQueueGetRlock(head);
            index = _cellSyncQueueGetIndex(head);
            wlock = _cellSyncQueueGetWlock(head);
            size  = _cellSyncQueueGetSize(head);

            head = _cellSyncQueueMakeHead(0, index, wlock, size - 1);
            if (write_head(ea, head)) return CELL_OK;
        }
    }
}

int cellSyncQueueTryPop(uint64_t ea, void *buf)
{
    uint64_t head;
    uint8_t  rlock, wlock;
    uint32_t index, size;
    uint32_t depth, buffer_size, tag;
    uint64_t buffer_ea;

    for (;;) {
        head = read_head(ea);
        rlock = _cellSyncQueueGetRlock(head);
        index = _cellSyncQueueGetIndex(head);
        wlock = _cellSyncQueueGetWlock(head);
        size  = _cellSyncQueueGetSize(head);

        depth       = read_depth();
        buffer_size = read_bsize();
        buffer_ea   = read_buffer_ea();
        tag         = read_tag();

        if (size == 0)       return CELL_SYNC_ERROR_AGAIN;
        if (rlock != 0)      return CELL_SYNC_ERROR_AGAIN;

        head = _cellSyncQueueMakeHead(1, index, wlock, size);
        if (!write_head(ea, head)) return CELL_SYNC_ERROR_AGAIN;

        /* DMA element data. */
        {
            uint32_t read_index = (index - size) % depth;
            uint64_t slot_ea = buffer_ea
                             + (uint64_t)read_index * buffer_size;
            mfc_get(data_lockline, slot_ea, buffer_size, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
            memcpy(buf, data_lockline, buffer_size);
        }

        /* Commit. */
        for (;;) {
            head = read_head(ea);
            rlock = _cellSyncQueueGetRlock(head);
            index = _cellSyncQueueGetIndex(head);
            wlock = _cellSyncQueueGetWlock(head);
            size  = _cellSyncQueueGetSize(head);

            head = _cellSyncQueueMakeHead(0, index, wlock, size - 1);
            if (write_head(ea, head)) return CELL_OK;
        }
    }
}

unsigned int cellSyncQueueSize(uint64_t ea)
{
    uint64_t head = read_head(ea);
    return _cellSyncQueueGetSize(head);
}

int cellSyncQueueClear(uint64_t ea)
{
    uint64_t head;
    uint8_t rlock, wlock;

    for (;;) {
        head  = read_head(ea);
        rlock = _cellSyncQueueGetRlock(head);
        wlock = _cellSyncQueueGetWlock(head);

        if (rlock != 0 || wlock != 0) continue;   /* wait for in-flight ops */

        if (write_head(ea, 0)) return CELL_OK;     /* zero index + size + locks */
    }
}

int cellSyncQueuePeek(uint64_t ea, void *buf)
{
    uint64_t head;
    uint8_t  rlock, wlock;
    uint32_t index, size;
    uint32_t depth, buffer_size, tag;
    uint64_t buffer_ea;

    for (;;) {
        head = read_head(ea);
        rlock = _cellSyncQueueGetRlock(head);
        index = _cellSyncQueueGetIndex(head);
        wlock = _cellSyncQueueGetWlock(head);
        size  = _cellSyncQueueGetSize(head);

        depth       = read_depth();
        buffer_size = read_bsize();
        buffer_ea   = read_buffer_ea();
        tag         = read_tag();

        if (size == 0)       continue;   /* empty -- spin */
        if (rlock != 0)      continue;   /* another reader holding lock */

        head = _cellSyncQueueMakeHead(1, index, wlock, size);
        if (!write_head(ea, head)) continue;

        /* DMA element data (no size decrement). */
        {
            uint32_t read_index = (index - size) % depth;
            uint64_t slot_ea = buffer_ea
                             + (uint64_t)read_index * buffer_size;
            mfc_get(data_lockline, slot_ea, buffer_size, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
            memcpy(buf, data_lockline, buffer_size);
        }

        /* Commit: release rlock, size unchanged. */
        for (;;) {
            head = read_head(ea);
            rlock = _cellSyncQueueGetRlock(head);
            index = _cellSyncQueueGetIndex(head);
            wlock = _cellSyncQueueGetWlock(head);
            size  = _cellSyncQueueGetSize(head);

            head = _cellSyncQueueMakeHead(0, index, wlock, size);
            if (write_head(ea, head)) return CELL_OK;
        }
    }
}

int cellSyncQueueTryPeek(uint64_t ea, void *buf)
{
    uint64_t head;
    uint8_t  rlock, wlock;
    uint32_t index, size;
    uint32_t depth, buffer_size, tag;
    uint64_t buffer_ea;

    for (;;) {
        head = read_head(ea);
        rlock = _cellSyncQueueGetRlock(head);
        index = _cellSyncQueueGetIndex(head);
        wlock = _cellSyncQueueGetWlock(head);
        size  = _cellSyncQueueGetSize(head);

        depth       = read_depth();
        buffer_size = read_bsize();
        buffer_ea   = read_buffer_ea();
        tag         = read_tag();

        if (size == 0)       return CELL_SYNC_ERROR_AGAIN;
        if (rlock != 0)      return CELL_SYNC_ERROR_AGAIN;

        head = _cellSyncQueueMakeHead(1, index, wlock, size);
        if (!write_head(ea, head)) return CELL_SYNC_ERROR_AGAIN;

        /* DMA element data. */
        {
            uint32_t read_index = (index - size) % depth;
            uint64_t slot_ea = buffer_ea
                             + (uint64_t)read_index * buffer_size;
            mfc_get(data_lockline, slot_ea, buffer_size, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
            memcpy(buf, data_lockline, buffer_size);
        }

        /* Commit: release rlock. */
        for (;;) {
            head = read_head(ea);
            rlock = _cellSyncQueueGetRlock(head);
            index = _cellSyncQueueGetIndex(head);
            wlock = _cellSyncQueueGetWlock(head);
            size  = _cellSyncQueueGetSize(head);

            head = _cellSyncQueueMakeHead(0, index, wlock, size);
            if (write_head(ea, head)) return CELL_OK;
        }
    }
}
