/* FIFO wrap phase events: attach, detach and the owner callback (docs/design/ps3dk-prof-fifo.md; ownership and
 * ordering in fifo_internal.h).  Built only into the observed extension archive (fifo.mk). */
#include "fifo_internal.h"

#include <string.h>

fifo_capture g_fifo;

#ifdef PS3DK_PROF_TEST_HOOKS
void (*ps3dk_prof__fifo_test_hook)(unsigned point, unsigned v);
#endif

/* single-writer counters: a load of the owner's own value, then a RELEASE store */
static inline void bump(uint64_t *c)
{
    uint64_t v = LD_RLX(c);
    if (v != UINT64_MAX)
        ST_REL(c, v + 1u);
}

int p_gpu_fifo_owner(prof *p, uint32_t gpu, void *ctx); /* gpu_runtime.c (PS3DK_PROF_WITH_FIFO) */

/* The owner's wrap boundary.  No lock, no SDK call, no allocation, no sink work; it never changes the protocol. */
static void fifo_event(void *user, const ps3tc_fifo_boundary *b)
{
    fifo_capture *f = (fifo_capture *)user;
    prof *p = g_prof;
    tls_state *t = &g_tls;
    uint64_t seq, w, r, occ;
    uint16_t flags = 0;
    fifo_rec *e;
    f->o_in_cb = 1;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof__fifo_test_hook)
        ps3dk_prof__fifo_test_hook(1, b->kind);
#endif
    /* admission: a live instance, before the stop latch, from the owner incarnation that attached it */
    if (LD_ACQ(&f->state) != FS_ATTACHED || !p || LD_ACQ(&p->stop_latch) || b->size != (uint32_t)sizeof *b ||
        b->kind < PS3TC_FIFO_B_WRAP_ENTER || b->kind > PS3TC_FIFO_B_WRAP_EXIT || t->slot1 != f->owner_slot + 1u ||
        t->incarnation != f->owner_inc || t->acked || t->retired) {
        bump(&f->refused);
        goto out;
    }
    seq = LD_RLX(&f->attempted) + 1u;
    if (seq >= P_CEIL) {
        ST_REL(&f->saturated, 1u); /* sticky: no sequence wraps or repeats */
        bump(&f->refused);
        goto out;
    }
    ST_REL(&f->attempted, seq);
    if (b->kind == PS3TC_FIFO_B_WRAP_ENTER) {
        /* the token is frozen here: the owner's current profiler frame, an association only */
        uint64_t cf = LD_ACQ(&p->slots[f->owner_slot].cb->current_frame);
        f->o_wrap++;
        f->o_tok = cf != 0;
        f->o_tok_frame = cf ? cf - 1u : 0u;
        bump(&f->started);
        ST_REL(&f->open_wrap, f->o_wrap);
    } else if (b->kind == PS3TC_FIFO_B_WRAP_EXIT) {
        bump(&f->completed);
        ST_REL(&f->open_wrap, 0u);
    }
    if (b->tb_after < b->tb_before || (f->o_clock_seen && b->tb_before < f->o_last_tb)) {
        flags |= FE_CLOCK_INVALID; /* raw values kept; no elapsed time is derived from this event */
    } else {
        f->o_last_tb = b->tb_after;
        f->o_clock_seen = 1;
    }
    if (f->o_tok)
        flags |= FE_TOKEN;
    w = LD_RLX(&f->write_idx);
    r = LD_ACQ(&f->read_idx);
    occ = w - r;
    if (occ >= f->ring_records) {
        bump(&f->dropped); /* the attempt keeps its sequence: the reader sees the gap */
        goto out;
    }
    e = &f->ring[w % f->ring_records];
    e->seq = seq;
    e->wrap_id = f->o_wrap;
    e->tb_before = b->tb_before;
    e->tb_after = b->tb_after;
    e->frame_owner = f->o_tok ? f->owner_inc : 0u;
    e->frame_id = f->o_tok ? f->o_tok_frame : 0u;
    e->begin_off = b->begin_off;
    e->tail_off = b->tail_off;
    e->target_off = b->target_off;
    e->requested_words = b->requested_words;
    e->current_off = b->current_off;
    e->kind = b->kind;
    e->flags = flags;
    ST_REL(&f->write_idx, w + 1u); /* the slot is complete before it is published */
    if (occ + 1u > LD_RLX(&f->high_water))
        ST_REL(&f->high_water, occ + 1u);
out:
    f->o_in_cb = 0;
}

int ps3dk_prof_fifo_attach(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, const ps3dk_prof_fifo_options *o,
                           uint32_t *instance)
{
    fifo_capture *f = &g_fifo;
    prof *p;
    uint32_t rr = FIFO_RING_DEF, mb = FIFO_MAX_DEF, exp, b = 0, e = 0;
    uint64_t need;
    int rc;
    if (instance)
        *instance = 0;
    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY;
    p = p_ready() ? g_prof : NULL;
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->gpu_max)
        return PS3DK_PROF_ENOTSUP;
    /* independent progress is mandatory: the background drain thread, never the APP owner's own drains */
    if (p->cfg.mode != PS3DK_PROF_MODE_THREAD || !p->drain_thread_started)
        return PS3DK_PROF_EPERM;
    if (!ctx || !instance)
        return PS3DK_PROF_EINVAL;
    if (o) {
        if (o->size != (uint32_t)sizeof *o || o->reserved)
            return PS3DK_PROF_EINVAL;
        if (o->ring_records)
            rr = o->ring_records;
        if (o->max_bytes)
            mb = o->max_bytes;
    }
    if (rr < FIFO_RING_MIN || rr > FIFO_RING_MAX || mb < FIFO_MAX_MIN || mb > FIFO_MAX_MAX)
        return PS3DK_PROF_EINVAL;
    /* checked before any allocation: rr <= 2^16 bounds the product, the sum and the ceiling compare in 64 bits */
    need = (uint64_t)rr * FIFO_REC_BYTES;
    if (need > UINT64_MAX - FIFO_CONTROL || need + FIFO_CONTROL > mb)
        return PS3DK_PROF_EINVAL;
    need += FIFO_CONTROL;
    if (LD_ACQ(&p->shutdown_once) || LD_ACQ(&p->stop_latch))
        return PS3DK_PROF_ESTOPPED;
    if (!g_tls.slot1 || g_tls.acked || g_tls.retired)
        return PS3DK_PROF_ETHREAD;
    rc = p_gpu_fifo_owner(p, gpu, ctx);
    if (rc != PS3DK_PROF_OK)
        return rc;
    exp = FS_NONE;
    if (!CAS_ACQREL(&f->state, &exp, (uint32_t)FS_ATTACHING))
        return PS3DK_PROF_EFULL; /* one instance per capture, never reattached */
    f->block = (uint8_t *)pal_alloc((size_t)need);
    if (!f->block) {
        ST_REL(&f->state, (uint32_t)FS_NONE);
        return PS3DK_PROF_ENOMEM;
    }
    memset(f->block, 0, (size_t)need);
    if (pal_owner_get(&f->owner) != 0) {
        rc = PS3DK_PROF_ETHREAD;
        goto unwind;
    }
    f->instance = 1;
    f->generation = 1;
    f->domain = gpu;
    f->owner_inc = g_tls.incarnation;
    f->owner_slot = g_tls.slot1 - 1u;
    f->owner_gen = g_tls.generation;
    f->ring_records = rr;
    f->max_bytes = mb;
    f->allocated = (uint32_t)need;
    f->ctx = ctx;
    f->ring = (fifo_rec *)(void *)(f->block + FIFO_CONTROL);
    f->reg.size = (uint32_t)sizeof f->reg;
    f->reg.reserved = 0;
    f->reg.user = f;
    f->reg.event = fifo_event;
    if (ps3tc_fifo_observe_install((struct _gcmCtxData *)ctx, &f->reg, &b, &e) != 0) {
        rc = PS3DK_PROF_EINVAL;
        goto unwind;
    }
    f->begin_off = b;
    f->end_off = e;
    exp = FS_ATTACHING;
    if (!CAS_ACQREL(&f->state, &exp, (uint32_t)FS_ATTACHED)) {
        /* the seal claimed the instance first; it never touched the block */
        (void)ps3tc_fifo_observe_remove((struct _gcmCtxData *)ctx, &f->reg);
        pal_free(f->block);
        f->block = NULL;
        return PS3DK_PROF_ESTOPPED;
    }
    *instance = f->instance;
    return PS3DK_PROF_OK;
unwind:
    pal_free(f->block);
    f->block = NULL;
    f->ring = NULL;
    ST_REL(&f->state, (uint32_t)FS_NONE);
    return rc;
}

int ps3dk_prof_fifo_detach(uint32_t instance)
{
    fifo_capture *f = &g_fifo;
    uint32_t st = LD_ACQ(&f->state), exp;
    if (!instance || st == FS_NONE || st == FS_ATTACHING || instance != f->instance)
        return PS3DK_PROF_EINVAL;
    if (!pal_owner_is_self(&f->owner))
        return PS3DK_PROF_EPERM;
    if (f->o_in_cb)
        return PS3DK_PROF_EBUSY; /* from inside the observer callback: the wrap is still running */
    if (st == FS_DETACHED || st == FS_CLOSED)
        return PS3DK_PROF_EALREADY;
    /* the owner removes its own observer: no boundary can run after this on this thread */
    (void)ps3tc_fifo_observe_remove((struct _gcmCtxData *)f->ctx, &f->reg);
    if (st == FS_SEALED)
        return PS3DK_PROF_ESTOPPED; /* sealed without the owner's acknowledgement: the block stays (quarantined) */
    exp = FS_ATTACHED;
    if (CAS_ACQREL(&f->state, &exp, (uint32_t)FS_DETACHED))
        return PS3DK_PROF_OK; /* every owner counter was RELEASED before this; the drainer writes the exact FINAL */
    return PS3DK_PROF_ESTOPPED;
}

#ifdef PS3DK_PROF_TEST_HOOKS
void ps3dk_prof__fifo_test_seed(uint64_t attempted) { ST_REL(&g_fifo.attempted, attempted); }
uint64_t ps3dk_prof__fifo_test_read_idx(void) { return LD_ACQ(&g_fifo.read_idx); }
uint32_t ps3dk_prof__fifo_test_state(void) { return LD_ACQ(&g_fifo.state); }
#endif
