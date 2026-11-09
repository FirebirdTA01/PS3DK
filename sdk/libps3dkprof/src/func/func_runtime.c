/* Function paths: configuration, storage, the cut (owner or quiesced drainer) and the drainer's chunks.
 * Contract: docs/design/ps3dk-prof-func-paths.md.  Ownership rules: func_internal.h. */
#include "func_internal.h"
#include <string.h>

ps3dk_prof_func_config p_func_options; /* zero size: defaults */
#ifdef PS3DK_PROF_TEST_HOOKS
void (*ps3dk_prof__func_test_hook)(unsigned point);
#endif

static uint64_t al8(uint64_t n) { return (n + 7u) & ~(uint64_t)7u; }

/* Bounded: at most four compare-and-swap attempts.  At the ceiling, or when contention exhausts the attempts, the
 * count stops being exact; the sticky status_sat makes every later FUNC_STATUS say so (a lower bound). */
void p_func_count(func_capture *f, uint64_t *c)
{
    uint64_t v = LD_RLX(c);
    for (unsigned i = 0; i < 4; i++) {
        if (v == UINT64_MAX)
            break;
        if (__atomic_compare_exchange_n(c, &v, v + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            if (v + 1 != UINT64_MAX)
                return;
            break;
        }
    }
    ST_RLX(&f->status_sat, 1u);
}

#ifdef PS3DK_PROF_TEST_HOOKS
void ps3dk_prof__func_test_seed(uint64_t unregistered, uint64_t no_actor)
{
    func_capture *f = g_prof->func;
    ST_RLX(&f->unregistered, unregistered);
    ST_RLX(&f->no_actor, no_actor);
    f->rep_unreg = unregistered;
    f->rep_noactor = no_actor;
}
void ps3dk_prof__func_test_seed_counter(unsigned k, uint64_t v)
{
    func_actor *a = p_func_actor(g_prof->func, g_tls.slot1 - 1);
    if (a && k < FC_N)
        a->cnt[k] = v;
}
#endif

/* Resolve defaults and compute the whole layout with overflow checks; 0 or EINVAL. */
int p_func_layout(ps3dk_prof_func_config *c, uint32_t nslots, func_capture *o)
{
    uint64_t hs = 1, per, total;
    if (c->size != sizeof *c || c->reserved)
        return PS3DK_PROF_EINVAL;
    if (!c->max_bytes)
        c->max_bytes = F_DEF_BYTES;
    if (!c->max_actors)
        c->max_actors = nslots < F_DEF_ACTORS ? nslots : F_DEF_ACTORS;
    if (!c->node_capacity)
        c->node_capacity = F_DEF_CAP;
    if (!c->stack_depth)
        c->stack_depth = F_DEF_DEPTH;
    if (!c->max_actors || (nslots && c->max_actors > nslots) || c->max_actors > 1024 ||
        c->node_capacity < F_CAP_MIN || c->node_capacity > F_CAP_MAX ||
        c->stack_depth < F_DEPTH_MIN || c->stack_depth > F_DEPTH_MAX)
        return PS3DK_PROF_EINVAL;
    while (hs < 2u * (uint64_t)c->node_capacity)
        hs <<= 1;
    memset(o, 0, sizeof *o);
    o->hash_slots = (uint32_t)hs;
    o->off_frames = al8(sizeof(func_actor));
    o->off_nodes = o->off_frames + al8((uint64_t)c->stack_depth * sizeof(func_frame));
    o->off_rows = o->off_nodes + (uint64_t)c->node_capacity * sizeof(func_node);
    o->off_hash = o->off_rows + (uint64_t)c->node_capacity * sizeof(func_row);
    o->off_dirty = o->off_hash + al8(hs * 4u);
    o->off_buf = o->off_dirty + al8((uint64_t)c->node_capacity * 4u);
    per = o->off_buf + al8(F_HDR + (uint64_t)c->node_capacity * (F_DEF + F_ROW));
    total = al8(sizeof(func_capture)) + (uint64_t)c->max_actors * per; /* all terms are small: no overflow */
    if (total > c->max_bytes)
        return PS3DK_PROF_EINVAL;
    o->stride = per;
    o->allocated = total;
    o->cfg = *c;
    return 0;
}

int ps3dk_prof_func_configure(const ps3dk_prof_func_config *cfg)
{
    uint32_t expected = G_UNINIT;
    ps3dk_prof_func_config c;
    func_capture probe;
    int rc;
    if (!CAS_ACQREL(&g_gate, &expected, G_CONFIGURING))
        return expected == G_READY || expected == G_TERMINAL ? PS3DK_PROF_EALREADY : PS3DK_PROF_EBUSY;
    rc = PS3DK_PROF_EINVAL;
    if (cfg) {
        c = *cfg;
        rc = p_func_layout(&c, 0, &probe); /* the thread ceiling is checked again at init */
        if (!rc)
            p_func_options = *cfg;
    }
    ST_REL(&g_gate, G_UNINIT);
    return rc;
}

int p_func_init(prof *p)
{
    ps3dk_prof_func_config c = p_func_options;
    func_capture lay;
    if (!c.size)
        c.size = sizeof c;
    if (p_func_layout(&c, p->nslots, &lay))
        return PS3DK_PROF_EINVAL;
    p->func = pal_alloc((size_t)lay.allocated);
    if (!p->func)
        return PS3DK_PROF_ENOMEM;
    memset(p->func, 0, (size_t)lay.allocated);
    *p->func = lay;
    return 0;
}

void p_func_unwind(prof *p)
{
    if (p->func) {
        pal_free(p->func);
        p->func = NULL;
    }
}

func_actor *p_func_actor(func_capture *f, uint32_t slot)
{
    if (!f || slot >= f->cfg.max_actors)
        return NULL;
    return (func_actor *)(void *)((uint8_t *)f + al8(sizeof(func_capture)) + (size_t)slot * f->stride);
}

/* Under thread_mutex, slot FREE: the previous incarnation's FINAL was written before the slot was freed. */
void p_func_enroll(prof *p, uint32_t slot, uint32_t inc)
{
    func_capture *f = p->func;
    func_actor *a = p_func_actor(f, slot);
    func_node *n;
    uint64_t e;
    if (!a)
        return;
    memset(a, 0, (size_t)f->stride); /* registration is rare; the whole actor, hash table included */
    e = LD_RLX(&f->epoch);
    a->incarnation = inc;
    a->enroll_epoch = a->last_pub = a->d_acked = a->deferred_epoch = e;
    ST_RLX(&a->pub, e);
    ST_RLX(&a->ack, e);
    a->start = p_tb();
    n = f_nodes(f, a);
    n[F_LOST].fn = 0;
    n[F_LOST].parent = F_LOST_PARENT;
    a->nodes = 1;
    a->live = 1;
}

/* The owner, immediately before the core's RELEASE of its ack/retire word (stop acknowledgement or thread_end). */
void p_func_stamp_final(prof *p)
{
    func_actor *a;
    if (!p->func || !g_tls.slot1)
        return;
    a = p_func_actor(p->func, g_tls.slot1 - 1);
    if (!a || !a->live || a->incarnation != g_tls.incarnation)
        return;
    if (g_tls.func_reentry) {
        uint32_t sat = g_tls.func_reentry == UINT32_MAX;
        f_add(&a->cnt[FC_REENTRY], g_tls.func_reentry, &sat);
        if (sat)
            a->iflags |= FF_SAT;
        g_tls.func_reentry = 0;
    }
    a->final_tick = p_tb();
    a->final_set = 1;
}

/* The cut: charge every retained open activation exactly once up to tick, advance the cursors, write the new
 * definitions and the touched rows into the publish buffer, and zero the interval.  Run by the owner (buffer
 * acknowledged) or by the drainer on a quiesced actor.  Does not publish. */
void p_func_cut(func_capture *f, func_actor *a, uint64_t tick, uint64_t epoch, uint16_t flags)
{
    func_frame *fr = f_frames(f, a);
    func_node *nd = f_nodes(f, a);
    func_row *rw = f_rows(f, a);
    uint32_t *dirty = f_dirty(f, a);
    uint8_t *b = f_buf(f, a), *q;
    uint32_t i, nrows = 0, ndef, sat;
    for (i = 0; i < a->depth; i++) {
        func_row *r = &rw[fr[i].node];
        f_touch(f, a, fr[i].node);
        r->flags |= FR_OPEN;
        r->open++;
        if (fr[i].nested)
            continue;
        sat = 0;
        uint64_t incl = 0, child = fr[i].child;
        if (tick >= fr[i].from)
            incl = tick - fr[i].from;
        else
            f_count(a, FC_CLOCK), a->iflags |= FF_CLOCK;
        if (i + 1 < a->depth && !fr[i + 1].nested && tick >= fr[i + 1].from)
            child += tick - fr[i + 1].from;
        if (child > incl)
            child = incl; /* only after a clock anomaly; counted above */
        f_add(&r->incl, incl, &sat);
        f_add(&r->child, child, &sat);
        if (sat)
            r->flags |= FR_SAT, a->iflags |= FF_SAT;
        fr[i].from = tick;
        fr[i].child = 0;
    }
    if (a->depth)
        a->iflags |= FF_OPEN;
    if (a->suppressed) {
        f_touch(f, a, fr[a->depth - 1].node);
        rw[fr[a->depth - 1].node].flags |= FR_SELF_UNTIMED;
        a->iflags |= FF_DEPTH;
    }
    ndef = a->nodes - a->defined;
    q = b + F_HDR;
    for (i = a->defined; i < a->nodes; i++, q += F_DEF) {
        p_le32(q, i);
        p_le32(q + 4, nd[i].parent);
        p_le64(q + 8, nd[i].fn);
    }
    for (i = 0; i < a->ndirty; i++) {
        uint32_t node = dirty[i];
        func_row *r = &rw[node];
        uint16_t rf = (uint16_t)(r->flags & 0x1F);
        if (node == F_LOST)
            rf |= FR_LOST;
        if (r->calls || r->incl || r->child || r->open || rf) {
            memset(q, 0, F_ROW);
            p_le32(q, node);
            p_le16(q + 4, rf);
            p_le32(q + 8, r->open);
            p_le64(q + 16, r->calls);
            p_le64(q + 24, r->incl);
            p_le64(q + 32, r->child);
            q += F_ROW;
            nrows++;
        }
        memset(r, 0, sizeof *r);
    }
    a->interval++;
    memset(b, 0, F_HDR);
    p_le16(b, 1);
    p_le16(b + 2, (uint16_t)(flags | FF_DATA | a->iflags));
    p_le32(b + 4, a->incarnation);
    p_le64(b + 8, a->interval);
    p_le64(b + 16, epoch);
    p_le64(b + 32, a->start);
    p_le64(b + 40, tick);
    p_le32(b + 48, ndef);
    p_le32(b + 52, nrows);
    p_le32(b + 56, a->depth);
    p_le32(b + 60, a->suppressed);
    for (i = 0; i < FC_N; i++)
        p_le64(b + 64 + 8 * i, a->cnt[i]);
    a->pub_len = (uint64_t)(q - b);
    a->defined = a->nodes;
    a->ndirty = 0;
    a->start = tick;
    a->iflags = 0; /* STACK_UNKNOWN is set again by the next unknown-caller entry */
    memset(a->cnt, 0, sizeof a->cnt);
}

/* ---- the drainer ------------------------------------------------------------------------------------------------ */

static uint8_t *payload(prof *p) { return p->dr.buf + 16; }

static int emit_config(prof *p)
{
    func_capture *f = p->func;
    uint8_t *b = payload(p);
    if (f->configured)
        return 0;
    memset(b, 0, 48);
    p_le16(b, 1);
    p_le16(b + 2, 1);
    p_le32(b + 4, (uint32_t)sizeof(void *));
    p_le32(b + 8, f->cfg.max_actors);
    p_le32(b + 12, f->cfg.node_capacity);
    p_le32(b + 16, f->cfg.stack_depth);
    p_le32(b + 20, f->hash_slots);
    p_le32(b + 24, F_MAX_PROBE);
    p_le64(b + 32, p->tb_freq);
    p_le64(b + 40, f->allocated);
    if (p_func_emit_chunk(p, C_FUNC_CONFIG, b, 48))
        return -1;
    f->configured = 1;
    return 0;
}

/* An unacknowledged published buffer: 1 written and acknowledged, 0 none, -1 sink failure (left unacknowledged). */
static int emit_pending(prof *p, func_actor *a, uint64_t req)
{
    func_capture *f = p->func;
    uint64_t pub = LD_ACQ(&a->pub);
    uint8_t *b = payload(p);
    uint16_t fl;
    if (pub == a->d_acked)
        return 0;
    memcpy(b, f_buf(f, a), (size_t)a->pub_len); /* pub_len was written before the RELEASE of pub */
    fl = (uint16_t)(b[2] | b[3] << 8);
    if (pub < req)
        fl |= FF_LAGGING;
    p_le16(b + 2, fl);
    p_le64(b + 24, req);
    if (p_func_emit_chunk(p, C_FUNC_TREE, b, (size_t)a->pub_len))
        return -1;
    a->d_acked = pub;
    ST_REL(&a->ack, pub); /* the owner may reuse the buffer only after this */
    return 1;
}

static int emit_nodata(prof *p, uint32_t inc, uint64_t req, uint16_t flags)
{
    uint8_t *b = payload(p);
    memset(b, 0, F_HDR);
    p_le16(b, 1);
    p_le16(b + 2, flags);
    p_le32(b + 4, inc);
    p_le64(b + 24, req);
    return p_func_emit_chunk(p, C_FUNC_TREE, b, F_HDR);
}

/* The actor is quiesced (its ack/retire word was acquired): the drainer takes the final cut itself. */
static void emit_final(prof *p, func_actor *a, uint64_t req)
{
    func_capture *f = p->func;
    if (emit_pending(p, a, req) < 0)
        return;
    p_func_cut(f, a, a->final_set ? a->final_tick : a->start, 0, FF_FINAL);
    {
        uint8_t *b = payload(p);
        memcpy(b, f_buf(f, a), (size_t)a->pub_len);
        p_le64(b + 24, req);
        if (p_func_emit_chunk(p, C_FUNC_TREE, b, (size_t)a->pub_len))
            return;
    }
    a->d_done = 1;
}

static void emit_status(prof *p, uint64_t req, uint16_t flags)
{
    func_capture *f = p->func;
    uint8_t *b = payload(p);
    uint64_t u = LD_RLX(&f->unregistered), n = LD_RLX(&f->no_actor);
    memset(b, 0, 32);
    p_le16(b, 1);
    p_le16(b + 2, (uint16_t)(flags | (LD_RLX(&f->status_sat) ? 2u : 0u))); /* sticky: lower bounds from then on */
    p_le64(b + 8, req);
    p_le64(b + 16, u - f->rep_unreg);
    p_le64(b + 24, n - f->rep_noactor);
    if (p_func_emit_chunk(p, C_FUNC_STATUS, b, 32))
        return;
    f->rep_unreg = u;
    f->rep_noactor = n;
}

static func_actor *drained_actor(prof *p, uint32_t slot, uint32_t inc)
{
    func_actor *a = p_func_actor(p->func, slot);
    /* live and incarnation were written under thread_mutex before the slot became LIVE */
    return a && a->live && a->incarnation == inc && !a->d_done ? a : NULL;
}

/* An ordinary drain, after the records and before retirement frees any slot. */
void p_func_drain(prof *p, const snap *s, uint32_t n)
{
    func_capture *f = p->func;
    uint64_t req;
    uint32_t i;
    if (!f || p->snk.failed || emit_config(p))
        return;
    req = LD_RLX(&f->epoch); /* the drainer is its only writer */
    for (i = 0; i < n; i++) {
        func_actor *a = drained_actor(p, s[i].slot, s[i].incarnation);
        int r;
        if (!a)
            continue;
        if (s[i].retire_now) {   /* the ack/retire word was acquired by the snapshot */
            emit_final(p, a, req);
            continue;
        }
        r = emit_pending(p, a, req);
        if (r < 0)
            return;
        if (r == 0 && req > a->enroll_epoch && a->d_acked < req)
            (void)emit_nodata(p, a->incarnation, req, FF_LAGGING); /* no hook since the request: never zero */
    }
    if (p->snk.failed)
        return;
    /* The next cut request.  Exhaustion rule: the epoch never passes UINT64_MAX - 1; from then on no further cut is
     * requested (owners keep accumulating into their FINAL) and FUNC_STATUS carries SATURATED.  An interval number
     * is at most the number of answered epochs plus the FINAL, so it cannot pass the epoch bound. */
    if (req < UINT64_MAX - 1) {
        ST_REL(&f->epoch, req + 1);
        req++;
    } else {
        ST_RLX(&f->status_sat, 1u);
    }
    emit_status(p, req, 0);
}

/* Sealing, after the frontier's records: FINAL for every acknowledged incarnation, INCOMPLETE_TAIL otherwise. */
void p_func_seal(prof *p)
{
    func_capture *f = p->func;
    drainer *d = &p->dr;
    uint64_t req;
    uint32_t i;
    if (!f || p->snk.failed || f->final_status || emit_config(p))
        return;
    req = LD_RLX(&f->epoch);
    for (i = 0; i < d->nfront; i++) {
        const fentry *e = &d->front[i];
        func_actor *a;
        if (e->done)
            continue;
        a = drained_actor(p, e->slot, e->incarnation);
        if (!a)
            continue;
        if (e->cls == ST_ACKED || e->cls == ST_RETIRED) {
            emit_final(p, a, req);
        } else if (emit_pending(p, a, req) >= 0) {
            (void)emit_nodata(p, a->incarnation, req, FF_LAGGING | FF_INCOMPLETE);
            a->d_done = 1;
        }
    }
    if (p->snk.failed)
        return;
    emit_status(p, req, 1);
    f->final_status = 1;
}
