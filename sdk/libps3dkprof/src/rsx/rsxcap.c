/* Capture RSX frame, phase B1, slice S2: the runtime core (see rsxcap.h).  The request slot and its transitions, the
 * per-cap records, the static descriptor pool and the tag formatter, the agent-side and hook-side APIs, and the
 * gpu_frame / detach / shutdown entries the runtime reaches through prof->rsxcap.
 *
 * Bounds (every loop states its own): owner and detach publication CASes 3 attempts (1 + the two flag bits another
 * actor can set); the agent's claim 2 and its publication 2 (only the stopper can change CLAIMING); the agent's
 * withdrawal, cancel and recycle as stated at each loop; the other-thread counter 4 attempts; table scans gpu_max;
 * the pool initialization 1056; checkpoints at most 63 per capture (the draw cap); the formatter's fields fixed. */
#include "prof_internal.h"

#ifdef PS3DK_PROF_WITH_SPU

#include <string.h>

#include "gpu_internal.h"
#include "prof_gpu.h"
#include "rsxcap.h"
#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
#define RSTEP(n)                                                                                                       \
    do {                                                                                                               \
        if (ps3dk_prof_test_hooks.rsxcap_step)                                                                         \
            ps3dk_prof_test_hooks.rsxcap_step(n);                                                                      \
    } while (0)
#else
#define RSTEP(n) ((void)0)
#endif

#define S_LD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define S_CAS(p, e, v) __atomic_compare_exchange_n((p), (e), (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

uint64_t ps3dk_prof__rsxcap_slot;
#define SLOT (&ps3dk_prof__rsxcap_slot)

/* ---------------------------------------------------------------- the tag formatter */
typedef struct sbuf {
    char b[512];
    size_t n;
} sbuf;

static void s_str(sbuf *s, const char *t)
{
    while (*t && s->n < sizeof s->b) /* bound: the literal's length (at most 32 bytes) */
        s->b[s->n++] = *t++;
}

static void s_u64(sbuf *s, uint64_t v)
{
    char d[20];
    int k = 0;
    do { /* bound: 20 digits */
        d[k++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v && k < 20);
    while (k && s->n < sizeof s->b) /* bound: k <= 20 */
        s->b[s->n++] = d[--k];
}

static void s_kv(sbuf *s, const char *key, uint64_t v)
{
    s_str(s, " ");
    s_str(s, key);
    s_str(s, "=");
    s_u64(s, v);
}

static void s_ident(sbuf *s, const rsxcap_ident *id)
{
    s_str(s, "ps3dk.rsxcap.v1");
    s_kv(s, "cap", id->cap);
    s_kv(s, "conn", id->conn);
    s_kv(s, "wire", id->wire);
    s_kv(s, "dom", id->dom);
    s_kv(s, "owner", id->owner);
    s_kv(s, "frame", id->frame);
}

static void s_counts(sbuf *s, const rsxcap_blk *c)
{
    s_kv(s, "ev", c->ev);
    s_kv(s, "lost", c->lost);
    s_kv(s, "sup", c->sup);
    s_kv(s, "ref", c->ref);
}

/* the payload and its NUL, or nothing at all */
static int s_out(const sbuf *s, char *buf, size_t capacity)
{
    if (!buf || s->n + 1u > capacity)
        return RSXCAP_FMT_ECAP;
    memcpy(buf, s->b, s->n);
    buf[s->n] = '\0';
    return (int)s->n;
}

int rsxcap_emit_begin(char *buf, size_t capacity, const rsxcap_ident *id)
{
    sbuf s;
    s.n = 0;
    s_ident(&s, id);
    return s_out(&s, buf, capacity);
}

int rsxcap_emit_ckpt(char *buf, size_t capacity, uint32_t cap, uint32_t block, const rsxcap_blk *c)
{
    sbuf s;
    s.n = 0;
    s_str(&s, "ps3dk.rsxcap.v1");
    s_kv(&s, "cap", cap);
    s_kv(&s, "block", block);
    s_counts(&s, c);
    return s_out(&s, buf, capacity);
}

int rsxcap_emit_end(char *buf, size_t capacity, const rsxcap_end *e)
{
    sbuf s;
    s.n = 0;
    s_ident(&s, &e->id);
    s_kv(&s, "block", e->block);
    s_counts(&s, &e->c);
    s_kv(&s, "hooks", e->hooks);
    s_kv(&s, "capped", e->capped);
    s_kv(&s, "other_ctx", e->other_ctx);
    s_kv(&s, "other_ctx_sat", e->other_ctx_sat);
    s_kv(&s, "other_thread", e->other_thread);
    s_kv(&s, "other_thread_sat", e->other_thread_sat);
    s_kv(&s, "immediate", e->immediate);
    s_kv(&s, "immediate_sat", e->immediate_sat);
    s_kv(&s, "ckpt_missing", e->ckpt_missing);
    s_kv(&s, "unsafe", e->unsafe_draws);
    s_kv(&s, "no_space", e->no_space);
    s_kv(&s, "reentrant", e->reentrant);
    s_kv(&s, "invalid", e->invalid);
    s_kv(&s, "mismatch", e->mismatch);
    s_kv(&s, "counts_sat", e->counts_sat);
    s_kv(&s, "hook_fault", e->hook_fault);
    s_str(&s, " coverage=librsx-draws-v1");
    return s_out(&s, buf, capacity);
}

/* the formatter refuses every value the reader refuses: the grammar's ranges, then the value rules */
int rsxcap_fmt_begin(char *buf, size_t capacity, const rsxcap_ident *id)
{
    if (id->cap == 0 || id->owner == 0)
        return RSXCAP_FMT_EVALUE;
    return rsxcap_emit_begin(buf, capacity, id);
}

int rsxcap_fmt_ckpt(char *buf, size_t capacity, uint32_t cap, uint32_t block, const rsxcap_blk *c)
{
    if (cap == 0 || block >= RSXCAP_BLOCKS_MAX)
        return RSXCAP_FMT_EVALUE;
    return rsxcap_emit_ckpt(buf, capacity, cap, block, c);
}

int rsxcap_fmt_end(char *buf, size_t capacity, const rsxcap_end *e)
{
    if (e->capped > 1 || e->other_ctx_sat > 1 || e->other_thread_sat > 1 || e->immediate_sat > 1 ||
        e->counts_sat > 1 || e->hook_fault > 1 || e->hooks > RSXCAP_SIDE_MAX || e->unsafe_draws > RSXCAP_SIDE_MAX ||
        e->no_space > RSXCAP_SIDE_MAX || e->reentrant > RSXCAP_SIDE_MAX || e->invalid > RSXCAP_SIDE_MAX ||
        e->mismatch > RSXCAP_SIDE_MAX)
        return RSXCAP_FMT_ERANGE;
    if (e->id.cap == 0 || e->id.owner == 0)
        return RSXCAP_FMT_EVALUE;
    if (e->hooks == 0 ? (e->block != 0 || e->c.ev || e->c.lost || e->c.sup || e->c.ref)
                      : e->block != (e->hooks - 1u) / RSXCAP_BLOCK)
        return RSXCAP_FMT_EVALUE;
    return rsxcap_emit_end(buf, capacity, e);
}

/* ---------------------------------------------------------------- the static pool (no heap) */
static ps3dk_prof_desc rsxcap_pool[RSXCAP_POOL];
static char rsxcap_tags_begin[RSXCAP_CAPS][RSXCAP_BEGIN_BYTES];
static char rsxcap_tags_end[RSXCAP_CAPS][RSXCAP_END_BYTES];
static char rsxcap_tags_ckpt[RSXCAP_CAPS][RSXCAP_CKPT_SLOTS][RSXCAP_CKPT_BYTES];
static ps3dk_prof_desc rsxcap_kinds[RSXCAP_KINDS] = {
    {"draw.arrays", "ps3dk.rsxcap.v1 kind=draw.arrays", RSXCAP_POOL_FILE, RSXCAP_KIND_LINE + 0u, 2u, 0u, 0u},
    {"draw.index", "ps3dk.rsxcap.v1 kind=draw.index", RSXCAP_POOL_FILE, RSXCAP_KIND_LINE + 1u, 2u, 0u, 0u},
    {"draw.inline", "ps3dk.rsxcap.v1 kind=draw.inline", RSXCAP_POOL_FILE, RSXCAP_KIND_LINE + 2u, 2u, 0u, 0u},
    {"clear", "ps3dk.rsxcap.v1 kind=clear", RSXCAP_POOL_FILE, RSXCAP_KIND_LINE + 3u, 2u, 0u, 0u},
    {"clear.zcull", "ps3dk.rsxcap.v1 kind=clear.zcull", RSXCAP_POOL_FILE, RSXCAP_KIND_LINE + 4u, 2u, 0u, 0u},
};

/* once, under spu_mutex, before any registration: names, tag storage, the synthetic file, line = 1 + pool index */
static void pool_init(void)
{
    uint32_t i;
    for (i = 0; i < RSXCAP_POOL; i++) { /* bound: RSXCAP_POOL (1056) */
        uint32_t c = i / RSXCAP_PER_CAP, role = i % RSXCAP_PER_CAP;
        ps3dk_prof_desc *d = &rsxcap_pool[i];
        d->name = role == 0 ? "rsx.capture.begin" : role == 1 ? "rsx.capture.end" : "rsx.capture.ckpt";
        d->logical_tag = role == 0   ? rsxcap_tags_begin[c]
                         : role == 1 ? rsxcap_tags_end[c]
                                     : rsxcap_tags_ckpt[c][role - 2u];
        d->file = RSXCAP_POOL_FILE;
        d->line = 1u + i;
        d->scope = 2u;
        d->state = 0u;
        d->reserved = 0u;
    }
}

const ps3dk_prof_desc *ps3dk_prof__rsxcap_pool(uint32_t *n)
{
    *n = RSXCAP_POOL;
    return rsxcap_pool;
}

const ps3dk_prof_desc *ps3dk_prof__rsxcap_kinds(uint32_t *n)
{
    *n = RSXCAP_KINDS;
    return rsxcap_kinds;
}

#define POOL_BEGIN(cap) (((cap)-1u) * RSXCAP_PER_CAP)
#define POOL_END(cap) (((cap)-1u) * RSXCAP_PER_CAP + 1u)
#define POOL_CKPT(cap, k) (((cap)-1u) * RSXCAP_PER_CAP + 2u + (k))

/* ---------------------------------------------------------------- records and owner work */
static rsxcap_rec g_rec[RSXCAP_CAPS];

/* the owner of the armed cap only (one capture at a time); reset in ARMING */
typedef struct rsxcap_work {
    uint32_t cap, hooks, capped, other_ctx, other_ctx_sat, immediate, immediate_sat, ckpt_missing, counts_sat;
    uint32_t ckpts;
    uint32_t side[5]; /* unsafe, no_space, reentrant, invalid, mismatch */
    rsxcap_blk blk;   /* the current block's receipt counts */
} rsxcap_work;
static rsxcap_work W;

/* agent-private (the agent thread alone): the last result it observed for each cap */
static struct {
    uint32_t valid;
    rsxcap_outcome o;
} g_ag[RSXCAP_CAPS];

static prof *live_prof(void) { return p_ready() ? g_prof : NULL; }

/* a terminal PAL fault (the extension fault or a lock failure): hooks submit nothing and make no transition */
static int hook_stopped(const prof *p) { return LD_ACQ(&p->ext_fault) || LD_RLX(&p->lock_failed); }

static void note_ticket(gpu_slot *g)
{
    if (g->cur_ref)
        g->own[g->cur_ref - 1].last_ticket = g->dom.p_ticket;
}

static void sat16(uint32_t *v, uint32_t *sat)
{
    if (*v < RSXCAP_SAT16)
        (*v)++;
    else
        *sat = 1;
}

static void side_inc(uint32_t which)
{
    if (W.side[which] < RSXCAP_SIDE_MAX)
        W.side[which]++;
    else
        W.counts_sat = 1; /* the first counter to saturate latches it */
}

static void count_receipt(const prg_receipt *r)
{
    switch (r->kind) {
    case PRG_R_EVENT: W.blk.ev++; break;
    case PRG_R_LOST: W.blk.lost++; break;
    case PRG_R_SUPPRESSED: W.blk.sup++; break;
    default: W.blk.ref++; break;
    }
}

/* the other-thread counter (r8 3, carry 1): whole-word CAS, at most RSXCAP_OT_ATTEMPTS attempts, then SAT */
__attribute__((noinline)) static void rsxcap_other_add(rsxcap_rec *rec)
{
    uint32_t w, nw, n;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.rsxcap_neg_plain_other) {
        rec->other++; /* the NEGATIVE control: a plain increment racing the owner's freeze */
        return;
    }
#endif
    w = __atomic_load_n(&rec->other, __ATOMIC_ACQUIRE);
    for (n = 0; n < RSXCAP_OT_ATTEMPTS; n++) { /* bound: RSXCAP_OT_ATTEMPTS (4) */
        if (w & RSXCAP_OT_FROZEN)
            return;
        nw = (w & 0xffffu) == 0xffffu ? (w | RSXCAP_OT_SAT) : w + 1u;
        if (nw == w)
            return; /* saturated already */
        RSTEP(HK_OTHER_BEFORE_CAS);
#ifdef PS3DK_PROF_TEST_HOOKS
        if (ps3dk_prof_test_hooks.rsxcap_other_cas_fail) {
            ps3dk_prof_test_hooks.rsxcap_other_cas_fail--;
            (void)__atomic_fetch_add(&rec->other, 1u, __ATOMIC_SEQ_CST); /* a real concurrent increment */
        }
#endif
        if (__atomic_compare_exchange_n(&rec->other, &w, nw, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
    if (!(w & RSXCAP_OT_FROZEN))
        (void)__atomic_fetch_or(&rec->other, RSXCAP_OT_SAT, __ATOMIC_ACQ_REL); /* contention: count below truth */
}

/* the owner, holding CLOSING (or ARMED on a direct close): the other-thread snapshot and the owner counters */
static void freeze(rsxcap_rec *rec, uint32_t cap)
{
    uint32_t ot = __atomic_fetch_or(&rec->other, RSXCAP_OT_FROZEN, __ATOMIC_ACQ_REL);
    rsxcap_end *e = &rec->end;
    e->id = (rsxcap_ident){cap, rec->conn, rec->wire, rec->dom, rec->owner, 0u, rec->frame};
    e->hooks = W.hooks;
    e->block = W.hooks ? (W.hooks - 1u) / RSXCAP_BLOCK : 0u;
    e->c = W.blk;
    e->capped = W.capped;
    e->other_ctx = W.other_ctx;
    e->other_ctx_sat = W.other_ctx_sat;
    e->other_thread = ot & 0xffffu;
    e->other_thread_sat = (ot & RSXCAP_OT_SAT) ? 1u : 0u;
    e->immediate = W.immediate;
    e->immediate_sat = W.immediate_sat;
    e->ckpt_missing = W.ckpt_missing;
    e->unsafe_draws = W.side[0];
    e->no_space = W.side[1];
    e->reentrant = W.side[2];
    e->invalid = W.side[3];
    e->mismatch = W.side[4];
    e->counts_sat = W.counts_sat;
    e->hook_fault = 0u; /* a runtime END never carries a hook fault (R49): a hook fault writes no END */
    rec->ckpts = W.ckpts;
    rec->frozen = 1u;
}

/* the owner's publication out of a state it alone holds (ARMING, ARMED, CLOSING) to TERMINAL: flags preserved,
 * retried only on a flag change (bound 3: 1 + CANCEL + STOP); returns 1 when published */
static int owner_terminal(uint32_t from, uint32_t cap, uint32_t out, uint32_t rsn)
{
    uint64_t w = S_LD(SLOT);
    uint32_t n;
    for (n = 0; n < 3; n++) { /* bound: 3 (R34) */
        if (RSXCAP_STATE(w) != from || RSXCAP_EPOCH(w) != cap)
            return 0; /* another actor owns it: stop, no write */
        RSTEP(RT_TERMINAL_CAS);
        if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_TERMINAL, w & RSXCAP_FLAGS, cap, out, rsn)))
            return 1;
    }
    return 0;
}

/* ARMED(cap) -> CLOSING(cap), flags preserved (bound 3); 1 on success with *w the CLOSING word */
static int owner_closing(uint32_t cap, uint64_t *wout)
{
    uint64_t w = S_LD(SLOT);
    uint32_t n;
    for (n = 0; n < 3; n++) { /* bound: 3 (R34) */
        uint64_t nw;
        if (RSXCAP_STATE(w) != RSXCAP_ARMED || RSXCAP_EPOCH(w) != cap)
            return 0;
        nw = RSXCAP_WORD(RSXCAP_CLOSING, w & RSXCAP_FLAGS, cap, 0, 0);
        RSTEP(RT_CLOSING_CAS);
        if (S_CAS(SLOT, &w, nw)) {
            *wout = nw;
            return 1;
        }
    }
    return 0;
}

/* the owner's settlement of its ARMED capture with no END: CLOSING, freeze, TERMINAL(ABANDONED or as given) */
static void owner_settle(uint32_t cap, uint32_t out, uint32_t rsn)
{
    uint64_t w;
    if (!owner_closing(cap, &w))
        return;
    freeze(&g_rec[cap - 1], cap);
    (void)owner_terminal(RSXCAP_CLOSING, cap, out, rsn);
}

static uint32_t fault_reason_of(const gpu_slot *g)
{
    return __atomic_load_n(&g->dom.fault_reason, __ATOMIC_ACQUIRE) == PRG_FAULT_HOOK ? RSXCAP_R_HOOK_FAULT
                                                                                       : RSXCAP_R_FAULT;
}

/* ---------------------------------------------------------------- gpu_frame (the owner, after PREPARE) */
/* a pool descriptor through the owner's own registration path; 0 with *idx, -1 refused, -2 a lock fault */
static int pool_register(prof *p, uint32_t i, uint32_t *idx)
{
    uint32_t scope = 0;
    int rc = p_gpu_desc(p, &rsxcap_pool[i], idx, &scope);
    if (rc == PS3DK_PROF_OK && scope == 2u)
        return 0;
    return LD_RLX(&p->lock_failed) ? -2 : -1;
}

/* REQUESTED(e) for this domain, a frame token: claim, BEGIN, ARMED */
static int arm(prof *p, gpu_slot *g, uint64_t w, uint32_t owner, uint64_t frame, uint8_t ref, uint32_t *committed)
{
    uint32_t e = RSXCAP_EPOCH(w), n, idx = 0;
    rsxcap_rec *rec = &g_rec[e - 1];
    prg_fc_out out;
    int rr;
    for (n = 0; n < 3; n++) { /* bound: 3 (R34): only flags may change under a REQUESTED word we still match */
        if (RSXCAP_STATE(w) != RSXCAP_REQUESTED || RSXCAP_EPOCH(w) != e)
            return RSXCAP_FRAME_PLAIN; /* lost to cancel / withdrawal / detach: no request for this call (R19) */
        if (w & RSXCAP_F_STOP) {
            /* STOP already set: settle in this CAS (R18), no BEGIN; the losing actor has written nothing */
            RSTEP(RT_CLAIM_CAS);
            if (S_CAS(SLOT, &w,
                      RSXCAP_WORD(RSXCAP_TERMINAL, w & RSXCAP_FLAGS, e, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING)))
                return RSXCAP_FRAME_PLAIN;
            continue;
        }
        RSTEP(RT_CLAIM_CAS);
        if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_ARMING, w & RSXCAP_FLAGS, e, 0, 0)))
            break;
    }
    if (n == 3)
        return RSXCAP_FRAME_PLAIN;
    RSTEP(RT_AFTER_CLAIM);
    memset(&W, 0, sizeof W);
    W.cap = e;
    rec->bound = 1u;
    rec->owner = owner;
    rec->frame = frame;
    rec->ctx = g->ctx;
    rec->thread = g->owner;
    {
        rsxcap_ident id = {e, rec->conn, rec->wire, rec->dom, owner, 0u, frame};
        if (rsxcap_fmt_begin(rsxcap_tags_begin[e - 1], RSXCAP_BEGIN_BYTES, &id) < 0) {
            (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_REFUSED, RSXCAP_R_DESCRIPTOR);
            return RSXCAP_FRAME_PLAIN;
        }
    }
    rr = pool_register(p, POOL_BEGIN(e), &idx);
    if (rr == -2) {
        (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING);
        return PRG_ESTOPPED;
    }
    if (rr != 0) {
        (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_REFUSED, RSXCAP_R_DESCRIPTOR);
        return RSXCAP_FRAME_PLAIN; /* no admission for the capture; the boundary runs today's prg_frame */
    }
    RSTEP(RT_AFTER_DESC);
    prg_frame_capture(&g->dom, owner, frame, ref, 0u, idx + 1u, &out);
    rec->begin_r = out.begin; /* written while ARMING (carry 2) */
    *committed = out.committed;
    if (out.rc == PRG_ESTOPPED || out.rc == PRG_EINVAL) {
        (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_REFUSED, RSXCAP_R_DOMAIN_STOPPED);
        return out.rc;
    }
    if (out.rc == PRG_EFAULT) {
        (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_REFUSED, RSXCAP_R_BEGIN_PUBLISH_FAULT);
        return PRG_EFAULT;
    }
    if (out.begin.kind != PRG_R_EVENT) {
        (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_REFUSED, RSXCAP_R_BEGIN_NOT_ENQUEUED);
        return PRG_OK;
    }
    RSTEP(RT_BEFORE_ARMED_PUB);
    w = S_LD(SLOT);
    for (n = 0; n < 3; n++) { /* bound: 3 (R34) */
        if (RSXCAP_STATE(w) != RSXCAP_ARMING || RSXCAP_EPOCH(w) != e)
            return PRG_OK;
        if (w & RSXCAP_F_STOP) { /* STOP at the publication: no ARMED (R21) */
            (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING);
            return PRG_OK;
        }
        if (LD_ACQ(&p->ext_fault)) { /* R93: an extension fault latched since the claim */
            (void)owner_terminal(RSXCAP_ARMING, e, RSXCAP_O_ABANDONED, RSXCAP_R_FAULT);
            return PRG_OK;
        }
        RSTEP(RT_ARMED_CAS);
        if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_ARMED, w & RSXCAP_FLAGS, e, 0, 0)))
            return PRG_OK;
    }
    return PRG_OK;
}

/* ARMED(e) for this domain at a boundary: CLOSING, END, TERMINAL */
static int close_(prof *p, gpu_slot *g, uint32_t e, uint32_t owner, uint64_t frame, uint8_t ref, uint32_t *committed)
{
    rsxcap_rec *rec = &g_rec[e - 1];
    uint64_t w;
    uint32_t idx = 0, outc, rsn;
    prg_fc_out out;
    int rr;
    uint32_t n, closed = 0;
    RSTEP(RT_BEFORE_CLOSING);
    w = S_LD(SLOT);
    for (n = 0; n < 3; n++) { /* bound: 3 (R34) */
        if (RSXCAP_STATE(w) != RSXCAP_ARMED || RSXCAP_EPOCH(w) != e)
            return RSXCAP_FRAME_PLAIN;
        if (w & RSXCAP_F_STOP) { /* R83: STOP in the word this attempt loaded: no normal close, no END */
            owner_settle(e, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING);
            return RSXCAP_FRAME_PLAIN;
        }
        RSTEP(RT_CLOSING_CAS);
        if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_CLOSING, w & RSXCAP_FLAGS, e, 0, 0))) {
            closed = 1; /* the commit point: STOP after it is preserved and does not cancel the close */
            break;
        }
    }
    if (!closed)
        return RSXCAP_FRAME_PLAIN;
    freeze(rec, e);
    RSTEP(RT_AFTER_FREEZE);
    if (rsxcap_fmt_end(rsxcap_tags_end[e - 1], RSXCAP_END_BYTES, &rec->end) < 0) {
        (void)owner_terminal(RSXCAP_CLOSING, e, RSXCAP_O_ABANDONED, RSXCAP_R_END_DESCRIPTOR);
        return RSXCAP_FRAME_PLAIN;
    }
    rr = pool_register(p, POOL_END(e), &idx);
    if (rr == -2) { /* R25: a lock fault is terminal */
        (void)owner_terminal(RSXCAP_CLOSING, e, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING);
        return PRG_ESTOPPED;
    }
    if (rr != 0) { /* R24: no END marker; the boundary still commits */
        (void)owner_terminal(RSXCAP_CLOSING, e, RSXCAP_O_ABANDONED, RSXCAP_R_END_DESCRIPTOR);
        return RSXCAP_FRAME_PLAIN;
    }
    RSTEP(RT_AFTER_END_DESC);
    prg_frame_capture(&g->dom, owner, frame, ref, idx + 1u, 0u, &out);
    rec->end_r = out.end;
    *committed = out.committed;
    if (out.rc == PRG_ESTOPPED || out.rc == PRG_EINVAL) {
        outc = RSXCAP_O_ABANDONED;
        rsn = RSXCAP_R_DOMAIN_STOPPED;
    } else if (out.rc == PRG_EFAULT) {
        outc = RSXCAP_O_ABANDONED;
        rsn = RSXCAP_R_FAULT;
    } else if (out.end.kind != PRG_R_EVENT) {
        outc = RSXCAP_O_ABANDONED;
        rsn = RSXCAP_R_END_NOT_ENQUEUED;
    } else {
        outc = RSXCAP_O_DONE;
        rsn = RSXCAP_R_NONE;
    }
    RSTEP(RT_BEFORE_TERMINAL);
    (void)owner_terminal(RSXCAP_CLOSING, e, outc, rsn);
    return out.rc;
}

static int rc_frame(prof *p, gpu_slot *g, uint32_t owner, uint64_t frame, uint8_t ref, uint32_t *committed)
{
    uint64_t w;
    uint32_t e;
    *committed = 0;
    RSTEP(RT_AFTER_PREPARE);
    w = __atomic_load_n(SLOT, __ATOMIC_ACQUIRE);
    e = RSXCAP_EPOCH(w);
    if (e == 0 || e > RSXCAP_CAPS)
        return RSXCAP_FRAME_PLAIN;
    if (RSXCAP_STATE(w) == RSXCAP_REQUESTED) {
        RSTEP(RT_AFTER_SLOT_LOAD);
        /* rec[e - 1] was published by the agent's REQUESTED CAS (acquired above) and is immutable since */
        if (g_rec[e - 1].dom != g->dom.domain_id || owner == 0)
            return RSXCAP_FRAME_PLAIN; /* another domain, or NONE: REQUESTED unchanged (R30) */
        return arm(p, g, w, owner, frame, ref, committed);
    }
    if (RSXCAP_STATE(w) == RSXCAP_ARMED && g_rec[e - 1].dom == g->dom.domain_id)
        return close_(p, g, e, owner, frame, ref, committed);
    return RSXCAP_FRAME_PLAIN;
}

/* ---------------------------------------------------------------- detach and shutdown */
static void rc_detach_settle(prof *p, gpu_slot *g)
{
    uint64_t w;
    uint32_t n, e;
    (void)p;
    RSTEP(DT_BEFORE_SETTLE);
    w = S_LD(SLOT); /* SC, after the DETACHING CAS (R78) */
    RSTEP(DT_AFTER_LOAD);
    for (n = 0; n < 3; n++) { /* bound: 3 (R34) */
        e = RSXCAP_EPOCH(w);
        if (e == 0 || e > RSXCAP_CAPS)
            return;
        if (RSXCAP_STATE(w) == RSXCAP_REQUESTED) {
            if (g_rec[e - 1].dom != g->dom.domain_id)
                return;
            RSTEP(DT_SETTLE_CAS);
            if (S_CAS(SLOT, &w,
                      RSXCAP_WORD(RSXCAP_TERMINAL, w & RSXCAP_FLAGS, e, RSXCAP_O_ABANDONED, RSXCAP_R_DETACH)))
                return;
            continue; /* re-examine: a flag change retries, a state change stops */
        }
        if (RSXCAP_STATE(w) == RSXCAP_ARMED && g_rec[e - 1].dom == g->dom.domain_id) {
            /* no END; with STOP the owner's detach is its stopping close (R38) */
            owner_settle(e, RSXCAP_O_ABANDONED, (w & RSXCAP_F_STOP) ? RSXCAP_R_STOPPING : RSXCAP_R_DETACH);
            return;
        }
        return; /* IDLE, CLAIMING (unreadable), another domain, or settled */
    }
}

static void rc_stop(prof *p)
{
    (void)p;
    (void)__atomic_fetch_or(SLOT, RSXCAP_F_STOP, __ATOMIC_SEQ_CST); /* any state, IDLE included (R40) */
}

/* attach, under spu_mutex on the owner, before the id is published: the 5 kinds, or not capable */
static void rc_attach(prof *p, gpu_slot *g)
{
    uint32_t k, idx, scope;
    g->rsxcap_capable = 0;
    /* only an already registered thread: auto-registration would take thread_mutex under spu_mutex */
    if (!g_tls.slot1 || g_tls.acked || g_tls.retired)
        return;
    for (k = 0; k < RSXCAP_KINDS; k++) { /* bound: 5 */
        scope = 0;
        if (p_gpu_desc(p, &rsxcap_kinds[k], &idx, &scope) != PS3DK_PROF_OK || scope != 2u)
            return;
        g->rsxcap_kind[k] = idx;
    }
    g->rsxcap_capable = 1;
}

static const rsxcap_ops g_ops = {rc_stop, rc_frame, rc_detach_settle, rc_attach};

/* ---------------------------------------------------------------- the opt-in */
static int agent_lock(prof *p)
{
    if (LD_ACQ(&p->ext_fault))
        return -1;
    if (p_ext_lock_at(p) != 0) {
        ST_REL(&p->ext_fault, 1u);
        ST_RLX(&p->lock_failed, 1);
        return -1;
    }
    if (LD_ACQ(&p->ext_fault)) {
        (void)p_ext_unlock_site(p, U_ANY);
        return -1;
    }
    return 0;
}

int ps3dk_prof__rsxcap_enable(void)
{
    prof *p;
    int rc;
    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY;
    p = live_prof();
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->gpu_max)
        return PS3DK_PROF_ENOTSUP;
    if (LD_ACQ(&p->ext_fault) || LD_ACQ(&p->shutdown_once))
        return PS3DK_PROF_ESTOPPED;
    if (agent_lock(p) != 0)
        return PS3DK_PROF_ESTOPPED;
    if (LD_ACQ(&p->rsxcap)) {
        rc = PS3DK_PROF_OK; /* idempotent */
    } else if (p->gpu_lifetime) {
        rc = PS3DK_PROF_EBUSY; /* late: an attach already happened in this process */
    } else {
        pool_init();
        ST_REL(&p->rsxcap, &g_ops);
        rc = PS3DK_PROF_OK;
    }
    if (p_ext_unlock_site(p, U_RSXCAP_ENABLE) != 0)
        return PS3DK_PROF_ESTOPPED;
    return rc;
}

/* ---------------------------------------------------------------- the agent side */
static void unpin(gpu_slot *g) { (void)__atomic_fetch_sub(&g->users, 1u, __ATOMIC_SEQ_CST); }

static int pin_slot(gpu_slot *g)
{
    uint32_t u = S_LD(&g->users);
    uint32_t n;
    for (n = 0; n < 3; n++) { /* bound: 3 attempts (under spu_mutex only other pins and unpins race it) */
        if (u >= GPU_USERS_MAX)
            return -1;
#ifdef PS3DK_PROF_TEST_HOOKS
        if (ps3dk_prof_test_hooks.rsxcap_pin_cas_fail) { /* a real concurrent pin lands between load and CAS */
            ps3dk_prof_test_hooks.rsxcap_pin_cas_fail--;
            (void)__atomic_fetch_add(&g->users, 1u, __ATOMIC_SEQ_CST);
        }
#endif
        if (S_CAS(&g->users, &u, u + 1u))
            return 0;
    }
    return -1;
}

static int eligible(const gpu_slot *g)
{
    return S_LD(&g->id) != 0u && S_LD(&g->dom.state) == PRG_ATTACHED && g->rsxcap_capable &&
           !__atomic_load_n(&g->dom.fault, __ATOMIC_ACQUIRE);
}

static int refuse(uint32_t *reason, uint32_t why)
{
    *reason = why;
    return RSXCAP_REFUSED;
}

static void ag_cache(uint32_t cap, const rsxcap_outcome *o)
{
    g_ag[cap - 1].valid = 1;
    g_ag[cap - 1].o = *o;
    g_ag[cap - 1].o.cached = 1;
}

static void ag_note(uint32_t cap, uint64_t w, uint32_t outc, uint32_t rsn)
{
    rsxcap_outcome o;
    const rsxcap_rec *rec = &g_rec[cap - 1];
    memset(&o, 0, sizeof o);
    o.word = w;
    o.cap = cap;
    o.state = RSXCAP_STATE(w);
    o.flags = (uint32_t)((w & RSXCAP_FLAGS) >> 8);
    o.outcome = outc;
    o.reason = rsn;
    o.conn = rec->conn; /* the agent's own fields */
    o.wire = rec->wire;
    o.dom = rec->dom;
    ag_cache(cap, &o);
}

int ps3dk_prof__rsxcap_request(uint32_t conn, uint32_t wire, uint32_t domain, uint32_t *cap, uint32_t *reason)
{
    prof *p;
    gpu_slot *sel = NULL;
    rsxcap_rec *rec;
    uint64_t w, nw;
    uint32_t e, i, n, found = 0, tslot = 0, why = RSXCAP_R_NONE, unlock_failed, withdrawn = 0, wrsn = 0;
    if (!cap || !reason)
        return PS3DK_PROF_EINVAL;
    *cap = 0;
    *reason = RSXCAP_R_NONE;
    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY; /* R90: the TLS guard alone, before any lock or pin */
    p = live_prof();
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->gpu_max || !LD_ACQ(&p->rsxcap))
        return refuse(reason, RSXCAP_R_NO_CAPABILITY);
    if (S_LD(SLOT) & RSXCAP_F_STOP)
        return refuse(reason, RSXCAP_R_STOPPING);
    if (agent_lock(p) != 0)
        return refuse(reason, RSXCAP_R_STOPPING); /* ext_fault latched by the lock failure; slot unchanged */
    w = S_LD(SLOT);
    e = RSXCAP_EPOCH(w);
    if (w & RSXCAP_F_STOP)
        why = RSXCAP_R_STOPPING;
    else if (RSXCAP_STATE(w) != RSXCAP_IDLE)
        why = RSXCAP_R_BUSY;
    else if (e >= RSXCAP_CAPS)
        why = RSXCAP_R_CAPACITY;
    else {
        /* R90: scan, resolve and check under the one hold; the selected slot stays pinned through the claim, the
         * unlock and the revalidation */
        int incomplete = 0;
        for (i = 0; i < p->gpu_max; i++) { /* bound: gpu_max */
            gpu_slot *g = &p->gpu[i];
            if (pin_slot(g) != 0) {
                incomplete = 1; /* a slot that cannot be pinned cannot be inspected: no resolution can be trusted */
                break;
            }
            if (domain ? S_LD(&g->id) == domain : eligible(g)) {
                found++;
                if (!sel) {
                    sel = g;
                    tslot = i;
                    continue; /* keep this pin */
                }
            }
            unpin(g);
        }
        if (incomplete) {
            why = RSXCAP_R_BUSY; /* refused before any claim: no cap consumed; the selected pin is released below */
        } else if (!sel) {
            why = RSXCAP_R_BAD_DOMAIN;
        } else if (!domain && found != 1) {
            why = RSXCAP_R_BAD_DOMAIN;
        } else if (!sel->rsxcap_capable) {
            why = RSXCAP_R_NO_CAPABILITY;
        } else if (__atomic_load_n(&sel->dom.fault, __ATOMIC_ACQUIRE)) {
            why = RSXCAP_R_DOMAIN_FAULTED;
        } else if (S_LD(&sel->dom.state) != PRG_ATTACHED) {
            why = RSXCAP_R_BAD_DOMAIN;
        }
        if (why != RSXCAP_R_NONE && sel) {
            unpin(sel);
            sel = NULL;
        }
    }
    if (why != RSXCAP_R_NONE) {
        (void)p_ext_unlock_site(p, U_RSXCAP_REQUEST); /* a failed unlock latches ext_fault; nothing was claimed */
        return refuse(reason, why);
    }
    RSTEP(AG_AFTER_SCAN);
    RSTEP(AG_BEFORE_CLAIM_CAS);
    {
        int claimed = 0;
        for (n = 0; n < 2; n++) { /* bound: 2 (R77): in IDLE only the stopper can change the word */
            if ((w & RSXCAP_F_STOP) || RSXCAP_STATE(w) != RSXCAP_IDLE || RSXCAP_EPOCH(w) != e)
                break;
            /* the old cap's CANCEL is dropped; STOP is required clear in the loaded word (R27) */
            RSTEP(AG_CLAIM_CAS);
            if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_CLAIMING, w & RSXCAP_F_STOP, e + 1u, 0, 0))) {
                claimed = 1;
                break;
            }
        }
        if (!claimed) {
            /* the CAS did not land (the stopper came first): refused, cap 0, no record written */
            unpin(sel);
            (void)p_ext_unlock_site(p, U_RSXCAP_REQUEST);
            return refuse(reason, (w & RSXCAP_F_STOP) ? RSXCAP_R_STOPPING : RSXCAP_R_BUSY);
        }
    }
    RSTEP(AG_AFTER_CLAIM_CAS);
    /* claimed cap e + 1: the agent's fields and zeroed telemetry, then the publication */
    e = e + 1u;
    rec = &g_rec[e - 1];
#ifdef PS3DK_PROF_TEST_HOOKS
    if (!ps3dk_prof_test_hooks.rsxcap_neg_late_fields) {
#endif
        memset(rec, 0, RSXCAP_REC_IMMUTABLE_BYTES);
        __atomic_store_n(&rec->other, 0u, __ATOMIC_RELAXED);
        rec->conn = conn;
        rec->wire = wire;
        rec->dom = S_LD(&sel->id);
        rec->tslot = tslot;
#ifdef PS3DK_PROF_TEST_HOOKS
    }
#endif
    RSTEP(AG_CLAIMING);
    w = S_LD(SLOT);
    RSTEP(AG_PUB_LOAD);
    nw = 0;
    for (n = 0; n < 2; n++) { /* bound: 2 (R34): only STOP can change CLAIMING */
        if (RSXCAP_STATE(w) != RSXCAP_CLAIMING || RSXCAP_EPOCH(w) != e)
            break;
        nw = (w & RSXCAP_F_STOP)
                 ? RSXCAP_WORD(RSXCAP_TERMINAL, w & RSXCAP_FLAGS, e, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING)
                 : RSXCAP_WORD(RSXCAP_REQUESTED, w & RSXCAP_FLAGS, e, 0, 0);
        RSTEP(AG_PUB_CAS);
        if (S_CAS(SLOT, &w, nw))
            break;
        nw = 0;
    }
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.rsxcap_neg_late_fields) { /* the NEGATIVE control: fields after the publication */
        memset(rec, 0, RSXCAP_REC_IMMUTABLE_BYTES);
        rec->conn = conn;
        rec->wire = wire;
        rec->dom = S_LD(&sel->id);
        rec->tslot = tslot;
    }
#endif
    RSTEP(AG_AFTER_PUBLICATION);
    unlock_failed = p_ext_unlock_site(p, U_RSXCAP_REQUEST) != 0;
    *cap = e;
    if (RSXCAP_STATE(nw) == RSXCAP_TERMINAL && nw) {
        /* R77: claimed before STOP, published as TERMINAL(REFUSED, stopping) */
        unpin(sel);
        ag_note(e, nw, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING);
        if (unlock_failed)
            return PS3DK_PROF_ESTOPPED;
        return refuse(reason, RSXCAP_R_STOPPING);
    }
    /* R78 / R89 / R91: the post-publication revalidation, still pinned (SC loads after the SC publication) */
    RSTEP(AG_BEFORE_CHECK);
    {
        int idok = S_LD(&sel->id) == rec->dom, stok = S_LD(&sel->dom.state) == PRG_ATTACHED;
        int flt = __atomic_load_n(&sel->dom.fault, __ATOMIC_ACQUIRE) != 0;
        int xf = unlock_failed || LD_ACQ(&p->ext_fault);
        w = S_LD(SLOT);
        RSTEP(AG_WITHDRAW_LOAD);
        for (n = 0; n < 3; n++) { /* bound: 3 (carry 1): 1 + a STOP change + a CANCEL change */
            if (RSXCAP_STATE(w) != RSXCAP_REQUESTED || RSXCAP_EPOCH(w) != e)
                break; /* the owner or detach moved it: no write, the outcome comes from query */
            wrsn = xf                       ? RSXCAP_R_FAULT
                   : (w & RSXCAP_F_STOP)    ? RSXCAP_R_STOPPING
                   : flt                    ? RSXCAP_R_DOMAIN_FAULTED
                   : (!idok || !stok)       ? RSXCAP_R_DOMAIN_DETACHED
                                            : RSXCAP_R_NONE;
            if (wrsn == RSXCAP_R_NONE)
                break; /* the check passed: ACCEPTED */
            RSTEP(AG_WITHDRAW_CAS);
            if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_IDLE, w & RSXCAP_FLAGS, e, 0, 0))) {
                withdrawn = 1;
                break;
            }
        }
    }
    unpin(sel); /* the last access to the table slot */
    if (withdrawn)
        ag_note(e, RSXCAP_WORD(RSXCAP_IDLE, w & RSXCAP_FLAGS, e, 0, 0), RSXCAP_O_WITHDRAWN, wrsn);
    if (unlock_failed)
        return PS3DK_PROF_ESTOPPED; /* R91: never cap 0, never ACCEPTED */
    if (withdrawn)
        return refuse(reason, wrsn);
    return RSXCAP_ACCEPTED;
}

int ps3dk_prof__rsxcap_cancel(uint32_t cap)
{
    uint64_t w;
    uint32_t n;
    if (cap == 0 || cap > RSXCAP_CAPS)
        return PS3DK_PROF_EINVAL;
    w = S_LD(SLOT);
    RSTEP(AG_CANCEL_LOAD);
    /* bound: 6 = 1 + the owner's three forward moves (ARMING, ARMED, CLOSING) + STOP + one spare */
    for (n = 0; n < 6; n++) {
        if (RSXCAP_EPOCH(w) != cap)
            return PS3DK_PROF_EINVAL; /* stale */
        switch (RSXCAP_STATE(w)) {
        case RSXCAP_REQUESTED:
            RSTEP(AG_CANCEL_CAS);
            if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_IDLE, w & RSXCAP_FLAGS, cap, 0, 0))) {
                ag_note(cap, RSXCAP_WORD(RSXCAP_IDLE, w & RSXCAP_FLAGS, cap, 0, 0), RSXCAP_O_CANCELLED,
                        RSXCAP_R_NONE);
                return PS3DK_PROF_OK;
            }
            continue;
        case RSXCAP_ARMING:
        case RSXCAP_ARMED:
        case RSXCAP_CLOSING:
            if (w & RSXCAP_F_CANCEL)
                return PS3DK_PROF_OK;
            RSTEP(AG_CANCEL_CAS);
            if (S_CAS(SLOT, &w, w | RSXCAP_F_CANCEL))
                return PS3DK_PROF_OK;
            continue;
        default:
            return PS3DK_PROF_EINVAL; /* IDLE, CLAIMING or TERMINAL: nothing to cancel */
        }
    }
    return PS3DK_PROF_EBUSY; /* fail closed past the bound */
}

int ps3dk_prof__rsxcap_query(uint32_t cap, rsxcap_outcome *out)
{
    uint64_t w;
    uint32_t n;
    prof *p = live_prof();
    const rsxcap_rec *rec;
    if (!out || cap == 0 || cap > RSXCAP_CAPS)
        return PS3DK_PROF_EINVAL;
    w = __atomic_load_n(SLOT, __ATOMIC_ACQUIRE);
    for (n = 0; n < 3; n++) { /* bound: 3: the R89 service retries only on a flag change */
        if (RSXCAP_EPOCH(w) != cap || RSXCAP_STATE(w) == RSXCAP_IDLE) {
            /* a stale cap (carry 3): only the agent-private result, never a later cap's word */
            if (!g_ag[cap - 1].valid)
                return PS3DK_PROF_EINVAL;
            *out = g_ag[cap - 1].o;
            out->word = w;
            return PS3DK_PROF_OK;
        }
        if (RSXCAP_STATE(w) == RSXCAP_REQUESTED && (w & RSXCAP_F_STOP)) {
            /* R89: the agent services REQUESTED + STOP; no record write */
            RSTEP(AG_SERVICE_CAS);
            if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_IDLE, w & RSXCAP_FLAGS, cap, 0, 0))) {
                ag_note(cap, RSXCAP_WORD(RSXCAP_IDLE, w & RSXCAP_FLAGS, cap, 0, 0), RSXCAP_O_WITHDRAWN,
                        RSXCAP_R_STOPPING);
                *out = g_ag[cap - 1].o;
                return PS3DK_PROF_OK;
            }
            continue;
        }
        break;
    }
    rec = &g_rec[cap - 1];
    memset(out, 0, sizeof *out);
    out->word = w;
    out->cap = cap;
    out->state = RSXCAP_STATE(w);
    out->flags = (uint32_t)((w & RSXCAP_FLAGS) >> 8);
    out->conn = rec->conn;
    out->wire = rec->wire;
    out->dom = rec->dom;
    if (out->state == RSXCAP_TERMINAL) {
        out->outcome = RSXCAP_OUTCOME(w);
        out->reason = RSXCAP_REASON(w);
        out->begin_r = rec->begin_r; /* after the acquire load of TERMINAL(cap): final */
        out->end_r = rec->end_r;
        out->end = rec->end;
        ag_cache(cap, out);
        out->cached = 0;
    } else if (out->state == RSXCAP_ARMED && (w & RSXCAP_F_STOP)) {
        out->outcome = RSXCAP_O_ARMED_STOPPED;
        out->reason = RSXCAP_R_STOPPING;
    } else if (out->state == RSXCAP_ARMED && p && LD_ACQ(&p->ext_fault)) {
        out->outcome = RSXCAP_O_ARMED_FAULTED;
        out->reason = RSXCAP_R_FAULT;
    }
    return PS3DK_PROF_OK;
}

int ps3dk_prof__rsxcap_recycle(uint32_t cap)
{
    uint64_t w;
    uint32_t n;
    rsxcap_outcome o;
    if (cap == 0 || cap > RSXCAP_CAPS)
        return PS3DK_PROF_EINVAL;
    w = __atomic_load_n(SLOT, __ATOMIC_ACQUIRE);
    if (RSXCAP_STATE(w) != RSXCAP_TERMINAL || RSXCAP_EPOCH(w) != cap)
        return PS3DK_PROF_EINVAL;
    (void)ps3dk_prof__rsxcap_query(cap, &o); /* caches the final result before the word moves on */
    RSTEP(AG_BEFORE_RECYCLE);
    for (n = 0; n < 2; n++) { /* bound: 2: in TERMINAL only the stopper can change the word */
        if (RSXCAP_STATE(w) != RSXCAP_TERMINAL || RSXCAP_EPOCH(w) != cap)
            return PS3DK_PROF_EINVAL;
        /* carry 3: the outcome and reason bits are cleared, STOP and CANCEL kept */
        RSTEP(AG_RECYCLE_CAS);
        if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_IDLE, w & RSXCAP_FLAGS, cap, 0, 0)))
            return PS3DK_PROF_OK;
    }
    return PS3DK_PROF_EBUSY;
}

/* ---------------------------------------------------------------- the hook side */
/* the owner's checkpoint for block k, immediately before the first draw of block k + 1; 0 when the capture settled */
static int checkpoint(prof *p, gpu_slot *g, uint32_t e, uint32_t k)
{
    uint32_t idx = 0;
    prg_receipt r;
    int rc = rsxcap_fmt_ckpt(rsxcap_tags_ckpt[e - 1][k], RSXCAP_CKPT_BYTES, e, k, &W.blk);
    memset(&W.blk, 0, sizeof W.blk);
    if (rc < 0 || pool_register(p, POOL_CKPT(e, k), &idx) != 0) {
        if (rc >= 0 && LD_RLX(&p->lock_failed)) {
            /* a lock fault in the registration is terminal (as R25): no MARK, no draw; settled, the hook stops */
            owner_settle(e, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING);
            return 0;
        }
        W.ckpt_missing++; /* registration refused: no MARK (OQ-11) */
        return 1;
    }
    rc = prg_mark_r(&g->dom, idx, &r);
    note_ticket(g);
    if (rc == PRG_EFAULT && r.kind == PRG_R_EVENT) {
        owner_settle(e, RSXCAP_O_ABANDONED, fault_reason_of(g)); /* R80: settled now, not a missing checkpoint */
        return 0;
    }
    if (r.kind != PRG_R_EVENT)
        W.ckpt_missing++;
    else
        W.ckpts++;
    if (rc == PRG_EFAULT) {
        owner_settle(e, RSXCAP_O_ABANDONED, fault_reason_of(g));
        return 0;
    }
    return 1;
}

int ps3dk_prof__rsxcap_draw_begin(ps3dk_prof_gcm_ctx *ctx, uint32_t kind, rsxcap_draw *dr)
{
    uint64_t w = __atomic_load_n(SLOT, __ATOMIC_ACQUIRE); /* the not-armed path: this one load */
    uint32_t e;
    rsxcap_rec *rec;
    prof *p;
    gpu_slot *g;
    prg_receipt r;
    int rc;
    dr->open = 0;
    if (RSXCAP_STATE(w) != RSXCAP_ARMED)
        return 0;
    RSTEP(HK_AFTER_SLOT_LOAD);
    e = RSXCAP_EPOCH(w);
    if (e == 0 || e > RSXCAP_CAPS)
        return 0;
    rec = &g_rec[e - 1]; /* read only after the acquire load showed ARMED(e) (R33) */
    if (!pal_owner_is_self(&rec->thread)) {
        if ((const void *)ctx == rec->ctx)
            rsxcap_other_add(rec);
        return 0;
    }
    p = g_prof;
    g = &p->gpu[rec->tslot];
    if (kind >= RSXCAP_KINDS || g_tls.in_gpu || g_tls.in_drain)
        return 0;
    if (hook_stopped(p))
        return 0; /* R93: ARMED stays ARMED (ARMED_FAULTED); no submission, no transition */
    if (w & RSXCAP_F_STOP) {
        owner_settle(e, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING); /* stop instrumenting, no END */
        return 0;
    }
    if ((const void *)ctx != rec->ctx) {
        sat16(&W.other_ctx, &W.other_ctx_sat);
        return 0;
    }
    if (W.capped)
        return 0;
    if (W.hooks >= RSXCAP_DRAW_CAP) {
        W.capped = 1; /* the 262145th eligible draw */
        return 0;
    }
    g_tls.in_gpu = 1;
    if (W.hooks && W.hooks % RSXCAP_BLOCK == 0u && !checkpoint(p, g, e, W.hooks / RSXCAP_BLOCK - 1u)) {
        g_tls.in_gpu = 0;
        return 0;
    }
    W.hooks++;
    rc = prg_begin_r(&g->dom, g->rsxcap_kind[kind], &dr->tok, &r);
    note_ticket(g);
    g_tls.in_gpu = 0;
    count_receipt(&r);
    if (rc == PRG_EFAULT) {
        owner_settle(e, RSXCAP_O_ABANDONED, fault_reason_of(g)); /* R80 */
        return 0;
    }
    if (r.kind == PRG_R_REFUSED)
        return 0; /* the INERT zone: no END (R46) */
    dr->open = 1;
    dr->cap = e;
    dr->kind = kind;
    dr->desc = g->rsxcap_kind[kind];
    return 1;
}

void ps3dk_prof__rsxcap_draw_end(rsxcap_draw *dr)
{
    rsxcap_rec *rec;
    gpu_slot *g;
    prg_receipt r;
    uint64_t w;
    int rc;
    if (!dr->open)
        return;
    dr->open = 0;
    rec = &g_rec[dr->cap - 1];
    g = &g_prof->gpu[rec->tslot];
    w = __atomic_load_n(SLOT, __ATOMIC_ACQUIRE);
    if (RSXCAP_STATE(w) != RSXCAP_ARMED || RSXCAP_EPOCH(w) != dr->cap || (w & RSXCAP_F_STOP) ||
        hook_stopped(g_prof)) {
        /* STOP (or a terminal fault) already observed, or the capture ended: no further instrumentation (R38).
         * The open zone is popped through the no-command path (prg_zone_abandon: nothing submitted or
         * published), so the producer stack stays exact; STOP then settles the capture without an END. */
        g_tls.in_gpu = 1;
        (void)prg_zone_abandon(&g->dom, &dr->tok, dr->desc);
        g_tls.in_gpu = 0;
        if (RSXCAP_STATE(w) == RSXCAP_ARMED && RSXCAP_EPOCH(w) == dr->cap && (w & RSXCAP_F_STOP) &&
            !hook_stopped(g_prof))
            owner_settle(dr->cap, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING);
        return;
    }
    g_tls.in_gpu = 1;
    rc = prg_end_r(&g->dom, &dr->tok, dr->desc, &r);
    note_ticket(g);
    g_tls.in_gpu = 0;
    count_receipt(&r);
    if (rc == PRG_EFAULT)
        owner_settle(dr->cap, RSXCAP_O_ABANDONED, fault_reason_of(g));
}

void ps3dk_prof__rsxcap_draw_abandon(rsxcap_draw *dr)
{
    rsxcap_rec *rec;
    gpu_slot *g;
    uint64_t w;
    if (!dr->open)
        return;
    dr->open = 0;
    rec = &g_rec[dr->cap - 1];
    g = &g_prof->gpu[rec->tslot];
    g_tls.in_gpu = 1;
    (void)prg_zone_abandon(&g->dom, &dr->tok, dr->desc); /* (1) its own admission; nothing submitted */
    g_tls.in_gpu = 0;
    w = __atomic_load_n(SLOT, __ATOMIC_ACQUIRE);
    if (RSXCAP_STATE(w) == RSXCAP_ARMED && RSXCAP_EPOCH(w) == dr->cap)
        side_inc(RSXCAP_C_MISMATCH);
    prg_hook_fault(&g->dom); /* (2) the latch: a refused abandon still takes the fault path */
    if (RSXCAP_STATE(w) == RSXCAP_ARMED && RSXCAP_EPOCH(w) == dr->cap)
        owner_settle(dr->cap, RSXCAP_O_ABANDONED, fault_reason_of(g)); /* (3) settled in this call (R62) */
}

void ps3dk_prof__rsxcap_count(ps3dk_prof_gcm_ctx *ctx, uint32_t which)
{
    uint64_t w = __atomic_load_n(SLOT, __ATOMIC_ACQUIRE);
    uint32_t e;
    const rsxcap_rec *rec;
    if (RSXCAP_STATE(w) != RSXCAP_ARMED)
        return;
    e = RSXCAP_EPOCH(w);
    if (e == 0 || e > RSXCAP_CAPS)
        return;
    rec = &g_rec[e - 1];
    if (!pal_owner_is_self(&rec->thread) || (const void *)ctx != rec->ctx || hook_stopped(g_prof))
        return;
    if (which == RSXCAP_C_IMMEDIATE) {
        sat16(&W.immediate, &W.immediate_sat);
        return;
    }
    if (which > RSXCAP_C_MISMATCH)
        return;
    if (W.capped && which != RSXCAP_C_UNSAFE)
        return; /* OQ-7: after capped, safe-draw classifications are not collected */
    side_inc(which);
}

#ifdef PS3DK_PROF_TEST_HOOKS
/* test accessors (never in an SDK archive) */
void ps3dk_prof__test_rsxcap_rec(uint32_t cap, rsxcap_rec *out);
void ps3dk_prof__test_rsxcap_rec(uint32_t cap, rsxcap_rec *out) { memcpy(out, &g_rec[cap - 1], sizeof *out); }
void ps3dk_prof__test_rsxcap_stop(void);
void ps3dk_prof__test_rsxcap_stop(void) { rc_stop(g_prof); } /* the stopper's fetch_or alone (no shutdown) */
void ps3dk_prof__test_gpu_dom(unsigned i, uint64_t *seq, uint64_t *ticket, uint32_t *depth);
void ps3dk_prof__test_gpu_dom(unsigned i, uint64_t *seq, uint64_t *ticket, uint32_t *depth)
{
    const prg_dom *d = &g_prof->gpu[i].dom;
    *seq = d->p_seq;
    *ticket = d->p_ticket;
    *depth = d->p_depth;
}
uint32_t ps3dk_prof__test_rsxcap_capable(unsigned i);
uint32_t ps3dk_prof__test_rsxcap_capable(unsigned i) { return g_prof->gpu[i].rsxcap_capable; }
/* how many table slots are eligible right now (published id, ATTACHED, capable, no fault) */
uint32_t ps3dk_prof__test_rsxcap_eligible(void);
uint32_t ps3dk_prof__test_rsxcap_eligible(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < g_prof->gpu_max; i++) /* bound: gpu_max */
        n += (uint32_t)eligible(&g_prof->gpu[i]);
    return n;
}
#endif

#endif
