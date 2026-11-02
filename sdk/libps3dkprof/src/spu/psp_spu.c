/* SPU lane producer core (design p3.2 r3).  The unpublished suffix of the sequence is [older_lost][staged][tail_lost],
 * contiguous; a publication is the longest prefix of LOSS(older), staged events, LOSS(tail), CAL that fits, and the
 * rest merges into older_lost (r3 A).  Every MFC status is consumed and checked; a failure latches FAULT, after which
 * no DMA is issued (r3 D). */
#include "psp_spu.h"

#include <string.h>

#define PSP_ENOSPC 7

static uint8_t *slot_at(psp_spu *s, uint32_t i) { return s->buf + (size_t)i * PSP_SLOT; }

static void fault(psp_spu *s)
{
    s->flags |= PSP_F_FAULT;
    s->terminal = 1;
    s->phase = PSP_FL_IDLE;
}

static void refuse(psp_spu *s)
{
    if (s->refused == UINT64_MAX) {
        fault(s); /* the counter cannot represent another refusal: terminal */
        return;
    }
    s->refused++;
    s->flags |= PSP_F_OVERFLOWED;
}

static int blocked(psp_spu *s)
{
    if (!s->port->blocking_events(s->port->ctx))
        return 0;
    if (s->refused_calls == UINT64_MAX)
        fault(s);
    else
        s->refused_calls++;
    return 1;
}

static int tag_wait(psp_spu *s)
{
    const psp_port *p = s->port;
    uint32_t saved = p->rd_tag_mask(p->ctx), st;
    p->wr_tag_mask(p->ctx, 1u << s->tag);
    st = p->tag_wait_all(p->ctx);
    p->wr_tag_mask(p->ctx, saved);
    return (st & (1u << s->tag)) ? 0 : -1;
}

/* getllar + putllc of the same bytes: an atomic snapshot that leaves no reservation of ours behind.  The putllc is
 * issued even when the getllar status is wrong, so a reservation the getllar may have taken never outlives the call;
 * either failure then latches FAULT in the caller. */
static int snapshot(psp_spu *s, uint64_t ea, uint8_t *line)
{
    const psp_port *p = s->port;
    int ok;
    p->getllar(p->ctx, ea, line);
    ok = (p->rd_atomic_stat(p->ctx) & PSP_AS_GETLLAR) != 0;
    p->putllc(p->ctx, ea, line);
    if (!(p->rd_atomic_stat(p->ctx) & (PSP_AS_PUTLLC_OK | PSP_AS_PUTLLC_LOST)))
        ok = 0;
    return ok ? 0 : -1;
}

static void image_b(psp_spu *s, uint32_t state)
{
    uint8_t *b = s->line_b;
    memset(b, 0, sizeof s->line_b);
    psp_be64(b + PSP_B_GEN - PSP_B, s->gen);
    psp_be64(b + PSP_B_WRITE_IDX - PSP_B, s->w);
    psp_be64(b + PSP_B_NEXT_SEQ - PSP_B, s->seq);
    psp_be64(b + PSP_B_OLDER_LOST - PSP_B, s->older_lost);
    psp_be64(b + PSP_B_REFUSED - PSP_B, s->refused);
    psp_be64(b + PSP_B_REFUSED_CALLS - PSP_B, s->refused_calls);
    psp_be32(b + PSP_B_STATE - PSP_B, state);
    psp_be32(b + PSP_B_FLAGS - PSP_B, s->flags);
    psp_be16(b + PSP_B_CAL_ANSWERED - PSP_B, s->cal_answered);
    psp_be32(b + PSP_B_EPOCHS_WRITTEN - PSP_B, s->epochs_written);
    psp_be64(b + PSP_B_GEN_END - PSP_B, s->gen);
}

static int put_b(psp_spu *s)
{
    const psp_port *p = s->port;
    p->putlluc(p->ctx, s->ea_ctl + PSP_B, s->line_b);
    if (!(p->rd_atomic_stat(p->ctx) & PSP_AS_PUTLLUC)) {
        fault(s);
        return -1;
    }
    return 0;
}

int psp_spu_init(psp_spu *s, const psp_port *port, uint64_t ea_ring, uint32_t ring_slots, uint64_t ea_ctl,
                 uint32_t tag, uint8_t *buf, uint32_t ls_cap, int own_dec)
{
    memset(s, 0, sizeof *s);
    s->port = port;
    if (tag > 31 || ls_cap < 4 || ls_cap > 4096 || (ls_cap & 1) || ring_slots < 8 || ring_slots > (1u << 20) ||
        (ring_slots & (ring_slots - 1)) || ((uintptr_t)buf & 15) || (ea_ring & 127) || (ea_ctl & 127))
        return PSP_EINVAL;
    s->ea_ring = ea_ring;
    s->ring_slots = ring_slots;
    s->ea_ctl = ea_ctl;
    s->tag = tag;
    s->buf = buf;
    s->ls_cap = ls_cap;
    if (blocked(s))
        return PSP_EBUSY;
    if (snapshot(s, ea_ctl + PSP_B, s->line_b) != 0) {
        fault(s);
        return PSP_EFAULT;
    }
    if (psp_rd32(s->line_b + PSP_B_STATE - PSP_B) != PSP_ST_NONE || psp_rd64(s->line_b) != 0) {
        s->terminal = 1;
        return PSP_ECLOSED; /* one EA, one init; our reservation already ended in snapshot() */
    }
    s->gen = 1;
    s->flags = own_dec ? PSP_F_OWN_DEC : 0;
    image_b(s, PSP_ST_STARTED);
    if (put_b(s) != 0)
        return PSP_EFAULT;
    s->state = PSP_ST_STARTED;
    return PSP_OK;
}

/* one event; returns 1 when it took a sequence number */
static int record(psp_spu *s, uint8_t kind, uint16_t desc, uint8_t depth)
{
    if (s->terminal || s->state != PSP_ST_STARTED) {
        refuse(s);
        return 0;
    }
    if (s->seq >= PSP_SEQ_MAX) {
        s->terminal = 1; /* no further admission: the source is OVERFLOWED */
        refuse(s);
        return 0;
    }
    s->seq++;
    if (s->tail_lost || s->n == s->ls_cap) {
        s->tail_lost++; /* frozen staging: a drop never lands before staged events */
    } else {
        psp_slot(slot_at(s, PSP_HEAD + s->n), s->port->rd_dec(s->port->ctx), desc, kind, depth);
        s->n++;
    }
    return 1;
}

void psp_spu_begin(psp_spu *s, uint16_t desc)
{
    if (s->suppressed || s->depth == PSP_MAX_DEPTH) {
        if (s->suppressed == UINT32_MAX) {
            fault(s); /* suppression cannot wrap: a later END must never pop an admitted parent */
            return;
        }
        s->suppressed++;
        refuse(s);
        return;
    }
    if (record(s, PSP_K_BEGIN, desc, (uint8_t)s->depth))
        s->depth++;
}

void psp_spu_end(psp_spu *s, uint16_t desc)
{
    if (s->suppressed) {
        s->suppressed--;
        refuse(s);
        return;
    }
    if (s->depth) {
        if (record(s, PSP_K_END, desc, (uint8_t)(s->depth - 1)))
            s->depth--;
    } else {
        record(s, PSP_K_END, desc, 0); /* raw: the reader reports END on an empty stack */
    }
}

void psp_spu_mark(psp_spu *s, uint16_t desc)
{
    if (s->suppressed || s->depth == PSP_MAX_DEPTH) {
        refuse(s);
        return;
    }
    record(s, PSP_K_MARK, desc, (uint8_t)s->depth);
}

static uint64_t ceil_markers(uint64_t lost) { return lost ? (lost - 1) / PSP_LOSS_MAX + 1 : 0; }
static uint32_t marker_count(uint64_t lost, uint64_t i) /* the i-th marker of a range split from its start */
{
    uint64_t left = lost - i * (uint64_t)PSP_LOSS_MAX;
    return (uint32_t)(left > PSP_LOSS_MAX ? PSP_LOSS_MAX : left);
}

static void exhaust(psp_spu *s)
{
    s->flags |= PSP_F_GEN_EXHAUSTED | PSP_F_OVERFLOWED;
    s->terminal = 1;
    s->older_lost += (uint64_t)s->n + s->tail_lost; /* the whole unpublished suffix, still contiguous */
    s->n = 0;
    s->tail_lost = 0;
}

int psp_spu_flush_plan(psp_spu *s)
{
    uint64_t r, space, mo, mt, pub_lost = 0;
    uint32_t f, fp, e = 0, t = 0, c = 0, k, i, used;
    uint16_t req, cancel, answer = 0;
    if (s->flags & PSP_F_FAULT)
        return PSP_EFAULT;
    if (s->state != PSP_ST_STARTED)
        return PSP_ECLOSED;
    if (blocked(s))
        return (s->flags & PSP_F_FAULT) ? PSP_EFAULT : PSP_EBUSY;
    if (snapshot(s, s->ea_ctl, s->line_a) != 0) {
        fault(s);
        return PSP_EFAULT;
    }
    r = psp_rd64(s->line_a + PSP_A_READ_IDX);
    if ((r & 1) || r < s->r_seen || r > s->w || s->w - r > s->ring_slots) {
        fault(s); /* corrupt incoming read index: checked before it enters any subtraction */
        return PSP_EFAULT;
    }
    s->r_seen = r;
    req = psp_rd16(s->line_a + PSP_A_CAL_REQ);
    cancel = psp_rd16(s->line_a + PSP_A_CAL_CANCEL);
    if (req && req > s->cal_answered && req != cancel)
        answer = req;
    space = s->ring_slots - (s->w - r);
    if (s->flags & PSP_F_GEN_EXHAUSTED) {
        /* no publication but CLOSED is left; a lane that only stopped admitting (seq) still publishes its staging */
        s->pub_k = 0;
        s->phase = PSP_FL_PLANNED;
        return PSP_ENOSPC;
    }
    if (s->gen + 2 >= PSP_GEN_MAX || s->w + 2 > PSP_W_MAX) {
        exhaust(s); /* the last generation stays reserved for CLOSED */
        s->pub_k = 0;
        s->phase = PSP_FL_PLANNED;
        return PSP_ENOSPC;
    }
    if (s->w + space > PSP_W_MAX)
        space = PSP_W_MAX - s->w;

    /* the longest fitting prefix of LOSS(older) e_0..e_{n-1} LOSS(tail) CAL; the front keeps 16-byte alignment */
    mo = ceil_markers(s->older_lost);
    f = (uint32_t)(mo < PSP_HEAD ? mo : PSP_HEAD);
    while (f && f + (f & 1) > space)
        f--;
    fp = f & 1;
    for (i = 0; i < f; i++)
        pub_lost += marker_count(s->older_lost, i);
    used = f + fp;
    if (f == mo) {
        e = (uint32_t)(s->n < space - used ? s->n : space - used);
        used += e;
        if (e == s->n) {
            mt = ceil_markers(s->tail_lost);
            t = (uint32_t)(mt < 2 ? mt : 2);
            if (t > space - used)
                t = (uint32_t)(space - used);
            for (i = 0; i < t; i++)
                pub_lost += marker_count(s->tail_lost, i);
            used += t;
            if (t == mt && answer && used < space)
                c = 1;
            used += c;
        }
    }
    k = used;
    if (k & 1) {
        if (k + 1 > space) {
            fault(s); /* cannot happen: odd k means the plan was not cut, and space is even */
            return PSP_EFAULT;
        }
        k++;
    }
    /* write the markers around the staged events */
    for (i = 0; i < f; i++)
        psp_slot(slot_at(s, PSP_HEAD - f + i), marker_count(s->older_lost, i), 0, PSP_K_LOSS, 0);
    if (fp)
        psp_slot(slot_at(s, PSP_HEAD - f - 1), 0, 0, PSP_K_PAD, 0);
    for (i = 0; i < t; i++)
        psp_slot(slot_at(s, PSP_HEAD + e + i), marker_count(s->tail_lost, i), 0, PSP_K_LOSS, 0);
    if (c)
        psp_slot(slot_at(s, PSP_HEAD + e + t), s->port->rd_dec(s->port->ctx), answer, PSP_K_CAL, 0);
    if (k > used)
        psp_slot(slot_at(s, PSP_HEAD + e + t + c), 0, 0, PSP_K_PAD, 0);
    s->pub_first = PSP_HEAD - f - fp;
    s->pub_k = k;
    s->pub_events = e;
    s->pub_lost = pub_lost;
    s->pub_cal = c ? answer : 0;
    s->pub_epoch = 0;
    s->phase = PSP_FL_PLANNED;
    return PSP_OK;
}

void psp_spu_flush_put(psp_spu *s)
{
    const psp_port *p = s->port;
    uint32_t left = s->pub_k, at = s->pub_first;
    uint64_t w = s->w;
    while (left) {
        uint32_t pos = (uint32_t)(w & (s->ring_slots - 1)), run = s->ring_slots - pos, cmd;
        if (run > left)
            run = left;
        while (run) {
            cmd = run > 2048 ? 2048 : run; /* 16 KiB per command */
            p->put(p->ctx, slot_at(s, at), s->ea_ring + (uint64_t)pos * PSP_SLOT, cmd * PSP_SLOT, s->tag);
            at += cmd;
            pos += cmd;
            w += cmd;
            left -= cmd;
            run -= cmd;
        }
    }
    s->phase = PSP_FL_PUT;
}

int psp_spu_flush_wait(psp_spu *s)
{
    if (s->pub_k && tag_wait(s) != 0) {
        fault(s);
        return PSP_EFAULT;
    }
    s->phase = PSP_FL_WAITED;
    return PSP_OK;
}

int psp_spu_flush_publish(psp_spu *s)
{
    uint64_t unpublished = s->older_lost + s->n + s->tail_lost - s->pub_events - s->pub_lost;
    if (s->pub_epoch)
        unpublished = 0;
    s->older_lost = unpublished;
    s->n = 0;
    s->tail_lost = 0;
    if (s->pub_k == 0) {
        s->phase = PSP_FL_IDLE;
        return PSP_OK; /* nothing visible changed: no generation spent */
    }
    s->w += s->pub_k;
    s->gen += 2;
    if (s->pub_cal)
        s->cal_answered = s->pub_cal;
    if (s->pub_epoch)
        s->epochs_written++;
    image_b(s, PSP_ST_STARTED);
    s->phase = PSP_FL_IDLE;
    return put_b(s) == 0 ? PSP_OK : PSP_EFAULT;
}

int psp_spu_flush(psp_spu *s)
{
    int rc = psp_spu_flush_plan(s);
    if (rc == PSP_EBUSY || rc == PSP_EFAULT || rc == PSP_ECLOSED)
        return rc;
    if (rc == PSP_ENOSPC) {
        s->phase = PSP_FL_IDLE;
        return rc;
    }
    psp_spu_flush_put(s);
    if (psp_spu_flush_wait(s) != PSP_OK)
        return PSP_EFAULT;
    return psp_spu_flush_publish(s);
}

int psp_spu_fini(psp_spu *s)
{
    int rc;
    if (s->state != PSP_ST_STARTED)
        return PSP_ECLOSED;
    rc = psp_spu_flush(s);
    if (rc == PSP_EBUSY || rc == PSP_EFAULT)
        return rc; /* nothing published, state stays STARTED; EBUSY may be retried */
    return psp_spu_fini_close(s);
}

int psp_spu_fini_close(psp_spu *s)
{
    if (s->state != PSP_ST_STARTED)
        return PSP_ECLOSED;
    if (s->flags & PSP_F_FAULT)
        return PSP_EFAULT;
    if (blocked(s))
        return (s->flags & PSP_F_FAULT) ? PSP_EFAULT : PSP_EBUSY;
    if (s->gen + 2 > PSP_GEN_MAX) {
        fault(s); /* the reserved credit is always there; reaching this is library corruption */
        return PSP_EFAULT;
    }
    s->older_lost += (uint64_t)s->n + s->tail_lost;
    s->n = 0;
    s->tail_lost = 0;
    s->gen += 2;
    image_b(s, PSP_ST_CLOSED);
    if (put_b(s) != 0)
        return PSP_EFAULT;
    s->state = PSP_ST_CLOSED;
    s->terminal = 1;
    return PSP_OK;
}

int psp_spu_dec_preflight(psp_spu *s)
{
    if (!(s->flags & PSP_F_OWN_DEC))
        return PSP_ENOTSUP;
    if (s->flags & PSP_F_FAULT)
        return PSP_EFAULT;
    if (s->terminal || s->state != PSP_ST_STARTED)
        return PSP_ECLOSED;
    /* everything earlier is published, the EPOCH publication fits, and it leaves the CLOSED credit */
    if (s->n || s->tail_lost || s->older_lost || s->ring_slots - (s->w - s->r_seen) < 2 ||
        s->gen + 2 >= PSP_GEN_MAX || s->w + 2 > PSP_W_MAX)
        return PSP_EAGAIN;
    return PSP_OK;
}

void psp_spu_dec_commit(psp_spu *s, uint32_t v)
{
    s->port->wr_dec(s->port->ctx, v); /* the write happens first; its record is published right after */
    psp_slot(slot_at(s, PSP_HEAD - 2), v, 0, PSP_K_EPOCH, 0);
    psp_slot(slot_at(s, PSP_HEAD - 1), 0, 0, PSP_K_PAD, 0);
    s->pub_first = PSP_HEAD - 2;
    s->pub_k = 2;
    s->pub_events = 0;
    s->pub_lost = 0;
    s->pub_cal = 0;
    s->pub_epoch = 1;
    s->phase = PSP_FL_PLANNED;
}

int psp_spu_dec_write(psp_spu *s, uint32_t v)
{
    int rc;
    if (!(s->flags & PSP_F_OWN_DEC))
        return PSP_ENOTSUP;
    rc = psp_spu_flush(s);
    if (rc != PSP_OK)
        return rc == PSP_ENOSPC ? PSP_EAGAIN : rc;
    rc = psp_spu_dec_preflight(s);
    if (rc != PSP_OK)
        return rc;
    psp_spu_dec_commit(s, v);
    psp_spu_flush_put(s);
    if (psp_spu_flush_wait(s) != PSP_OK)
        return PSP_EFAULT;
    return psp_spu_flush_publish(s);
}
