/* The -finstrument-functions hooks (docs/design/ps3dk-prof-func-paths.md).  Owner-thread only: no lock, no
 * allocation, no shared write except the release publication of a completed cut.  This file, like the whole
 * library, is never compiled with -finstrument-functions; the attribute keeps that true for the hooks themselves. */
#include "func_internal.h"
#include <string.h>

#define NOI __attribute__((no_instrument_function))

void __cyg_profile_func_enter(void *fn, void *site) NOI;
void __cyg_profile_func_exit(void *fn, void *site) NOI;

static NOI uint64_t hkey(uint32_t parent, uint64_t fn)
{
    uint64_t h = (fn ^ ((uint64_t)parent << 32 | parent)) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

/* (parent, fn) -> node, creating it while capacity lasts; F_LOST on a full table or an exhausted probe. */
static NOI uint32_t lookup(func_capture *f, func_actor *a, uint32_t parent, uint64_t fn)
{
    uint32_t *ht = f_hash(f, a), mask = f->hash_slots - 1, k;
    func_node *nd = f_nodes(f, a);
    uint64_t h = hkey(parent, fn);
    for (k = 0; k < F_MAX_PROBE; k++) {
        uint32_t *e = &ht[(h + k) & mask];
        if (*e == 0) {
            if (a->nodes >= f->cfg.node_capacity)
                return F_LOST;
            *e = a->nodes;
            nd[a->nodes].fn = fn;
            nd[a->nodes].parent = parent;
            return a->nodes++;
        }
        if (nd[*e].fn == fn && nd[*e].parent == parent)
            return *e;
    }
    return F_LOST;
}

/* Stack loss: drop every retained activation without charging anything after its cursor. */
static NOI void reset(func_capture *f, func_actor *a)
{
    func_frame *fr = f_frames(f, a);
    for (uint32_t i = 0; i < a->depth; i++) {
        f_touch(f, a, fr[i].node);
        f_rows(f, a)[fr[i].node].flags |= FR_TRUNC;
    }
    a->depth = 0;
    a->suppressed = 0;
    a->unknown = 1;
    a->iflags |= FF_RESET;
    f_count(a, FC_UNBAL);
}

static NOI void enter(func_capture *f, func_actor *a, uint64_t fn, uint64_t frame, uint64_t now)
{
    func_frame *fr = f_frames(f, a), *top = a->depth ? &fr[a->depth - 1] : NULL;
    func_row *rw = f_rows(f, a);
    uint32_t parent, node, sat = 0, nested = 0;
    if (top && frame >= top->frame) {
        /* a callee's frame is always below its caller's: the stack was left by a nonlocal exit */
        reset(f, a);
        top = NULL;
    }
    if (a->suppressed || a->depth == f->cfg.stack_depth) {
        if (a->suppressed != UINT32_MAX)
            a->suppressed++;
        f_count(a, FC_DEPTH);
        a->iflags |= FF_DEPTH;
        f_touch(f, a, top->node);
        rw[top->node].flags |= FR_SELF_UNTIMED;
        return;
    }
    if (top) {
        parent = top->node;
    } else {
        parent = a->unknown ? F_UNKNOWN_CALLER : F_ROOT;
        if (a->unknown)
            a->iflags |= FF_UNKNOWN;
    }
    if (top && top->node == F_LOST) {
        node = F_LOST;
        nested = 1; /* inside a LOST activation: calls only, its time is already in the outer one */
    } else {
        node = lookup(f, a, parent, fn);
    }
    if (node == F_LOST) {
        f_count(a, FC_LOST);
        a->iflags |= FF_LOST;
    }
    f_touch(f, a, node);
    f_add(&rw[node].calls, 1, &sat);
    if (sat)
        rw[node].flags |= FR_SAT, a->iflags |= FF_SAT;
    fr = &fr[a->depth++];
    fr->fn = fn;
    fr->from = now;
    fr->child = 0;
    fr->frame = frame;
    fr->node = node;
    fr->nested = nested;
}

static NOI void leave(func_capture *f, func_actor *a, uint64_t fn, uint64_t frame, uint64_t now)
{
    func_frame *fr = f_frames(f, a), *top;
    func_row *r;
    uint64_t el = 0;
    uint32_t sat = 0;
    if (a->suppressed) {
        /* refused levels are deeper than the top retained activation: never pop a retained frame for them */
        if (frame >= fr[a->depth - 1].frame)
            reset(f, a);
        else
            a->suppressed--;
        return;
    }
    if (!a->depth) {
        f_count(a, FC_ORPHAN);
        return;
    }
    top = &fr[a->depth - 1];
    if (top->fn != fn || frame > top->frame) {
        reset(f, a); /* nonlocal exit or mismatch: invalidate, never guess */
        return;
    }
    a->depth--;
    if (top->nested)
        return;
    if (now >= top->from)
        el = now - top->from;
    else
        f_count(a, FC_CLOCK), a->iflags |= FF_CLOCK;
    r = &f_rows(f, a)[top->node];
    f_touch(f, a, top->node);
    f_add(&r->incl, el, &sat);
    f_add(&r->child, top->child < el ? top->child : el, &sat);
    if (sat)
        r->flags |= FR_SAT, a->iflags |= FF_SAT;
    if (a->depth) {
        sat = 0;
        f_add(&fr[a->depth - 1].child, el, &sat);
        if (sat)
            a->iflags |= FF_SAT;
    }
}

/* A new cut request: publish a cut at this hook's tick unless the previous buffer is still unacknowledged. */
static NOI void maybe_cut(func_capture *f, func_actor *a, uint64_t now)
{
    uint64_t e = LD_RLX(&f->epoch);
    if (e == a->last_pub)
        return;
    if (LD_ACQ(&a->ack) != a->last_pub) {
        if (a->deferred_epoch != e) {
            a->deferred_epoch = e;
            f_count(a, FC_DEFER); /* keep accumulating; the next cut covers the longer interval */
        }
        return;
    }
    p_func_cut(f, a, now, e, 0);
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof__func_test_hook)
        ps3dk_prof__func_test_hook(1);
#endif
    a->last_pub = e;
    ST_REL(&a->pub, e); /* the buffer and pub_len are complete */
}

static NOI void hook(uint64_t fn, uint64_t frame, int is_enter)
{
    tls_state *t = &g_tls;
    prof *p;
    func_capture *f;
    func_actor *a;
    uint64_t now;
    if (t->func_busy) {
        if (t->func_reentry != UINT32_MAX)
            t->func_reentry++;
        return;
    }
    if (LD_ACQ(&g_gate) != G_READY)
        return;
    p = g_prof;
    f = p->func;
    if (!f || t->in_drain || t->acked || t->retired)
        return;
    t->func_busy = 1;
    if (!t->slot1) {
        if (is_enter && p_func_auto_register(p) != 0)
            __atomic_fetch_add(&f->unregistered, 1, __ATOMIC_RELAXED);
        if (!t->slot1)
            goto out;
    }
    if (LD_ACQ(&p->stop_latch)) {
        p_observe_stop(p); /* stamps the final tick, then the core's release ack */
        goto out;
    }
    a = p_func_actor(f, t->slot1 - 1);
    if (!a || !a->live || a->incarnation != t->incarnation) {
        if (is_enter)
            __atomic_fetch_add(&f->no_actor, 1, __ATOMIC_RELAXED);
        goto out;
    }
    now = p_tb();
    maybe_cut(f, a, now);
    if (t->func_reentry) {
        uint32_t sat = 0;
        f_add(&a->cnt[FC_REENTRY], t->func_reentry, &sat);
        if (sat)
            a->iflags |= FF_SAT;
        t->func_reentry = 0;
    }
    if (is_enter)
        enter(f, a, fn, frame, now);
    else
        leave(f, a, fn, frame, now);
out:
    t->func_busy = 0;
}

/* The activation's frame: the instrumented caller's frame address (the PPU back chain; on the host the caller's
 * frame pointer, so host tests build with -fno-omit-frame-pointer). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
void __cyg_profile_func_enter(void *fn, void *site)
{
    (void)site;
    hook((uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)__builtin_frame_address(1), 1);
}
void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)site;
    hook((uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)__builtin_frame_address(1), 0);
}
#pragma GCC diagnostic pop
