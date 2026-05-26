/*
 * lfqueue.c -- SPU-side cellSyncLFQueue* runtime.
 *
 * Lock-free bounded MPMC queue for SPU<->PPU communication using
 * per-slot sequence counters (Vyukov 2008, "Bounded MPMC Queue").
 *
 * Descriptor (128 bytes, 128B-aligned at ea) holds uint32_t head,
 * tail, push_commit, pop_commit, plus immutable metadata.  Per-slot
 * uint32_t sequence counters live in a separate array at the end of
 * the caller-allocated buffer.  All seq DMA is performed OUTSIDE
 * descriptor getllar/putllc reservation windows.
 *
 * Algorithm: Cell BE Handbook Ch. 19 + Vyukov MPMC with MFC atomics.
 */

#include <stdint.h>
#include <string.h>
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cellstatus.h>

static uint64_t __attribute__((aligned(128))) lockline[16];
static uint64_t __attribute__((aligned(128))) xfer_buf[16];
static uint32_t __attribute__((aligned(128))) seq_buf[32];

/* Write a sequence array: seq[i] = base + i for i in [0, count).
 * seq_base must be 16-byte aligned (count * 4 bytes total). */
static void write_seq_array(uint64_t seq_base, uint32_t base_val,
                            unsigned int count, unsigned int tag)
{
    unsigned int chunk, i, rem, sz;
    uint64_t ea;

    for (chunk = 0; chunk < count / 32; chunk++) {
        for (i = 0; i < 32; i++)
            seq_buf[i] = base_val + chunk * 32 + i;
        mfc_put(seq_buf, seq_base + chunk * 128, 128, tag, 0, 0);
        mfc_write_tag_mask(1u << tag);
        mfc_read_tag_status_all();
    }

    rem = count % 32;
    if (rem) {
        for (i = 0; i < rem; i++)
            seq_buf[i] = base_val + (count / 32) * 32 + i;
        ea = seq_base + (count / 32) * 128;
        sz = rem * 4;

        /* Write in MFC-legal chunks: 16-byte multiples then 8 or 4. */
        if (sz >= 16) {
            unsigned int large = (sz / 16) * 16;
            mfc_put(seq_buf, ea, large, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
            sz -= large;
            ea += large;
        }
        if (sz == 8) {
            mfc_put((char *)seq_buf + (rem - 2) * 4, ea, 8, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
        } else if (sz == 4) {
            mfc_put((char *)seq_buf + (rem - 1) * 4, ea, 4, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
        } else if (sz == 12) {
            /* 12 = 8 + 4 */
            mfc_put((char *)seq_buf + (rem - 3) * 4, ea, 8, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
            mfc_put((char *)seq_buf + (rem - 1) * 4, ea + 8, 4, tag, 0, 0);
            mfc_write_tag_mask(1u << tag);
            mfc_read_tag_status_all();
        }
    }
}

/* MFC size validation (same as queue/rwm). */
static int mfc_legal_size(uint32_t sz)
{
    if (sz == 1 || sz == 2 || sz == 4 || sz == 8 || sz == 16) return 1;
    if (sz <= 128 && (sz & 15) == 0) return 1;
    return 0;
}

/* 4-byte sequence read/write helpers.
 * Use 128B-aligned LS buffer so MFC 4-byte transfers are legal. */

static void put_u32(uint32_t val, uint64_t ea, unsigned int tag)
{
    static uint32_t __attribute__((aligned(128))) wb;
    wb = val;
    mfc_put(&wb, ea, 4, tag, 0, 0);
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

static uint32_t get_u32(uint64_t ea, unsigned int tag)
{
    static uint32_t __attribute__((aligned(128))) rb;
    mfc_get(&rb, ea, 4, tag, 0, 0);
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
    return rb;
}

/* Signal increment for PPU notification. */
static void signal_inc(uint64_t ea_signal)
{
    static uint32_t __attribute__((aligned(128))) sig_ll[32];
    for (;;) {
        uint32_t v = cellAtomicLockLine32(sig_ll, ea_signal);
        if (cellAtomicStoreConditional32(sig_ll, ea_signal, v + 1))
            break;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

int cellSyncLFQueueInitialize(uint64_t ea, uint64_t buffer,
                              unsigned int size, unsigned int depth,
                              CellSyncQueueDirection direction,
                              uint64_t eaSignal)
{
    unsigned int i;

    if ((ea & 0x7F) != 0 || (buffer & 0x7F) != 0)
        return CELL_SYNC_ERROR_ALIGN;
    if (eaSignal != 0 && (eaSignal & 3) != 0)
        return CELL_SYNC_ERROR_ALIGN;
    if (size == 0 || size > 128 || !mfc_legal_size(size))
        return CELL_SYNC_ERROR_INVAL;
    if (depth == 0)
        return CELL_SYNC_ERROR_INVAL;
    if (direction > 3)
        return CELL_SYNC_ERROR_INVAL;
    if (direction == CELL_SYNC_QUEUE_ANY2ANY)
        return CELL_SYNC_ERROR_NOSYS;

    /* Build descriptor. */
    for (i = 0; i < 16; i++) lockline[i] = 0;
    lockline[0] = 0;  /* head=0, tail=0 */
    lockline[1] = 0;  /* push_commit=0, pop_commit=0 */
    lockline[2] = ((uint64_t)size << 32) | depth;
    lockline[3] = buffer;
    lockline[4] = (uint64_t)direction << 32;
    lockline[5] = eaSignal;
    lockline[8] = 0;  /* state=READY */

    mfc_put(lockline, ea, 128, 31, 0, 0);
    mfc_write_tag_mask(1u << 31);
    mfc_read_tag_status_all();

    /* Init per-slot sequences: seq[i] = i. */
    {
        uint64_t seq_base = (buffer + (uint64_t)depth * size + 15) & ~15ULL;
        write_seq_array(seq_base, 0, depth, 31);
    }

    return CELL_OK;
}

int cellSyncLFQueueSize(uint64_t ea, unsigned int *size)
{
    (void)cellAtomicLockLine64(lockline, ea);
    *size = (unsigned int)((uint32_t)(lockline[1] >> 32) -
                           (uint32_t)(lockline[1] & 0xFFFFFFFFu));
    return CELL_OK;
}

int cellSyncLFQueueDepth(uint64_t ea, unsigned int *depth)
{
    (void)cellAtomicLockLine64(lockline, ea);
    *depth = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
    return CELL_OK;
}

int cellSyncLFQueueGetDirection(uint64_t ea, CellSyncQueueDirection *direction)
{
    (void)cellAtomicLockLine64(lockline, ea);
    *direction = (CellSyncQueueDirection)(lockline[4] >> 32);
    return CELL_OK;
}

int cellSyncLFQueueGetEntrySize(uint64_t ea, unsigned int *pSize)
{
    (void)cellAtomicLockLine64(lockline, ea);
    *pSize = (unsigned int)(lockline[2] >> 32);
    return CELL_OK;
}

int _cellSyncLFQueueGetSignalAddress(uint64_t ea, uint64_t *pSignal)
{
    (void)cellAtomicLockLine64(lockline, ea);
    *pSignal = lockline[5];
    return CELL_OK;
}

int cellSyncLFQueueClear(uint64_t ea)
{
    unsigned int depth;

    /* Acquire: set state=CLEARING after verifying no in-flight ops. */
    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        if ((uint32_t)(lockline[8] & 0xFFFFFFFFu) != 0) continue;
        if ((uint32_t)(lockline[0] >> 32) != (uint32_t)(lockline[1] >> 32))
            continue;
        if ((uint32_t)(lockline[0] & 0xFFFFFFFFu) !=
            (uint32_t)(lockline[1] & 0xFFFFFFFFu))
            continue;
        depth = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
        lockline[8] = 1;  /* CLEARING */
        if (cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            break;
    }

    /* Re-init sequence array (OUTSIDE descriptor reservation). */
    {
        uint64_t seq_base =
            (lockline[3] + (uint64_t)depth * (uint32_t)(lockline[2] >> 32) + 15)
            & ~15ULL;
        write_seq_array(seq_base, 0, depth, 31);
    }

    /* Release: reset counters, set state=READY. */
    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        if ((uint32_t)(lockline[8] & 0xFFFFFFFFu) != 1)
            return CELL_SYNC_ERROR_ABORT;
        lockline[0] = 0;
        lockline[1] = 0;
        lockline[8] = 0;
        if (cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            break;
    }

    return CELL_OK;
}

int _cellSyncLFQueuePushBeginBody(uint64_t ea,
                                  CellSyncLFQueuePushContainer *pContainer,
                                  unsigned int isBlocking)
{
    uint32_t head, pop_commit;
    uint32_t entry_size, depth;
    uint64_t buffer_ea, seq_base;
    uint32_t pos;
    unsigned int tag;

    tag = pContainer->tag;
    if (tag > 31) return CELL_SYNC_ERROR_INVAL;

    for (;;) {
        /* Pass 1: snapshot descriptor, compute candidate. */
        (void)cellAtomicLockLine64(lockline, ea);
        head       = (uint32_t)(lockline[0] >> 32);
        pop_commit = (uint32_t)(lockline[1] & 0xFFFFFFFFu);
        entry_size = (uint32_t)(lockline[2] >> 32);
        depth      = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
        buffer_ea  = lockline[3];

        if ((uint32_t)(lockline[8] & 0xFFFFFFFFu) != 0) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }

        if ((head - pop_commit) >= depth) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }

        pos = head;
        seq_base = (buffer_ea + (uint64_t)depth * entry_size + 15) & ~15ULL;
        /* no putllc -- drop snapshot */

        /* Pass 2a: read seq[pos] OUTSIDE reservation. */
        if (get_u32(seq_base + (uint64_t)(pos % depth) * 4, tag) != pos) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }

        /* Pass 2b: fresh getllar, verify head unchanged, reserve. */
        (void)cellAtomicLockLine64(lockline, ea);
        if ((uint32_t)(lockline[0] >> 32) != pos) continue;
        if ((uint32_t)(lockline[8] & 0xFFFFFFFFu) != 0) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }
        {
            uint32_t pc = (uint32_t)(lockline[1] & 0xFFFFFFFFu);
            uint32_t dp = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
            if ((pos - pc) >= dp) {
                if (isBlocking) continue;
                return CELL_SYNC_ERROR_AGAIN;
            }
        }

        lockline[0] = ((uint64_t)(pos + 1) << 32) |
                       (lockline[0] & 0xFFFFFFFFu);
        if (!cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            continue;

        /* Reserved. DMA caller data to slot. */
        pContainer->pointer = (int)pos;
        memcpy(xfer_buf, pContainer->buffer, entry_size);
        mfc_put(xfer_buf,
                buffer_ea + (uint64_t)(pos % depth) * entry_size,
                entry_size, tag, 0, 0);
        mfc_write_tag_mask(1u << tag);
        mfc_read_tag_status_all();
        return CELL_OK;
    }
}

int cellSyncLFQueuePushEnd(uint64_t ea,
                           CellSyncLFQueuePushContainer *pContainer)
{
    uint32_t pos, depth;
    uint32_t old_pc, old_head, new_pc;
    uint64_t buffer_ea, seq_base;
    unsigned int tag;

    tag = pContainer->tag;
    if (tag > 31) return CELL_SYNC_ERROR_INVAL;

    pos = (uint32_t)pContainer->pointer;

    /* Read metadata once (immutable after init). */
    (void)cellAtomicLockLine64(lockline, ea);
    depth     = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
    buffer_ea = lockline[3];
    seq_base  = (buffer_ea + (uint64_t)depth *
                 (uint32_t)(lockline[2] >> 32) + 15) & ~15ULL;
    /* drop */

    /* Publish seq[pos] = pos + 1. */
    put_u32(pos + 1, seq_base + (uint64_t)(pos % depth) * 4, tag);

    /* Advance push_commit contiguously. */
    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        old_pc   = (uint32_t)(lockline[1] >> 32);
        old_head = (uint32_t)(lockline[0] >> 32);
        depth    = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
        /* drop */

        if (old_pc >= old_head) return CELL_OK;

        new_pc = old_pc;
        while (new_pc < old_head) {
            if (get_u32(seq_base + (uint64_t)(new_pc % depth) * 4, tag) !=
                new_pc + 1)
                break;
            new_pc++;
        }

        if (new_pc == old_pc) return CELL_OK;

        (void)cellAtomicLockLine64(lockline, ea);
        if ((uint32_t)(lockline[1] >> 32) != old_pc) continue;
        lockline[1] = ((uint64_t)new_pc << 32) |
                       (lockline[1] & 0xFFFFFFFFu);
        if (!cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            continue;

        /* Signal PPU consumer. */
        if ((uint32_t)(lockline[4] >> 32) == CELL_SYNC_QUEUE_SPU2PPU &&
            lockline[5])
            signal_inc(lockline[5]);
        return CELL_OK;
    }
}

int _cellSyncLFQueuePopBeginBody(uint64_t ea,
                                 CellSyncLFQueuePopContainer *pContainer,
                                 unsigned int isBlocking)
{
    uint32_t tail, push_commit;
    uint32_t entry_size, depth;
    uint64_t buffer_ea, seq_base;
    uint32_t pos;
    unsigned int tag;

    tag = pContainer->tag;
    if (tag > 31) return CELL_SYNC_ERROR_INVAL;

    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        tail        = (uint32_t)(lockline[0] & 0xFFFFFFFFu);
        push_commit = (uint32_t)(lockline[1] >> 32);
        entry_size  = (uint32_t)(lockline[2] >> 32);
        depth       = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
        buffer_ea   = lockline[3];

        if ((uint32_t)(lockline[8] & 0xFFFFFFFFu) != 0) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }

        if (tail >= push_commit) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }

        pos = tail;
        seq_base = (buffer_ea + (uint64_t)depth * entry_size + 15) & ~15ULL;
        /* drop */

        if (get_u32(seq_base + (uint64_t)(pos % depth) * 4, tag) != pos + 1) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }

        (void)cellAtomicLockLine64(lockline, ea);
        if ((uint32_t)(lockline[0] & 0xFFFFFFFFu) != pos) continue;
        if ((uint32_t)(lockline[8] & 0xFFFFFFFFu) != 0) {
            if (isBlocking) continue;
            return CELL_SYNC_ERROR_AGAIN;
        }
        {
            uint32_t pc = (uint32_t)(lockline[1] >> 32);
            if (pos >= pc) {
                if (isBlocking) continue;
                return CELL_SYNC_ERROR_AGAIN;
            }
        }
        lockline[0] = (lockline[0] & 0xFFFFFFFF00000000u) | (pos + 1);
        if (!cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            continue;

        pContainer->pointer = (int)pos;
        mfc_get(xfer_buf,
                buffer_ea + (uint64_t)(pos % depth) * entry_size,
                entry_size, tag, 0, 0);
        mfc_write_tag_mask(1u << tag);
        mfc_read_tag_status_all();
        memcpy(pContainer->buffer, xfer_buf, entry_size);
        return CELL_OK;
    }
}

int cellSyncLFQueuePopEnd(uint64_t ea,
                          CellSyncLFQueuePopContainer *pContainer)
{
    uint32_t pos, depth;
    uint32_t old_pc, old_tail, new_pc;
    uint64_t buffer_ea, seq_base;
    unsigned int tag;

    tag = pContainer->tag;
    if (tag > 31) return CELL_SYNC_ERROR_INVAL;

    pos = (uint32_t)pContainer->pointer;

    (void)cellAtomicLockLine64(lockline, ea);
    depth     = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
    buffer_ea = lockline[3];
    seq_base  = (buffer_ea + (uint64_t)depth *
                 (uint32_t)(lockline[2] >> 32) + 15) & ~15ULL;
    /* drop */

    put_u32(pos + depth, seq_base + (uint64_t)(pos % depth) * 4, tag);

    for (;;) {
        (void)cellAtomicLockLine64(lockline, ea);
        old_pc   = (uint32_t)(lockline[1] & 0xFFFFFFFFu);
        old_tail = (uint32_t)(lockline[0] & 0xFFFFFFFFu);
        depth    = (uint32_t)(lockline[2] & 0xFFFFFFFFu);
        /* drop */

        if (old_pc >= old_tail) return CELL_OK;

        new_pc = old_pc;
        while (new_pc < old_tail) {
            if (get_u32(seq_base + (uint64_t)(new_pc % depth) * 4, tag) !=
                new_pc + depth)
                break;
            new_pc++;
        }

        if (new_pc == old_pc) return CELL_OK;

        (void)cellAtomicLockLine64(lockline, ea);
        if ((uint32_t)(lockline[1] & 0xFFFFFFFFu) != old_pc) continue;
        lockline[1] = (lockline[1] & 0xFFFFFFFF00000000u) | new_pc;
        if (!cellAtomicStoreConditional64(lockline, ea, lockline[0]))
            continue;

        if ((uint32_t)(lockline[4] >> 32) == CELL_SYNC_QUEUE_PPU2SPU &&
            lockline[5])
            signal_inc(lockline[5]);
        return CELL_OK;
    }
}
