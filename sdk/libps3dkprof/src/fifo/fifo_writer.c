/* FIFO wrap phase events: the drain-thread side (docs/design/ps3dk-prof-fifo.md).  It takes no lock: it reads the
 * immutable instance fields after an acquire of `state`, the ring through write_idx (acquire), and the owner counters
 * with acquire loads in a fixed order; it releases read_idx only after the sink accepted the whole chunk.  A failed
 * sink is terminal for the capture and nothing is retried. */
#include "fifo_internal.h"

static int config_chunk(prof *p, fifo_capture *f)
{
    uint8_t *b = p_fifo_payload(p);
    p_le16(b, 1);
    p_le16(b + 2, 1); /* PHASE_EVENTS */
    p_le32(b + 4, f->instance);
    p_le32(b + 8, f->domain);
    p_le32(b + 12, f->owner_inc);
    p_le32(b + 16, FIFO_REC_BYTES);
    p_le32(b + 20, f->ring_records);
    p_le32(b + 24, f->max_bytes);
    p_le32(b + 28, f->allocated);
    p_le64(b + 32, p->tb_freq);
    p_le32(b + 40, f->begin_off);
    p_le32(b + 44, f->end_off);
    p_le32(b + 48, f->generation);
    p_le32(b + 52, 0);
    p_le64(b + 56, 0);
    return p_fifo_emit_chunk(p, C_FIFO_CONFIG, b, 64);
}

/* Every slot published so far, in whole chunks; 0 when all of them were accepted. */
static int drain_events(prof *p, fifo_capture *f)
{
    uint64_t w = LD_ACQ(&f->write_idx);
    while (f->d_read < w) {
        uint64_t n = w - f->d_read, i, last = 0;
        uint8_t *b = p_fifo_payload(p), *q;
        if (n > FIFO_EVENTS_MAX)
            n = FIFO_EVENTS_MAX;
        p_le16(b, 1);
        p_le16(b + 2, 0);
        p_le32(b + 4, f->instance);
        p_le32(b + 8, (uint32_t)n);
        p_le32(b + 12, 0);
        q = b + 16;
        for (i = 0; i < n; i++, q += FIFO_REC_BYTES) {
            const fifo_rec *e = &f->ring[(f->d_read + i) % f->ring_records];
            p_le64(q, e->seq);
            p_le64(q + 8, e->wrap_id);
            p_le64(q + 16, e->tb_before);
            p_le64(q + 24, e->tb_after);
            p_le32(q + 32, f->owner_inc);
            p_le16(q + 36, e->kind);
            p_le16(q + 38, e->flags);
            p_le32(q + 40, e->frame_owner);
            p_le32(q + 44, 0);
            p_le64(q + 48, e->frame_id);
            p_le32(q + 56, e->begin_off);
            p_le32(q + 60, e->tail_off);
            p_le32(q + 64, e->target_off);
            p_le32(q + 68, e->requested_words);
            p_le32(q + 72, e->current_off);
            p_le32(q + 76, 0);
            p_le64(q + 80, 0);
            p_le64(q + 88, 0);
            last = e->seq;
        }
        if (p_fifo_emit_chunk(p, C_FIFO_EVENTS, b, 16u + (size_t)n * FIFO_REC_BYTES) != 0)
            return -1; /* not accepted: the slots stay owned by the drainer, never reused */
        f->d_read += n;
        f->d_written += n;
        f->d_last_seq = last;
        ST_REL(&f->read_idx, f->d_read); /* only now may the owner reuse them */
    }
    return 0;
}

/* One status.  The load order keeps the lower-bound relations true against a running owner: the events were taken
 * first (write_idx); completed before started; dropped before attempted. */
static int status_chunk(prof *p, fifo_capture *f, uint16_t flags)
{
    uint8_t *b = p_fifo_payload(p);
    uint64_t completed = LD_ACQ(&f->completed), dropped = LD_ACQ(&f->dropped);
    uint64_t started = LD_ACQ(&f->started), attempted = LD_ACQ(&f->attempted);
    uint64_t refused = LD_ACQ(&f->refused), open = LD_ACQ(&f->open_wrap), hw = LD_ACQ(&f->high_water);
    if (LD_ACQ(&f->saturated) || refused == UINT64_MAX || dropped == UINT64_MAX)
        flags |= FST_SATURATED;
    if (f->d_statuses && f->d_statuses == UINT32_MAX)
        flags |= FST_SATURATED;
    p_le16(b, 1);
    p_le16(b + 2, flags);
    p_le32(b + 4, f->instance);
    p_le64(b + 8, attempted);
    p_le64(b + 16, f->d_written);
    p_le64(b + 24, dropped);
    p_le64(b + 32, refused);
    p_le64(b + 40, f->d_last_seq);
    p_le64(b + 48, started);
    p_le64(b + 56, completed);
    p_le64(b + 64, open);
    p_le64(b + 72, hw);
    if (p_fifo_emit_chunk(p, C_FIFO_STATUS, b, 80) != 0)
        return -1;
    f->d_attempted = attempted;
    f->d_refused = refused;
    f->d_dropped = dropped;
    if (f->d_statuses != UINT32_MAX)
        f->d_statuses++;
    return 0;
}

/* The prerequisites of CONFIG: the owner's THREADS entry and the domain's DOMAIN entry were accepted. */
static int declared(prof *p, fifo_capture *f)
{
    if (!f->d_owner_seen && p->dr.decl_gen[f->owner_slot] == f->owner_gen)
        f->d_owner_seen = 1;
    return f->d_owner_seen && f->domain >= 1026u && f->domain < 1026u + 64u &&
           (p->gpu_declared >> (f->domain - 1026u) & 1u);
}

static void close_detached(fifo_capture *f)
{
    f->d_closed = 1;
    ST_REL(&f->state, (uint32_t)FS_CLOSED);
    pal_free(f->block); /* the owner removed its observer before DETACHED: nothing else touches the block */
    f->block = NULL;
    f->ring = NULL;
}

/* The seal's claim, first in p_quiesce_freeze_seal, before any sink check: an unattached or still-attaching instance
 * becomes SEALED, so no attach can publish after the seal began (fifo_runtime.c, the instance claim).  An ATTACHED one
 * is left to the seal's drain. */
void p_fifo_seal_claim(void)
{
    fifo_capture *f = &g_fifo;
    uint32_t exp = FS_NONE;
    if (CAS_ACQREL(&f->state, &exp, (uint32_t)FS_SEALED)) {
        f->d_closed = 1;
        return;
    }
    exp = FS_ATTACHING;
    if (CAS_ACQREL(&f->state, &exp, (uint32_t)FS_SEALED))
        f->d_closed = 1; /* that attach unwinds its own block; nothing of it was published */
}

/* The seal's end, after any sink outcome: an instance the seal could not drain (a failed sink) is SEALED too, so the
 * owner refuses every later boundary and its detach is ESTOPPED; its block stays allocated (quarantined). */
void p_fifo_seal_close(void)
{
    uint32_t exp = FS_ATTACHED;
    (void)CAS_ACQREL(&g_fifo.state, &exp, (uint32_t)FS_SEALED);
}

void p_fifo_drain(prof *p, int seal)
{
    fifo_capture *f = &g_fifo;
    uint32_t st = LD_ACQ(&f->state), exp;
    if (f->d_closed || st == FS_NONE)
        return;
    if (st == FS_ATTACHING) {
        if (!seal)
            return;
        exp = FS_ATTACHING;
        if (CAS_ACQREL(&f->state, &exp, (uint32_t)FS_SEALED)) {
            f->d_closed = 1; /* the attach will see SEALED, unwind and free its own block */
            return;
        }
        st = exp;
    }
    if (p->snk.failed)
        return;
    if (!f->d_config) {
        if (!declared(p, f)) {
            if (seal) {
                /* nothing can be written for an instance whose owner or domain was never declared */
                exp = FS_ATTACHED;
                if (st == FS_ATTACHED && CAS_ACQREL(&f->state, &exp, (uint32_t)FS_SEALED))
                    f->d_closed = 1;
                else if (LD_ACQ(&f->state) == FS_DETACHED)
                    close_detached(f);
                else
                    f->d_closed = 1;
            }
            return;
        }
        if (config_chunk(p, f) != 0)
            return;
        f->d_config = 1;
    }
    if (drain_events(p, f) != 0)
        return;
    if (seal && st == FS_ATTACHED) {
        exp = FS_ATTACHED;
        if (CAS_ACQREL(&f->state, &exp, (uint32_t)FS_SEALED)) {
            /* the owner never acknowledged: lower bounds, final totals unavailable, the block is quarantined */
            (void)drain_events(p, f);
            if (!p->snk.failed)
                (void)status_chunk(p, f, FST_FINAL | FST_LOWER_BOUND | FST_OWNER_UNACK);
            f->d_closed = 1;
            return;
        }
        st = exp; /* the owner detached meanwhile */
        if (drain_events(p, f) != 0)
            return;
    }
    if (st == FS_DETACHED) {
        if (status_chunk(p, f, FST_FINAL) == 0)
            close_detached(f);
        return;
    }
    /* a provisional status when the owner's visible counts moved */
    if (f->d_statuses == 0 || LD_ACQ(&f->attempted) != f->d_attempted || LD_ACQ(&f->refused) != f->d_refused ||
        LD_ACQ(&f->dropped) != f->d_dropped)
        (void)status_chunk(p, f, FST_LOWER_BOUND);
}
