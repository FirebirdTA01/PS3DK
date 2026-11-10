/*
 * PS3 Custom Toolchain - libgcm_cmd / ps3tc_fifo_observe.h
 *
 * PRIVATE bridge between the observed FIFO wrap (ps3tc_fifo_wrap.c built with
 * PS3TC_FIFO_OBSERVE, archive libgcm_cmd_prof_fifo.a) and the profiler's FIFO
 * observer (sdk/libps3dkprof/src/fifo).  Not installed and not part of the
 * public GCM context ABI: the installed gcmContextData layout is unchanged.
 * Contract: docs/design/ps3dk-prof-fifo.md.
 *
 * Ownership.  Registration and removal happen on the context owner (the one
 * thread that emits into the context and therefore runs the wrap callback),
 * while the context is quiescent.  The registration is copied by value at
 * install and is immutable until removal, so the callback path reads plain
 * fields: every access is on that one thread.
 *
 * The callback receives a const scalar snapshot of one protocol boundary.  It
 * returns nothing, so it cannot change the reserve outcome, and it must take
 * no lock, make no SDK call and allocate nothing.  A boundary reached while the
 * observer itself runs (a reentrant wrap) is not delivered; it is counted in
 * `suppressed` instead.
 */
#ifndef PS3TC_FIFO_OBSERVE_H
#define PS3TC_FIFO_OBSERVE_H

#include <stdint.h>

/* Boundary kinds, in protocol order. */
#define PS3TC_FIFO_B_WRAP_ENTER   1u
#define PS3TC_FIFO_B_PHASE1_BEGIN 2u
#define PS3TC_FIFO_B_PHASE1_END   3u
#define PS3TC_FIFO_B_PHASE2_BEGIN 4u
#define PS3TC_FIFO_B_PHASE2_END   5u
#define PS3TC_FIFO_B_WRAP_EXIT    6u

/* One boundary.  tb_before/tb_after bracket the boundary on the PPU timebase:
 *   WRAP_ENTER   one read after the callback's checks, before the tail JUMP store;
 *   PHASEn_BEGIN before the PUT store .. after the barrier that follows it;
 *   PHASEn_END   after the last wait iteration .. after the successful GET check;
 *   WRAP_EXIT    one read after current was rewound to begin.
 * Offsets are IO offsets.  target_off: tail (phase 1), begin (phase 2), 0 for
 * ENTER/EXIT; current_off: tail, or begin at EXIT. */
typedef struct ps3tc_fifo_boundary {
    uint32_t size;               /* sizeof(ps3tc_fifo_boundary) */
    uint16_t kind;               /* PS3TC_FIFO_B_* */
    uint16_t flags;              /* 0 */
    uint64_t tb_before, tb_after;
    uint32_t begin_off, tail_off, target_off, requested_words, current_off, reserved;
} ps3tc_fifo_boundary;

typedef void (*ps3tc_fifo_observe_fn)(void *user, const ps3tc_fifo_boundary *b);

typedef struct ps3tc_fifo_observer {
    uint32_t size;               /* sizeof(ps3tc_fifo_observer) */
    uint32_t reserved;           /* 0 */
    void *user;
    ps3tc_fifo_observe_fn event;
} ps3tc_fifo_observer;

/* The bridge's per-process state (one observed context). */
typedef struct ps3tc_fifo_obs_state {
    ps3tc_fifo_observer reg;     /* reg.event == NULL: nothing installed */
    const void *ctx;
    uint32_t busy;               /* inside reg.event: further boundaries are suppressed */
    uint32_t pad;
    uint64_t suppressed;
} ps3tc_fifo_obs_state;

/* The wrap being observed: frozen at entry. */
typedef struct ps3tc_fifo_obs_wrap {
    uint32_t begin_off, tail_off, requested_words, pad;
} ps3tc_fifo_obs_wrap;

/* Deliver one boundary.  Shared by the PPU wrap and the host model, so the
 * kind-specific target/current rules and the reentry guard are tested once. */
static inline void ps3tc_fifo_obs_emit(ps3tc_fifo_obs_state *s, unsigned kind, uint64_t tb_before,
                                       uint64_t tb_after, const ps3tc_fifo_obs_wrap *w)
{
    ps3tc_fifo_boundary b;
    if (!s->reg.event)
        return;
    if (s->busy) {
        if (s->suppressed != UINT64_MAX)
            s->suppressed++;
        return;
    }
    b.size = (uint32_t)sizeof b;
    b.kind = (uint16_t)kind;
    b.flags = 0;
    b.tb_before = tb_before;
    b.tb_after = tb_after;
    b.begin_off = w->begin_off;
    b.tail_off = w->tail_off;
    b.requested_words = w->requested_words;
    b.target_off = (kind == PS3TC_FIFO_B_PHASE1_BEGIN || kind == PS3TC_FIFO_B_PHASE1_END) ? w->tail_off
                 : (kind == PS3TC_FIFO_B_PHASE2_BEGIN || kind == PS3TC_FIFO_B_PHASE2_END) ? w->begin_off : 0u;
    b.current_off = kind == PS3TC_FIFO_B_WRAP_EXIT ? w->begin_off : w->tail_off;
    b.reserved = 0;
    s->busy = 1;
    s->reg.event(s->reg.user, &b);
    s->busy = 0;
}

/* Install/remove bookkeeping, shared by the PPU bridge and the host model.
 * begin_off/end_off are the IO offsets the caller translated for ctx->begin
 * and ctx->end, ea_bytes the effective-address extent end - begin.  The ring
 * must be 4-aligned, nonempty, below the JUMP bit and contiguous in IO. */
static inline int ps3tc_fifo_obs_install(ps3tc_fifo_obs_state *s, const void *ctx, const ps3tc_fifo_observer *obs,
                                         uint32_t begin_off, uint32_t end_off, uint64_t ea_bytes,
                                         uint32_t *out_begin, uint32_t *out_end)
{
    if (!ctx || !obs || !out_begin || !out_end || obs->size != (uint32_t)sizeof *obs || obs->reserved ||
        !obs->event || s->reg.event)
        return -1;
    if (((begin_off | end_off) & 3u) || begin_off >= end_off || end_off >= 0x20000000u ||
        (uint64_t)(end_off - begin_off) != ea_bytes)
        return -1;
    s->reg = *obs;
    s->ctx = ctx;
    *out_begin = begin_off;
    *out_end = end_off;
    return 0;
}

static inline int ps3tc_fifo_obs_remove(ps3tc_fifo_obs_state *s, const void *ctx, const ps3tc_fifo_observer *obs)
{
    if (!ctx || !obs || s->ctx != ctx || !s->reg.event || s->reg.event != obs->event || s->reg.user != obs->user ||
        s->busy)
        return -1;
    s->reg.event = (ps3tc_fifo_observe_fn)0;
    s->reg.user = (void *)0;
    s->ctx = (const void *)0;
    return 0;
}

/* Bridge entry points, defined only by the observed archive (and the host
 * model): linking the profiler's FIFO extension against the default
 * libgcm_cmd.a leaves them undefined, so that mistake fails at link time.
 *
 * install: 0 with the out offsets set to the validated IO offsets of
 * ctx->begin and ctx->end; -1 (nothing changed) for a NULL or bad argument, an
 * unmapped or misaligned ring, or an observer already installed.  remove: 0
 * when obs was the installed registration for ctx, else -1. */
struct _gcmCtxData;
int ps3tc_fifo_observe_install(struct _gcmCtxData *ctx, const ps3tc_fifo_observer *obs,
                               uint32_t *begin_off, uint32_t *end_off);
int ps3tc_fifo_observe_remove(struct _gcmCtxData *ctx, const ps3tc_fifo_observer *obs);
uint64_t ps3tc_fifo_observe_suppressed(void);

#endif /* PS3TC_FIFO_OBSERVE_H */
