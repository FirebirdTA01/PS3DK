/* Capture RSX frame, phase B1, slice S2: the runtime core (INTERNAL; not installed).  One request slot per process,
 * immutable per-cap records, the static descriptor pool and its tag formatter, the agent-side API (S4 calls it) and
 * the hook-side API (S3 calls it from librsx_prof.a).  Host builds only in S2.
 *
 * The slot word (one atomic u64): state bits 0-7, flags bits 8-15 (CANCEL, STOP), epoch bits 16-47 (= cap, 1..16,
 * never reused), and in TERMINAL only the outcome (bits 48-55) and its reason (bits 56-63); bits 48-63 are zero in
 * every other state.  Every transition by every actor is a CAS of the whole word from the exact word that actor
 * loaded; STOP is process-sticky (never cleared); CANCEL belongs to the cap in the epoch field and is dropped only by
 * the agent's IDLE(e) -> CLAIMING(e + 1) claim.  A CAS that fails is re-examined: a flags-only change is retried
 * within a fixed bound, a state or epoch change stops the actor (another actor owns the slot), and passing the bound
 * fails closed (TERMINAL or a refusal, never a spin).  No timers, sleeps or timeouts anywhere: every loop carries
 * its bound.
 *
 * rec[cap - 1] holds the agent's fields (written in CLAIMING, before its publication CAS, together with zeroed
 * telemetry), the owner binding (written in ARMING, before the ARMED publication; the BEGIN receipt is written while
 * ARMING) and the frozen counters and END receipt (written while CLOSING, before the TERMINAL publication).  A record
 * is never written again after its TERMINAL publication; the outcome lives in the TERMINAL word.  Contested
 * REQUESTED -> TERMINAL / IDLE transitions write nothing shared. */
#ifndef PS3DK_PROF_RSXCAP_H
#define PS3DK_PROF_RSXCAP_H

#include <stddef.h>
#include <stdint.h>

#include "pal.h"
#include "prg.h"
#include "prof_gpu.h"

#define rsxcap_fmt_begin ps3dk_prof__rsxcap_fmt_begin
#define rsxcap_fmt_ckpt ps3dk_prof__rsxcap_fmt_ckpt
#define rsxcap_fmt_end ps3dk_prof__rsxcap_fmt_end
#define rsxcap_emit_begin ps3dk_prof__rsxcap_emit_begin
#define rsxcap_emit_ckpt ps3dk_prof__rsxcap_emit_ckpt
#define rsxcap_emit_end ps3dk_prof__rsxcap_emit_end

/* slot states */
enum { RSXCAP_IDLE = 0, RSXCAP_CLAIMING = 1, RSXCAP_REQUESTED = 2, RSXCAP_ARMING = 3, RSXCAP_ARMED = 4,
       RSXCAP_CLOSING = 5, RSXCAP_TERMINAL = 6 };
#define RSXCAP_F_CANCEL 0x100ull
#define RSXCAP_F_STOP 0x200ull
#define RSXCAP_FLAGS 0xff00ull
#define RSXCAP_STATE(w) ((uint32_t)((w)&0xffu))
#define RSXCAP_EPOCH(w) ((uint32_t)(((w) >> 16) & 0xffffffffu))
#define RSXCAP_OUTCOME(w) ((uint32_t)(((w) >> 48) & 0xffu))
#define RSXCAP_REASON(w) ((uint32_t)((w) >> 56))
#define RSXCAP_WORD(st, flags, epoch, out, rsn)                                                                        \
    ((uint64_t)(st) | ((uint64_t)(flags)&RSXCAP_FLAGS) | ((uint64_t)(epoch) << 16) | ((uint64_t)(out) << 48) |        \
     ((uint64_t)(rsn) << 56))

/* outcomes: DONE / REFUSED / ABANDONED live in the TERMINAL word; the rest are reported by query only */
enum { RSXCAP_O_NONE = 0, RSXCAP_O_DONE = 1, RSXCAP_O_REFUSED = 2, RSXCAP_O_ABANDONED = 3,
       RSXCAP_O_WITHDRAWN = 4,     /* agent-private: the agent's own REQUESTED -> IDLE withdrawal */
       RSXCAP_O_CANCELLED = 5,     /* agent-private: cancel won in REQUESTED */
       RSXCAP_O_ARMED_STOPPED = 6, /* query of ARMED + STOP with no owner progress (a local ABANDONED(stopping)) */
       RSXCAP_O_ARMED_FAULTED = 7  /* query of ARMED after an extension fault (a local ABANDONED(fault)) */ };
enum { RSXCAP_R_NONE = 0, RSXCAP_R_BUSY = 1, RSXCAP_R_CAPACITY = 2, RSXCAP_R_NO_CAPABILITY = 3,
       RSXCAP_R_BAD_DOMAIN = 4, RSXCAP_R_DOMAIN_FAULTED = 5, RSXCAP_R_STOPPING = 6, RSXCAP_R_DETACH = 7,
       RSXCAP_R_DOMAIN_DETACHED = 8, RSXCAP_R_DOMAIN_STOPPED = 9, RSXCAP_R_DESCRIPTOR = 10,
       RSXCAP_R_BEGIN_NOT_ENQUEUED = 11, RSXCAP_R_BEGIN_PUBLISH_FAULT = 12, RSXCAP_R_END_DESCRIPTOR = 13,
       RSXCAP_R_END_NOT_ENQUEUED = 14, RSXCAP_R_FAULT = 15, RSXCAP_R_HOOK_FAULT = 16 };

/* request results (besides the negative PS3DK_PROF_E* call errors) */
enum { RSXCAP_ACCEPTED = 0, RSXCAP_REFUSED = 1 };

#define RSXCAP_CAPS 16u
#define RSXCAP_CKPT_SLOTS 64u                        /* pool checkpoint slots per cap; at most 63 are used */
#define RSXCAP_PER_CAP (2u + RSXCAP_CKPT_SLOTS)      /* BEGIN, END, checkpoints */
#define RSXCAP_POOL (RSXCAP_CAPS * RSXCAP_PER_CAP)   /* 1056 */
#define RSXCAP_BEGIN_BYTES 128u
#define RSXCAP_END_BYTES 496u
#define RSXCAP_CKPT_BYTES 112u
#define RSXCAP_POOL_FILE "<ps3dk-rsxcap>"
#define RSXCAP_KIND_LINE 2001u
#define RSXCAP_DRAW_CAP 262144u
#define RSXCAP_BLOCK 4096u
#define RSXCAP_SIDE_MAX 262144u
#define RSXCAP_SAT16 65535u
#define RSXCAP_BLOCKS_MAX 63u /* checkpoint blocks 0..62 */

/* the draw kinds (the fixture's `draw` records) */
enum { RSXCAP_K_ARRAYS = 0, RSXCAP_K_INDEX, RSXCAP_K_INLINE, RSXCAP_K_CLEAR, RSXCAP_K_CLEAR_ZCULL, RSXCAP_KINDS };
/* ps3dk_prof__rsxcap_count: which */
enum { RSXCAP_C_UNSAFE = 0, RSXCAP_C_NO_SPACE = 1, RSXCAP_C_REENTRANT = 2, RSXCAP_C_INVALID = 3,
       RSXCAP_C_MISMATCH = 4, RSXCAP_C_IMMEDIATE = 5 };

/* the other-thread word: count bits 0-15, SAT bit 16, FROZEN bit 31 */
#define RSXCAP_OT_SAT 0x10000u
#define RSXCAP_OT_FROZEN 0x80000000u
#define RSXCAP_OT_ATTEMPTS 4u

typedef struct rsxcap_ident {
    uint32_t cap, conn, wire, dom, owner, pad;
    uint64_t frame;
} rsxcap_ident;

typedef struct rsxcap_blk {
    uint32_t ev, lost, sup, ref; /* one block's zone BEGIN and END receipt counts */
} rsxcap_blk;

typedef struct rsxcap_end {
    rsxcap_ident id;
    uint32_t block;
    rsxcap_blk c;
    uint32_t hooks, capped, other_ctx, other_ctx_sat, other_thread, other_thread_sat, immediate, immediate_sat;
    uint32_t ckpt_missing, unsafe_draws, no_space, reentrant, invalid, mismatch, counts_sat, hook_fault;
} rsxcap_end;

/* The formatter (validated): the tag of a marker, or a refusal that writes nothing.  The emitter (unvalidated
 * grammar output, its fixed field order): succeeds iff the payload plus its NUL fits the capacity; a refusal writes
 * nothing at all.  Both return the payload length, or a negative RSXCAP_FMT_*. */
enum { RSXCAP_FMT_ECAP = -1, RSXCAP_FMT_EVALUE = -2, RSXCAP_FMT_ERANGE = -3 };
int rsxcap_emit_begin(char *buf, size_t capacity, const rsxcap_ident *id);
int rsxcap_emit_ckpt(char *buf, size_t capacity, uint32_t cap, uint32_t block, const rsxcap_blk *c);
int rsxcap_emit_end(char *buf, size_t capacity, const rsxcap_end *e);
int rsxcap_fmt_begin(char *buf, size_t capacity, const rsxcap_ident *id);
int rsxcap_fmt_ckpt(char *buf, size_t capacity, uint32_t cap, uint32_t block, const rsxcap_blk *c);
int rsxcap_fmt_end(char *buf, size_t capacity, const rsxcap_end *e);

/* one per cap */
typedef struct rsxcap_rec {
    /* agent (CLAIMING) */
    uint32_t conn, wire, dom, tslot; /* tslot: the table slot of the domain, pinned through the claim */
    /* owner binding (ARMING) */
    uint32_t bound, owner;
    uint64_t frame;
    const void *ctx;
    pal_owner thread;
    prg_receipt begin_r;
    /* frozen at CLOSING */
    prg_receipt end_r;
    rsxcap_end end;      /* the END fields (also when no END was written: the frozen counts) */
    uint32_t ckpts;      /* checkpoints whose MARK was submitted */
    uint32_t frozen;     /* 1 once the counters below and end are final */
    /* NOT part of the immutable bytes: the other-thread word (atomic) */
    uint32_t other;
    uint32_t pad;
} rsxcap_rec;
#define RSXCAP_REC_IMMUTABLE_BYTES offsetof(rsxcap_rec, other)

typedef struct rsxcap_outcome {
    uint64_t word;      /* the raw slot word as loaded (epoch may name a later cap) */
    uint32_t cap, state, flags, outcome, reason, cached;
    uint32_t conn, wire, dom, pad;
    prg_receipt begin_r, end_r;
    rsxcap_end end;     /* the frozen counts (TERMINAL) */
} rsxcap_outcome;

typedef struct rsxcap_draw {
    uint32_t open, cap, kind, desc;
    prg_token tok;
} rsxcap_draw;

/* step points (test builds call ps3dk_prof_test_hooks.rsxcap_step; compiled out otherwise) */
enum { RT_AFTER_PREPARE = 1, RT_AFTER_SLOT_LOAD, RT_AFTER_CLAIM, RT_AFTER_DESC, RT_BEFORE_ARMED_PUB,
       RT_BEFORE_CLOSING, RT_AFTER_FREEZE, RT_AFTER_END_DESC, RT_BEFORE_TERMINAL, HK_AFTER_SLOT_LOAD,
       HK_OTHER_BEFORE_CAS, AG_BEFORE_CLAIM_CAS, AG_CLAIMING, AG_PUB_LOAD, AG_AFTER_PUBLICATION, AG_BEFORE_CHECK,
       AG_WITHDRAW_LOAD, AG_CANCEL_LOAD, AG_BEFORE_RECYCLE, AG_AFTER_SCAN, DT_BEFORE_SETTLE, DT_AFTER_LOAD,
       RT_ARMED_CAS, RT_CLOSING_CAS, RT_TERMINAL_CAS, AG_AFTER_CLAIM_CAS,
       /* one hit per CAS attempt of each publisher (the attempt counts of the bound tests) */
       RT_CLAIM_CAS, AG_CLAIM_CAS, AG_PUB_CAS, AG_WITHDRAW_CAS, AG_CANCEL_CAS, AG_RECYCLE_CAS, DT_SETTLE_CAS,
       AG_SERVICE_CAS, RSXCAP_STEPS };

/* ---- agent side (S4 calls; one agent thread, caller-serialized) ---- */
/* RSXCAP_ACCEPTED with *cap; RSXCAP_REFUSED with *reason and *cap (0 before a claim, the claimed cap after it);
 * EBUSY inside a drain, a sink callback or a GPU call (no lock, no pin, slot unchanged); ENOTINIT; ESTOPPED when
 * the agent's closing unlock failed after its publication (*cap = the claimed cap, diagnostic). */
int ps3dk_prof__rsxcap_request(uint32_t conn, uint32_t wire, uint32_t domain, uint32_t *cap, uint32_t *reason);
int ps3dk_prof__rsxcap_cancel(uint32_t cap);   /* OK; EINVAL (stale or nothing to cancel); EBUSY past its bound */
int ps3dk_prof__rsxcap_query(uint32_t cap, rsxcap_outcome *out); /* OK; EINVAL (stale cap with no cached result) */
int ps3dk_prof__rsxcap_recycle(uint32_t cap);  /* TERMINAL(cap) -> IDLE(cap) only; else EINVAL */
/* ---- hook side (S3 calls from librsx_prof.a) ---- */
extern uint64_t ps3dk_prof__rsxcap_slot; /* the slot word; the not-armed check is one acquire load */
int ps3dk_prof__rsxcap_draw_begin(ps3dk_prof_gcm_ctx *ctx, uint32_t kind, rsxcap_draw *dr); /* 1: a zone is open */
void ps3dk_prof__rsxcap_draw_end(rsxcap_draw *dr);
void ps3dk_prof__rsxcap_draw_abandon(rsxcap_draw *dr); /* untrusted body: abandon, mismatch++, hook fault, settle */
void ps3dk_prof__rsxcap_count(ps3dk_prof_gcm_ctx *ctx, uint32_t which);
/* ---- the process opt-in, before any GPU attach (OQ-5, R82) ---- */
int ps3dk_prof__rsxcap_enable(void);

/* contract accessors (the pool and the kind table, as registered) */
const ps3dk_prof_desc *ps3dk_prof__rsxcap_pool(uint32_t *n);
const ps3dk_prof_desc *ps3dk_prof__rsxcap_kinds(uint32_t *n);

#endif
