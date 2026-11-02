/* RSX (GPU) zone domain core: see prg.h.  Host model (R1). */
#include "prg.h"

#include <string.h>

#define LD_ACQ(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ST_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define LD_SC(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define ST_SC(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define CAS(p, e, v) __atomic_compare_exchange_n((p), (e), (v), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define CAS_SC(p, e, v) __atomic_compare_exchange_n((p), (e), (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

enum { C_DOMAIN = 8, C_EPOCH = 9, C_CALIBRATION = 10, C_GPU_EVENTS = 14, C_LOSS = 15, C_FINAL = 16 };
enum { LOSS_DRAIN_OVERFLOW = 3, LOSS_REPORT = 4 };
enum { FIN_FINALIZED = 1, FIN_ABANDONED = 2, FIN_OVERFLOWED = 3 };

static void le(uint8_t *b, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}

/* ---------------------------------------------------------------- the fault policy */
static void fault(prg_dom *d) { ST_REL(&d->fault, 1u); }
static int faulted(prg_dom *d) { return LD_ACQ(&d->fault) != 0; }

/* every lock operation: a refusal, or an unlock reporting failure after the real unlock, latches the fault */
static int snap_lock(prg_dom *d) { return d->port.snap_lock(d->port.ctx) == 0 ? 0 : (fault(d), -1); }
static int snap_unlock(prg_dom *d) { return d->port.snap_unlock(d->port.ctx) == 0 ? 0 : (fault(d), -1); }
static int ext_lock(prg_dom *d) { return d->port.ext_lock(d->port.ctx) == 0 ? 0 : (fault(d), -1); }
static int ext_unlock(prg_dom *d) { return d->port.ext_unlock(d->port.ctx) == 0 ? 0 : (fault(d), -1); }

/* ---------------------------------------------------------------- output (drainer) */
static int emit(prg_dom *d, uint32_t type, const uint8_t *p, size_t n)
{
    if (d->sink_failed)
        return -1;
    if (d->out.chunk(d->out.ctx, type, p, n) != 0) {
        d->sink_failed = 1; /* terminal for output; the lifecycle continues output-free */
        return -1;
    }
    return 0;
}

static void declare(prg_dom *d)
{
    uint8_t b[64 + 4];
    if (d->declared)
        return;
    memset(b, 0, sizeof b);
    le(b, 1, 4);                 /* DOMAIN count */
    le(b + 4, d->domain_id, 4);
    b[8] = 2;                    /* RSX_TIMESTAMP */
    b[9] = 64;
    b[10] = 2;                   /* up */
    le(b + 12, 2, 4);            /* source_kind: GCM context */
    le(b + 16, 0, 4);            /* source_ref 0: the context association is runtime-only */
    le(b + 52, 3, 2);            /* str name "rsx" */
    memcpy(b + 54, "rsx", 3);
    if (emit(d, C_DOMAIN, b, 57) != 0)
        return;
    memset(b, 0, sizeof b);
    le(b, 1, 4);                 /* EPOCH count */
    le(b + 4, d->domain_id, 4);
    le(b + 8, 1, 4);             /* epoch 1 */
    b[12] = 1;                   /* reason start */
    le(b + 24, d->port.mftb(d->port.ctx), 8);
    if (emit(d, C_EPOCH, b, 32) == 0)
        d->declared = 1;
}

static void flush_events(prg_dom *d)
{
    size_t n = d->ev_n;
    if (!n)
        return;
    le(d->scratch, d->domain_id, 4);
    le(d->scratch + 4, 1, 4);
    le(d->scratch + 8, d->ev_first, 8);
    le(d->scratch + 16, n, 4);
    if (d->declared && emit(d, C_GPU_EVENTS, d->scratch, 20 + 28 * n) == 0)
        d->events_accepted += n;
    d->ev_n = 0;
}

static void unreserve_terminal(prg_dom *d)
{
    if (d->terminal_reserved) {
        d->terminal_reserved = 0;
        d->ledger->loss_reserved--;
    }
}

/* one LOSS chunk.  A non-terminal LOSS needs a free ledger credit; without one the domain's output stops at
 * next_emit (loss_exhausted) and the finalization's one TERMINAL LOSS, reserved at attach, covers the rest.  Returns
 * nonzero only for exhaustion (a sink failure still advances the frontier: output is dead, the accounting is not) */
static int emit_loss(prg_dom *d, uint64_t first, uint64_t count, uint8_t reason, int terminal)
{
    prg_ledger *g = d->ledger;
    uint8_t b[28];
    if (!count)
        return 0;
    if (terminal) {
        unreserve_terminal(d);
    } else if (d->loss_exhausted || (uint64_t)g->loss_used + g->loss_reserved >= g->loss_max) {
        if (!d->loss_exhausted)
            d->st.loss_exhausted++;
        d->loss_exhausted = 1;
        ST_REL(&d->exhausted, 1u); /* the sticky admission stop: calls already admitted finish (frozen frontier) */
        return -1;
    }
    g->loss_used++;
    flush_events(d);
    memset(b, 0, sizeof b);
    le(b, d->domain_id, 4);
    b[4] = 2; /* GPU domain */
    b[5] = reason;
    le(b + 8, first, 8);
    le(b + 16, count, 8);
    if (d->declared && emit(d, C_LOSS, b, sizeof b) == 0)
        d->lost_accepted += count;
    d->next_emit = first + count;
    return 0;
}

static void stage_event(prg_dom *d, const prg_meta *m, uint64_t timer)
{
    uint8_t *e;
    if (d->ev_n && (d->ev_n == d->scratch_events || d->ev_first + d->ev_n != m->seq))
        flush_events(d);
    if (!d->ev_n)
        d->ev_first = m->seq;
    e = d->scratch + 20 + 28 * d->ev_n;
    memset(e, 0, 28);
    le(e, timer, 8);
    le(e + 8, m->desc, 4);
    e[12] = m->kind;
    e[13] = m->depth;
    le(e + 16, m->frame_owner, 4);
    le(e + 20, m->frame_id, 8);
    d->ev_n++;
    d->next_emit = m->seq + 1;
}

/* ---------------------------------------------------------------- the snapshot (serialized) */
static int snap_copy_out(prg_dom *d, prg_snap *s)
{
    if (snap_lock(d) != 0)
        return -1;
    *s = d->snap;
    return snap_unlock(d); /* an unlock failure makes the copy unproven: the caller must not use it */
}

static int publish(prg_dom *d)
{
    prg_snap s;
    s.seq = d->p_seq;
    s.ticket = d->p_ticket;
    s.rec_head = d->p_rec_head;
    s.br_head = d->p_br_head;
    s.open_first = d->p_open_first;
    s.open_count = d->p_open_count;
    s.open_begins = d->p_depth;
    s.suppressed = d->p_suppressed;
    s.overflow = d->p_overflow;
    s.pad = 0;
    if (snap_lock(d) != 0)
        return -1;
    d->snap = s;
    return snap_unlock(d);
}

/* ---------------------------------------------------------------- attach */
int prg_attach(prg_dom *d, uint32_t domain_id, uint32_t report_first, uint32_t report_count, uint32_t label,
               const prg_port *port, const prg_out *out, prg_ledger *ledger, prg_meta *meta, prg_stage *stage,
               uint8_t *scratch, uint32_t scratch_events)
{
    if (!d || !port || !out || !ledger || !meta || !stage || !scratch || !scratch_events ||
        scratch_events > PRG_EVENTS_MAX || report_count < 2 ||
        report_count > PRG_SLOTS_MAX || report_first > UINT32_MAX - report_count || domain_id < 2 ||
        domain_id > 65535)
        return PRG_EINVAL;
    if (ledger->loss_max > PRG_LOSS_CHUNKS_MAX || ledger->br_epoch_max > PRG_BRACKETS_EPOCH_MAX ||
        ledger->br_file_max > PRG_BRACKETS_FILE_MAX)
        return PRG_EINVAL; /* a larger injected limit would permit output the reader rejects as MALFORMED */
    if ((uint64_t)ledger->loss_used + ledger->loss_reserved >= ledger->loss_max)
        return PRG_EINVAL; /* no terminal LOSS credit: a finalization could not account for its trailing loss */
    memset(d, 0, sizeof *d);
    d->domain_id = domain_id;
    d->report_first = report_first;
    d->report_count = report_count;
    d->label = label;
    d->port = *port;
    d->out = *out;
    d->ledger = ledger;
    d->meta = meta;
    d->stage = stage;
    d->scratch = scratch;
    d->scratch_events = scratch_events;
    ledger->loss_reserved++;
    d->terminal_reserved = 1;
    /* quiescent context (an app precondition): every slot poisoned, label 0, then a sync, before any submission */
    for (uint32_t i = 0; i < report_count; i++)
        d->port.write_timer(d->port.ctx, report_first + i, PRG_POISON);
    d->port.write_label(d->port.ctx, label, 0);
    d->port.sync(d->port.ctx);
    ST_REL(&d->state, PRG_ATTACHED);
    return PRG_OK;
}

/* ---------------------------------------------------------------- the producer */
/* The admission handshake (C11 memory model).  Admission: SC store inflight = 1, then SC load state.  Every transition
 * that stops admission out of ATTACHED (seal's ATTACHED/DETACHING -> SEALING, detach's ATTACHED -> DETACHING) is an SC
 * RMW, followed on the seal side by an SC load of inflight; release loads inflight SC.  All of these lie in the single
 * total order S of SC operations, consistent with sequenced-before.  If the admission load precedes the stopping RMW
 * in S, so does its inflight store, and the later SC load of inflight (seal, release) reads 1 or the leave() release
 * store of 0, which then synchronizes-with that load: the call's publication happens-before everything the loader
 * does next.  Otherwise the admission load follows the RMW in S and reads SEALING/DETACHING or a later state (never
 * ATTACHED again: the states are monotone), so the call is refused.  States reached only after admission has stopped
 * (SEALED, DETACH_FINAL, RELEASED) need only acq_rel.  The other stop reasons (anomaly, overflow, exhausted, fault) are
 * not part of the handshake: a call admitted before one is set completes and lies inside or beyond the frozen
 * snapshot like any other admitted call. */
static int admit(prg_dom *d)
{
    ST_SC(&d->inflight, 1u);
    if (LD_SC(&d->state) != PRG_ATTACHED || LD_ACQ(&d->anomaly) || LD_ACQ(&d->overflow) || LD_ACQ(&d->exhausted) ||
        faulted(d)) {
        ST_REL(&d->inflight, 0u);
        return 0;
    }
    return 1;
}

static void leave(prg_dom *d) { ST_REL(&d->inflight, 0u); }

/* the producer's exit: a failed publication is terminal (EFAULT), never ignored */
static int publish_leave(prg_dom *d, int rc)
{
    if (publish(d) != 0)
        rc = PRG_EFAULT;
    leave(d);
    return rc;
}

static void lose(prg_dom *d, uint64_t s)
{
    if (!d->p_open_count)
        d->p_open_first = s;
    d->p_open_count++; /* contiguous: the open loss always ends at s - 1 */
}

/* number and submit one event; the caller has admitted; returns the number or UINT64_MAX (overflow) */
static uint64_t submit(prg_dom *d, uint8_t kind, uint8_t depth, uint32_t desc)
{
    uint64_t s, t = d->p_ticket;
    uint32_t slot;
    if (d->p_seq == UINT64_MAX || d->p_ticket == UINT64_MAX) {
        d->p_overflow = 1;
        ST_REL(&d->overflow, 1u);
        return UINT64_MAX;
    }
    s = d->p_seq++;
    slot = (uint32_t)(t % d->report_count);
    if (t - LD_ACQ(&d->drained_pub) >= d->report_count) { /* the slot's previous ticket is not drained */
        d->st.lost_ring++;
        lose(d, s);
        return s;
    }
    if (d->p_open_count && d->p_rec_head - LD_ACQ(&d->rec_tail) >= PRG_INTERVALS) { /* no credit to close the loss */
        d->st.lost_records++;
        lose(d, s);
        return s;
    }
    if (d->port.reserve_pair(d->port.ctx, d->report_first + slot, d->label, (uint32_t)(t + 1)) != 0) {
        d->st.lost_fifo++;
        lose(d, s);
        return s;
    }
    if (d->p_open_count) { /* close the open loss into an immutable record before this event becomes visible */
        prg_interval *r = &d->rec[d->p_rec_head % PRG_INTERVALS];
        r->first = d->p_open_first;
        r->count = d->p_open_count;
        d->p_rec_head++;
        d->p_open_count = 0;
    }
    d->meta[slot] = (prg_meta){s, d->p_frame_id, desc, d->p_frame_owner, kind, depth, 0, d->p_owner_ref};
    d->p_ticket = t + 1;
    return s;
}

int prg_begin(prg_dom *d, uint32_t desc, prg_token *tok)
{
    uint64_t s;
    if (!tok)
        return d->st.refused_api++, PRG_EINVAL;
    *tok = (prg_token){0, 0, 0, 0, 0}; /* domain 0 never matches: an unissued token is refused by prg_end */
    if (!admit(d))
        return d->st.refused_api++, PRG_ESTOPPED;
    if (d->p_depth >= PRG_DEPTH) {
        if (d->p_suppressed >= PRG_SUPPRESS_MAX || d->p_sup_serial == UINT64_MAX) {
            d->st.refused_suppress_overflow++;
            leave(d);
            return PRG_EOVERFLOW;
        }
        /* SUPPRESSED: no number; the token carries its level and a never-reused issuance serial, so only the innermost
         * suppressed zone can end, and a stale token of an ended zone never matches a later one at the same level */
        d->p_sup_stack[d->p_suppressed++] = ++d->p_sup_serial;
        *tok = (prg_token){d->domain_id, 0x80000000u | d->p_suppressed, d->p_sup_serial, desc, 0};
        return publish_leave(d, PRG_OK);
    }
    s = submit(d, PRG_K_BEGIN, (uint8_t)d->p_depth, desc);
    if (s != UINT64_MAX) {
        d->p_stack[d->p_depth] = (prg_frame_ent){s, desc, 0};
        *tok = (prg_token){d->domain_id, d->p_depth, s, desc, 0};
        d->p_depth++;
    }
    return publish_leave(d, s == UINT64_MAX ? PRG_EOVERFLOW : PRG_OK);
}

int prg_end(prg_dom *d, const prg_token *tok, uint32_t desc)
{
    uint64_t s;
    const prg_frame_ent *top;
    if (!tok || tok->domain != d->domain_id || tok->desc != desc)
        return d->st.refused_api++, PRG_EINVAL;
    if (!admit(d))
        return d->st.refused_api++, PRG_ESTOPPED;
    if (tok->depth & 0x80000000u) {
        if (!d->p_suppressed || (tok->depth & 0x7fffffffu) != d->p_suppressed ||
            tok->begin_seq != d->p_sup_stack[d->p_suppressed - 1]) {
            leave(d);
            return d->st.refused_api++, PRG_EINVAL; /* not the innermost suppressed zone (or already ended) */
        }
        d->p_suppressed--;
        return publish_leave(d, PRG_OK);
    }
    top = d->p_depth ? &d->p_stack[d->p_depth - 1] : NULL;
    if (d->p_suppressed || !top || tok->depth != d->p_depth - 1 || top->seq != tok->begin_seq || top->desc != desc) {
        leave(d);
        return d->st.refused_api++, PRG_EINVAL; /* not the top of the stack, or not its descriptor: no number */
    }
    s = submit(d, PRG_K_END, (uint8_t)tok->depth, desc);
    if (s != UINT64_MAX)
        d->p_depth--;
    return publish_leave(d, s == UINT64_MAX ? PRG_EOVERFLOW : PRG_OK);
}

int prg_mark(prg_dom *d, uint32_t desc)
{
    uint64_t s;
    if (!admit(d))
        return d->st.refused_api++, PRG_ESTOPPED;
    if (d->p_depth >= PRG_DEPTH) { /* inside suppressed nesting: depth 64 has no encoding; refused, no number */
        d->st.refused_mark_depth++;
        leave(d);
        return PRG_EOVERFLOW;
    }
    s = submit(d, PRG_K_MARK, (uint8_t)d->p_depth, desc);
    return publish_leave(d, s == UINT64_MAX ? PRG_EOVERFLOW : PRG_OK);
}

int prg_frame(prg_dom *d, uint32_t owner, uint64_t frame_id, uint8_t owner_ref)
{
    if (owner == 0 && frame_id != 0)
        return PRG_EINVAL;
    if (!admit(d))
        return PRG_ESTOPPED;
    d->p_frame_owner = owner; /* validated by the runtime (R2): the core records what it is given */
    d->p_frame_id = frame_id;
    d->p_owner_ref = owner_ref;
    leave(d);
    return PRG_OK;
}

/* ---------------------------------------------------------------- calibration (ctx owner thread) */
/* *done: the readable prefix (capped at issued); *dist: the label's true modular distance from completed, so two reads
 * against the SAME baseline compare without the cap hiding a backward step */
static int label_window(prg_dom *d, uint32_t L, uint64_t completed, uint64_t issued, uint64_t *done, uint64_t *dist)
{
    uint64_t dd = (uint32_t)(L - (uint32_t)completed);
    *dist = dd;
    if (dd <= issued - completed) {
        *done = completed + dd;
        return 0;
    }
    if (dd <= d->report_count) {
        /* the label ran past the published ticket (the GPU may see a submission before its ticket is published):
         * completions are in order, so every PUBLISHED ticket is done; only the unpublished extra is deferred */
        *done = issued;
        return 0;
    }
    return -1; /* backward or beyond the window */
}

/* The bracket's labels are checked against ONE window pinned at submission: base = drained_pub then (a lower bound
 * of every later label), top = the caller's own ticket t + 1 (the ctx owner is the only submitter, so the label can
 * never legitimately pass it).  Both reads must be exactly t + 1. */
int prg_calibrate(prg_dom *d, uint64_t timeout_us)
{
    uint64_t t, base, span, tb0, now, deadline;
    uint32_t slot, e, outcome;
    prg_bracket b = {0, 0, 0};
    int rc = PRG_OK;
    if (!admit(d))
        return PRG_ESTOPPED;
    if (LD_ACQ(&d->cal_state) != PRG_CAL_IDLE) {
        leave(d);
        return PRG_EBUSY;
    }
    if (d->port.now_us(d->port.ctx, &now) != 0) {
        leave(d);
        return PRG_ECLOCK;
    }
    deadline = timeout_us > UINT64_MAX - now ? UINT64_MAX : now + timeout_us;
    t = d->p_ticket;
    base = LD_ACQ(&d->drained_pub);
    slot = (uint32_t)(t % d->report_count);
    if (t == UINT64_MAX || t - base >= d->report_count || d->p_br_head - LD_ACQ(&d->br_tail) >= PRG_BRACKETS) {
        leave(d);
        return PRG_EBUSY; /* no free slot, or no bracket credit */
    }
    span = t + 1 - base;
    tb0 = d->port.mftb(d->port.ctx);
    d->port.sync(d->port.ctx);
    if (d->port.reserve_pair(d->port.ctx, d->report_first + slot, d->label, (uint32_t)(t + 1)) != 0) {
        leave(d);
        return PRG_EBUSY;
    }
    d->meta[slot] = (prg_meta){0, 0, 0, 0, 0, 0, 1, 0};
    d->cal_ticket = t;
    ST_REL(&d->cal_state, (uint32_t)PRG_CAL_ACTIVE);
    d->p_ticket = t + 1;
    if (publish(d) != 0) {
        leave(d);
        return PRG_EFAULT; /* the cell stays ACTIVE: the fault stops every drainer */
    }
    d->port.flush(d->port.ctx);
    outcome = PRG_CAL_CONSUMED;
    for (;;) { /* poll the label (read only: the drainer alone advances completed) */
        uint64_t dd = (uint32_t)(d->port.read_label(d->port.ctx, d->label) - (uint32_t)base);
        if (dd > span) {
            outcome = PRG_CAL_BAD; /* backward or beyond the caller's own ticket */
            rc = PRG_EANOMALY;
            break;
        }
        if (dd == span)
            break;
        if (d->port.now_us(d->port.ctx, &now) != 0) {
            outcome = PRG_CAL_CANCELLED;
            rc = PRG_ECLOCK;
            break;
        }
        if (now >= deadline) {
            outcome = PRG_CAL_CANCELLED;
            rc = PRG_ETIMEDOUT;
            break;
        }
    }
    if (rc == PRG_OK) {
        d->port.sync(d->port.ctx);
        b.timer = d->port.read_timer(d->port.ctx, d->report_first + slot);
        d->port.sync(d->port.ctx);
        if ((uint32_t)(d->port.read_label(d->port.ctx, d->label) - (uint32_t)base) != span ||
            b.timer == PRG_POISON) {
            outcome = PRG_CAL_BAD; /* the second label left the window, or A1 failed: the anomaly policy */
            rc = PRG_EANOMALY;
        } else {
            b.tb0 = tb0;
            b.tb1 = d->port.mftb(d->port.ctx);
            if (b.tb1 <= b.tb0 || b.tb0 < d->p_last_tb1)
                rc = PRG_ECLOCK; /* the timebase did not advance, or the bracket overlaps the last one: none */
            else
                d->br[d->p_br_head % PRG_BRACKETS] = b; /* invisible until published below */
        }
    }
    e = PRG_CAL_ACTIVE;
    if (!CAS(&d->cal_state, &e, outcome)) {
        leave(d);
        return PRG_ESTOPPED; /* the finalization cancelled the cell first: no bracket, the outcome is its */
    }
    if (rc == PRG_OK) {
        d->p_last_tb1 = b.tb1;
        d->p_br_head++;
        d->st.brackets++;
        return publish_leave(d, PRG_OK);
    }
    if (outcome == PRG_CAL_BAD)
        d->st.cal_bad++;
    leave(d);
    return rc;
}

/* ---------------------------------------------------------------- the drainer */
static void anomaly(prg_dom *d) { ST_REL(&d->anomaly, 1u); }

/* the brackets below the FROZEN head h (a snapshot field, never the live producer head), within the ledger budget */
static void emit_brackets(prg_dom *d, uint64_t h)
{
    prg_ledger *g = d->ledger;
    uint8_t b[20 + 24 * PRG_BRACKETS];
    uint32_t n = 0, kept = 0;
    while (d->br_tail + n < h && n < PRG_BRACKETS) {
        const prg_bracket *x = &d->br[(d->br_tail + n) % PRG_BRACKETS];
        n++;
        if (d->br_epoch_used >= g->br_epoch_max || g->br_file_used >= g->br_file_max) {
            d->st.brackets_dropped++; /* the per-epoch or per-file budget */
            continue;
        }
        d->br_epoch_used++;
        g->br_file_used++;
        le(b + 20 + 24 * kept, x->timer, 8);
        le(b + 28 + 24 * kept, x->tb0, 8);
        le(b + 36 + 24 * kept, x->tb1, 8);
        kept++;
    }
    if (kept) {
        le(b, d->domain_id, 4);
        le(b + 4, 1, 4);
        le(b + 8, d->br_seq, 8);
        le(b + 16, kept, 4);
        if (d->declared)
            (void)emit(d, C_CALIBRATION, b, 20 + 24 * (size_t)kept);
        d->br_seq += kept;
    }
    if (n)
        ST_REL(&d->br_tail, d->br_tail + n);
}

/* emit published loss records whose first number is the next to emit; once the LOSS budget is exhausted, records
 * are retired unemitted (the terminal LOSS covers them) */
static void emit_records(prg_dom *d, uint64_t rec_head)
{
    while (d->rec_tail < rec_head) {
        const prg_interval *r = &d->rec[d->rec_tail % PRG_INTERVALS];
        if (!d->loss_exhausted) {
            if (r->first != d->next_emit)
                break;
            (void)emit_loss(d, r->first, r->count, LOSS_DRAIN_OVERFLOW, 0);
        }
        ST_REL(&d->rec_tail, d->rec_tail + 1);
    }
}

/* VALIDATE then COMMIT.  The read phase stages every completed report (and checks poison, monotonicity, the
 * calibration outcomes and the number continuity against the snapshot's records); only after the second label read
 * validates the interval does anything reach the output, the poison writes or drained_pub (slot reuse). */
static void drain_once(prg_dom *d, const prg_snap *s, int final)
{
    uint32_t L1, L2;
    uint64_t done1, done2, dist1, dist2, n, last, ne, rt;
    if (LD_ACQ(&d->anomaly))
        return;
    declare(d);
    L1 = d->port.read_label(d->port.ctx, d->label);
    if (label_window(d, L1, d->completed, s->ticket, &done1, &dist1) != 0) {
        anomaly(d);
        return;
    }
    d->port.sync(d->port.ctx);
    last = d->last_timer;
    ne = d->next_emit;
    rt = d->rec_tail;
    for (n = 0; d->drained + n < done1; n++) {
        uint32_t slot = (uint32_t)((d->drained + n) % d->report_count);
        prg_stage *x = &d->stage[n];
        x->m = d->meta[slot];
        x->slot = slot;
        if (x->m.is_cal) {
            uint32_t c = LD_ACQ(&d->cal_state);
            if (c == PRG_CAL_ACTIVE && final && CAS(&d->cal_state, &c, (uint32_t)PRG_CAL_CANCELLED))
                c = PRG_CAL_CANCELLED; /* the finalization takes the cell: the caller's later CAS fails */
            if (c == PRG_CAL_ACTIVE)
                break; /* the caller owns the slot: the drained prefix stops here */
            if (c != PRG_CAL_CONSUMED && c != PRG_CAL_CANCELLED) {
                anomaly(d); /* BAD (the caller's window or poison check failed), or a cell that is not in flight */
                return;
            }
            continue;
        }
        if (d->port.emittable && !d->port.emittable(d->port.ctx, &x->m)) {
            d->st.held++; /* HOLD: the prefix stops here; the slot stays owned; next pass retries */
            break;
        }
        x->timer = d->port.read_timer(d->port.ctx, d->report_first + slot);
        if (x->timer == PRG_POISON || x->timer < last) {
            anomaly(d); /* label before report, poison collision or a decrease */
            return;
        }
        last = x->timer;
        if (!d->loss_exhausted) {
            while (rt < s->rec_head && d->rec[rt % PRG_INTERVALS].first == ne) {
                ne = d->rec[rt % PRG_INTERVALS].first + d->rec[rt % PRG_INTERVALS].count;
                rt++;
            }
            if (x->m.seq != ne) {
                anomaly(d); /* a gap no record covers: never emit a falsely identified event */
                return;
            }
            ne = x->m.seq + 1;
        }
    }
    d->port.sync(d->port.ctx);
    L2 = d->port.read_label(d->port.ctx, d->label);
    if (label_window(d, L2, d->completed, s->ticket, &done2, &dist2) != 0 || done2 < done1 || dist2 < dist1) {
        anomaly(d); /* the second read went backward or out of the window: nothing of this interval is committed */
        return;
    }
    d->completed = done1;
    for (uint64_t i = 0; i < n; i++) {
        const prg_stage *x = &d->stage[i];
        if (x->m.is_cal) {
            d->port.write_timer(d->port.ctx, d->report_first + x->slot, PRG_POISON);
            ST_REL(&d->cal_state, (uint32_t)PRG_CAL_IDLE);
        } else {
            emit_records(d, s->rec_head);
            if (!d->loss_exhausted)
                stage_event(d, &x->m, x->timer);
            d->last_timer = x->timer;
            d->port.write_timer(d->port.ctx, d->report_first + x->slot, PRG_POISON);
        }
        d->drained++;
        ST_REL(&d->drained_pub, d->drained); /* the slot is free only now */
    }
    emit_records(d, s->rec_head);
    flush_events(d);
    emit_brackets(d, s->br_head);
}

static int finalize(prg_dom *d);

/* the caller holds the extension mutex.  DOMAIN + EPOCH are declared first, whatever the fault state: they need no
 * snapshot, and a faulted domain must still appear in the file (unfinalized), never vanish.  With drainer_finalizes,
 * a DETACHING domain whose published work is all drained is finalized HERE, by the drainer (the sole encoder). */
void prg_drain_held(prg_dom *d)
{
    prg_snap s;
    uint32_t st = LD_ACQ(&d->state), e = PRG_DETACHING;
    if (!LD_ACQ(&d->retired) && st != PRG_FREE)
        declare(d);
    if (faulted(d) || LD_ACQ(&d->retired) || (st != PRG_ATTACHED && st != PRG_DETACHING) || snap_copy_out(d, &s) != 0)
        return;
    drain_once(d, &s, 0);
    if (d->drainer_finalizes && st == PRG_DETACHING && LD_ACQ(&d->drained_pub) == s.ticket &&
        d->rec_tail == s.rec_head && CAS(&d->state, &e, (uint32_t)PRG_DETACH_FINAL)) {
        (void)finalize(d); /* -1 (an unproven snapshot): no FINAL, not retired; the waiter sees the fault */
        d->detach_turn = 1; /* the runtime acknowledges this turn after its closing extension unlock */
    }
}

void prg_drain(prg_dom *d)
{
    if (faulted(d) || ext_lock(d) != 0)
        return;
    prg_drain_held(d);
    (void)ext_unlock(d);
}

/* the finalization owner's work, under the ext lock: the frozen frontier, the one terminal LOSS, FINAL by
 * precedence, retirement.  Nonzero (no FINAL, not retired) when the snapshot cannot be proven: the fault policy */
static int finalize(prg_dom *d)
{
    prg_snap s;
    uint8_t b[4 + 36];
    uint64_t F;
    uint32_t state;
    int pending;
    if (snap_copy_out(d, &s) != 0)
        return -1;
    drain_once(d, &s, 1);
    declare(d);
    emit_records(d, s.rec_head);
    F = s.seq;
    /* pending = a submitted ticket not drained, or a numbered event outside the open loss not yet emitted (events
     * withheld by an exhausted LOSS budget were drained: they are lost, not pending) */
    pending = d->drained < s.ticket || (!d->loss_exhausted && (s.open_count ? s.open_first : F) > d->next_emit);
    if (d->next_emit < F)
        (void)emit_loss(d, d->next_emit, F - d->next_emit, pending ? LOSS_REPORT : LOSS_DRAIN_OVERFLOW, 1);
    unreserve_terminal(d);
    flush_events(d);
    emit_brackets(d, s.br_head);
    state = LD_ACQ(&d->anomaly)                ? FIN_ABANDONED
            : s.overflow || d->loss_exhausted ? FIN_OVERFLOWED /* the seq/ticket space or the LOSS budget */
            : pending                         ? FIN_ABANDONED
                                              : FIN_FINALIZED;
    memset(b, 0, sizeof b);
    le(b, 1, 4);
    le(b + 4, d->domain_id, 4);
    b[8] = 2;
    b[9] = (uint8_t)state;
    le(b + 12, F, 8);
    le(b + 20, d->events_accepted, 8);
    le(b + 28, d->lost_accepted, 8);
    le(b + 36, s.open_begins, 4);
    d->final_state = state;
    ST_REL(&d->retired, 1u); /* the one terminal decision, whatever the sink does with its chunk */
    if (d->declared && emit(d, C_FINAL, b, sizeof b) == 0)
        d->final_written = 1;
    return 0;
}

/* the seal finalizes only under the ext lock and only from a proven snapshot; on a fault it does nothing (the file
 * stays incomplete, the resources stay borrowed) */
/* the caller holds the extension mutex */
void prg_seal_held(prg_dom *d)
{
    uint32_t e = PRG_ATTACHED;
    if (!LD_ACQ(&d->retired) && LD_ACQ(&d->state) != PRG_FREE)
        declare(d); /* as prg_drain_held: a faulted domain is still declared */
    if (faulted(d) ||
        (!CAS_SC(&d->state, &e, (uint32_t)PRG_SEALING) &&
         (e != PRG_DETACHING || !CAS_SC(&d->state, &e, (uint32_t)PRG_SEALING))))
        return; /* a fault, or already terminal: the other path owns the finalization */
    (void)LD_SC(&d->inflight); /* the handshake: an in-flight call either saw SEALING or lies beyond the snapshot */
    if (finalize(d) == 0)
        ST_REL(&d->state, PRG_SEALED);
}

void prg_seal(prg_dom *d)
{
    if (faulted(d) || ext_lock(d) != 0)
        return;
    prg_seal_held(d);
    (void)ext_unlock(d);
}

int prg_detach(prg_dom *d, uint64_t timeout_us, void (*poll_hook)(void *), void *poll_ctx)
{
    uint64_t now, deadline;
    uint32_t e = PRG_ATTACHED;
    int rc;
    if (faulted(d))
        return PRG_EFAULT;
    if (!CAS_SC(&d->state, &e, (uint32_t)PRG_DETACHING) && e != PRG_DETACHING)
        return e == PRG_SEALING || e == PRG_SEALED ? PRG_ESTOPPED : PRG_EINVAL;
    if (d->port.now_us(d->port.ctx, &now) != 0)
        return PRG_ECLOCK;
    deadline = timeout_us > UINT64_MAX - now ? UINT64_MAX : now + timeout_us;
    for (;;) {
        prg_snap s;
        if (poll_hook)
            poll_hook(poll_ctx); /* e.g. an inline drain when detach runs on the designated drainer thread */
        if (faulted(d))
            return PRG_EFAULT;
        if (LD_ACQ(&d->state) != PRG_DETACHING)
            return PRG_ESTOPPED; /* the seal won: the frozen result stands */
        if (snap_copy_out(d, &s) != 0)
            return PRG_EFAULT;
        /* every ticket drained and every published record emitted; a private open loss does not block (the
         * finalization emits it) */
        if (LD_ACQ(&d->drained_pub) == s.ticket && LD_ACQ(&d->rec_tail) == s.rec_head)
            break;
        if (d->port.now_us(d->port.ctx, &now) != 0)
            return PRG_ECLOCK;
        if (now >= deadline)
            return PRG_ETIMEDOUT; /* stays DETACHING; ownership retained; call again */
    }
    if (ext_lock(d) != 0)
        return PRG_EFAULT;
    e = PRG_DETACHING;
    if (faulted(d) || !CAS(&d->state, &e, (uint32_t)PRG_DETACH_FINAL)) {
        (void)ext_unlock(d); /* a fault, or the seal won under the same lock */
        return faulted(d) ? PRG_EFAULT : PRG_ESTOPPED;
    }
    rc = finalize(d) == 0 ? PRG_OK : PRG_EFAULT;
    if (ext_unlock(d) != 0)
        rc = PRG_EFAULT; /* the finalization (if any) stands; the fault is reported */
    return rc;
}

/* retirement happens under the ext lock and nothing touches the GPU resources after it, so a later unlock fault does
 * not block the release; a domain that never retired (a fault before its finalization) stays borrowed.  The release
 * returns the GPU resources only: the port's mutex storage and this object stay alive (a later user observes the
 * fault), and a mutex whose unlock reported failure must never be destroyed on the strength of this release */
int prg_detach_wait(prg_dom *d, uint64_t timeout_us, void (*poll_hook)(void *), void *poll_ctx)
{
    uint64_t now, deadline;
    uint32_t e = PRG_ATTACHED;
    if (faulted(d))
        return PRG_EFAULT;
    if (!CAS_SC(&d->state, &e, (uint32_t)PRG_DETACHING) && e != PRG_DETACHING && e != PRG_DETACH_FINAL)
        return e == PRG_SEALING || e == PRG_SEALED ? PRG_ESTOPPED : PRG_EINVAL;
    if (d->port.now_us(d->port.ctx, &now) != 0)
        return PRG_ECLOCK;
    deadline = timeout_us > UINT64_MAX - now ? UINT64_MAX : now + timeout_us;
    for (;;) {
        if (poll_hook)
            poll_hook(poll_ctx); /* e.g. one drain on the designated drainer thread, or a short sleep */
        if (faulted(d))
            return PRG_EFAULT;
        e = LD_ACQ(&d->state);
        if (e == PRG_DETACH_FINAL) {
            uint32_t ack = LD_ACQ(&d->detach_ack); /* not `retired` alone: that is set before FINAL is offered */
            if (ack == 1 && LD_ACQ(&d->retired))
                return PRG_OK; /* the acknowledged turn AND the frozen decision */
            if (ack == 2)
                return PRG_EFAULT;
        }
        if (e == PRG_SEALING || e == PRG_SEALED)
            return PRG_ESTOPPED; /* the seal finalized it: the frozen result stands */
        if (d->port.now_us(d->port.ctx, &now) != 0)
            return PRG_ECLOCK;
        if (now >= deadline)
            return PRG_ETIMEDOUT; /* stays DETACHING; call again */
    }
}

int prg_release(prg_dom *d)
{
    uint32_t e;
    if (!LD_ACQ(&d->retired))
        return PRG_EBUSY;
    if (LD_SC(&d->inflight))
        return PRG_EBUSY; /* the admitted call has not acknowledged its exit (or this is callback reentry) */
    e = PRG_SEALED;
    if (CAS(&d->state, &e, (uint32_t)PRG_RELEASED))
        return PRG_OK;
    e = PRG_DETACH_FINAL;
    if (CAS(&d->state, &e, (uint32_t)PRG_RELEASED))
        return PRG_OK;
    return PRG_EINVAL; /* already released, or never retired through either path */
}
