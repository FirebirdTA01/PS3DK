/* RSX (GPU) zone domain core (design P3.3 r2 + r3/r4/r5 deltas, R1 corrections review 11:52): portable C behind a
 * port, HOST MODEL ONLY in slice R1.  One domain per GCM context.  The render (ctx owner) thread numbers and submits
 * events; one drainer at a time (the ext lock) reads completions and emits chunks; publication between them is a
 * serialized snapshot (snap lock).
 *
 * Counters (u64, never mixed): seq (numbered events), ticket (submitted timestamp+label pairs, calibration included),
 * completed (labels seen), drained (reports read and committed, slots poisoned), accepted (events and LOSS in accepted
 * chunks).  A1 (report visible before its label) and A2 (coherent report read) are ASSUMPTIONS; poison and
 * monotonicity are diagnostics only.  Mapping UNMEASURED.
 *
 * Fault policy: any lock or publication failure (lock refused, or unlock reporting failure after the real unlock)
 * latches the sticky `fault` word: no further admission, no work without lock ownership, no finalization from a
 * fabricated snapshot; the last PROVEN published/accepted state stands (the file stays without this domain's FINAL)
 * and a domain that never retired stays borrowed (a fault after a completed finalization, e.g. its closing ext
 * unlock, does not block the release).  A genuinely stuck mutex has no time bound (documented residual).
 *
 * Read intervals are VALIDATE-then-COMMIT: nothing of an interval reaches the output, the poison writes or
 * drained_pub (slot reuse) before its second label read validates it.  Calibration brackets are validated by the
 * caller against one pinned window; their queue frontier is part of the snapshot.  LOSS chunks and brackets are
 * bounded by the injected ledger (one terminal LOSS reserved per domain at attach).
 *
 * Delegated to R2 (runtime integration), not enforced here: the report area size and the label's user range, overlap
 * with other domains' ranges/labels/contexts, the shared SPU+GPU ledger (R1 takes an injected one), frame-owner and
 * scope-2 descriptor HOLDS until their v1 declarations are accepted, and frame-token validation against live
 * incarnations (the core records the owner/frame it is given). */
#ifndef PRG_H
#define PRG_H

#include <stddef.h>
#include <stdint.h>

#define prg_attach ps3dk_prof__prg_attach
#define prg_begin ps3dk_prof__prg_begin
#define prg_end ps3dk_prof__prg_end
#define prg_mark ps3dk_prof__prg_mark
#define prg_frame ps3dk_prof__prg_frame
#define prg_calibrate ps3dk_prof__prg_calibrate
#define prg_drain ps3dk_prof__prg_drain
#define prg_seal ps3dk_prof__prg_seal
#define prg_detach ps3dk_prof__prg_detach
#define prg_release ps3dk_prof__prg_release
#define prg_drain_held ps3dk_prof__prg_drain_held
#define prg_seal_held ps3dk_prof__prg_seal_held
#define prg_detach_wait ps3dk_prof__prg_detach_wait

#define PRG_SLOTS_MAX 1024u
#define PRG_INTERVALS 16u
#define PRG_BRACKETS 64u
#define PRG_DEPTH 64u
#define PRG_SUPPRESS_MAX 64u /* suppressed levels beyond PRG_DEPTH (bounded: each holds its issuance serial) */
#define PRG_POISON 0xFFFFFFFFFFFFFFFFull
#define PRG_EVENTS_MAX 37448u /* one GPU_EVENTS chunk */
/* the reader's file-wide bounds, the defaults for a ledger */
#define PRG_LOSS_CHUNKS_MAX 65536u
#define PRG_BRACKETS_EPOCH_MAX 65536u
#define PRG_BRACKETS_FILE_MAX 262144u

enum { PRG_OK = 0, PRG_EINVAL = -1, PRG_EBUSY = -2, PRG_ESTOPPED = -3, PRG_ETIMEDOUT = -4, PRG_ECLOCK = -5,
       PRG_EOVERFLOW = -6, PRG_EFAULT = -7, PRG_EANOMALY = -8 };
/* admission state: monotone, changed only by CAS */
enum { PRG_FREE = 0, PRG_ATTACHED, PRG_DETACHING, PRG_SEALING, PRG_SEALED, PRG_DETACH_FINAL, PRG_RELEASED };
enum { PRG_K_BEGIN = 1, PRG_K_END = 2, PRG_K_MARK = 3 };
/* the calibration cell: IDLE -> ACTIVE (the caller owns its slot) -> exactly one CAS to an outcome (the caller's
 * CONSUMED/BAD/CANCELLED, or the finalization's CANCELLED) -> IDLE when the drainer consumes the slot; BAD latches
 * the anomaly when the drainer reaches it */
enum { PRG_CAL_IDLE = 0, PRG_CAL_ACTIVE, PRG_CAL_CONSUMED, PRG_CAL_CANCELLED, PRG_CAL_BAD };

typedef struct prg_meta prg_meta;

/* the GPU and memory port: reserve_pair is ALL-OR-NOTHING (0: both commands written; nonzero: nothing written) */
typedef struct prg_port {
    void *ctx;
    int (*reserve_pair)(void *ctx, uint32_t report, uint32_t label, uint32_t value);
    void (*flush)(void *ctx);
    uint32_t (*read_label)(void *ctx, uint32_t label);
    void (*write_label)(void *ctx, uint32_t label, uint32_t value);
    uint64_t (*read_timer)(void *ctx, uint32_t report);
    void (*write_timer)(void *ctx, uint32_t report, uint64_t value);
    void (*sync)(void *ctx);
    uint64_t (*mftb)(void *ctx);
    int (*now_us)(void *ctx, uint64_t *us); /* the monotonic clock for deadlines; nonzero = ECLOCK */
    int (*snap_lock)(void *ctx);            /* the per-domain snapshot lock; nonzero = failure (the fault policy) */
    int (*snap_unlock)(void *ctx);
    int (*ext_lock)(void *ctx);             /* the extension mutex: drains and the one finalization are serialized */
    int (*ext_unlock)(void *ctx);
    /* R2: may this staged event be committed now?  1 yes; 0 HOLD (the drained prefix stops at it, nothing of it or
     * after it is committed this pass, its slot stays owned; at finalization it is pending).  NULL: always 1.
     * Called by the drainer in the read phase only. */
    int (*emittable)(void *ctx, const prg_meta *m);
} prg_port;

/* chunk output (the drainer's sink): 0 accepted, nonzero refused (a sink failure is terminal for output) */
typedef struct prg_out {
    void *ctx;
    int (*chunk)(void *ctx, uint32_t type, const uint8_t *payload, size_t len);
} prg_out;

/* the file-wide aggregate ledger (the reader's limits); R1 injects one, R2 shares it with the SPU lanes.  Every field
 * is touched only by drainer-side code under the ext lock. */
typedef struct prg_ledger {
    uint32_t loss_max, loss_used, loss_reserved; /* LOSS chunks per file; one terminal LOSS reserved per domain */
    uint32_t br_epoch_max;                       /* brackets per (domain, epoch) */
    uint32_t br_file_max, br_file_used;          /* brackets per file */
} prg_ledger;

struct prg_meta { /* written before the ticket is published; immutable until the slot is drained */
    uint64_t seq, frame_id;
    uint32_t desc, frame_owner;
    uint8_t kind, depth, is_cal, owner_ref; /* owner_ref: the runtime's owner entry (0 none), for emittable */
};

typedef struct prg_stage { /* one tentatively read report, committed only after the second label read */
    uint64_t timer;
    prg_meta m;
    uint32_t slot, pad;
} prg_stage;

typedef struct prg_interval { /* an immutable published loss record */
    uint64_t first, count;
} prg_interval;

typedef struct prg_snap { /* the one publication: copied in and out under the snap lock only */
    uint64_t seq, ticket, rec_head, br_head;
    uint64_t open_first, open_count; /* the open loss, as of this snapshot */
    uint32_t open_begins, suppressed, overflow, pad;
} prg_snap;

typedef struct prg_bracket {
    uint64_t timer, tb0, tb1;
} prg_bracket;

typedef struct prg_token {
    uint32_t domain, depth; /* SUPPRESSED (no number): 0x80000000 | the suppressed level */
    uint64_t begin_seq;     /* the BEGIN's number; for a suppressed token its never-reused issuance serial */
    uint32_t desc, pad;
} prg_token;

typedef struct prg_frame_ent {
    uint64_t seq;
    uint32_t desc, pad;
} prg_frame_ent;

typedef struct prg_stats {
    uint64_t refused_api, refused_suppress_overflow, refused_mark_depth, lost_ring, lost_fifo, lost_records;
    uint64_t brackets, brackets_dropped, cal_bad, loss_exhausted, held;
} prg_stats;

typedef struct prg_dom {
    /* configuration (attach) */
    uint32_t domain_id, report_first, report_count, label;
    prg_port port;
    prg_out out;
    prg_ledger *ledger;
    prg_meta *meta;   /* report_count entries, caller storage */
    prg_stage *stage; /* report_count entries, caller storage */
    /* producer (ctx owner thread) private */
    uint64_t p_seq, p_ticket, p_open_first, p_open_count, p_rec_head, p_br_head;
    uint32_t p_depth, p_suppressed, p_overflow;
    prg_frame_ent p_stack[PRG_DEPTH];
    uint32_t p_frame_owner;
    uint64_t p_frame_id, p_last_tb1; /* p_last_tb1: the last accepted bracket's end (brackets never overlap) */
    uint64_t p_sup_serial, p_sup_stack[PRG_SUPPRESS_MAX]; /* suppressed issuance serials (0 never issued) */
    uint8_t p_owner_ref;
    /* published */
    prg_snap snap;
    prg_interval rec[PRG_INTERVALS];
    prg_bracket br[PRG_BRACKETS];
    uint64_t rec_tail;     /* drainer -> producer (atomic) */
    uint64_t br_tail;      /* drainer -> producer (atomic) */
    uint64_t drained_pub;  /* drainer -> producer (atomic): slots below it are free */
    uint32_t state, inflight, anomaly, overflow, fault, retired; /* atomics */
    uint32_t exhausted; /* atomic: the drainer exhausted the LOSS budget (a sticky admission stop, FINAL OVERFLOWED) */
    uint32_t cal_state;                                          /* atomic: the calibration cell */
    uint64_t cal_ticket;                                         /* producer private (diagnostic) */
    /* drainer private (under the ext lock) */
    uint64_t completed, drained, next_emit, last_timer, br_epoch_used, br_seq;
    uint32_t declared, sink_failed, final_written, final_state, terminal_reserved, loss_exhausted;
    uint64_t events_accepted, lost_accepted;
    uint32_t drainer_finalizes; /* R2: set by the runtime after attach: the drainer alone finalizes a detach */
    uint32_t detach_turn;       /* R2, drainer-private: this drain turn finalized the detach (the runtime acks it) */
    uint32_t detach_ack;        /* R2, atomic: 0 pending; 1 the finalizing turn completed, its closing extension
                                 * unlock included; 2 that unlock failed.  Published by the runtime, never the core */
    uint8_t *scratch; /* one GPU_EVENTS chunk, caller storage: PRG_SCRATCH_FOR(scratch_events) bytes */
    size_t ev_n, scratch_events;
    uint64_t ev_first;
    prg_stats st;
} prg_dom;

#define PRG_SCRATCH_FOR(n) (20u + 28u * (size_t)(n))
#define PRG_SCRATCH_BYTES PRG_SCRATCH_FOR(PRG_EVENTS_MAX)

/* Return codes.  attach: EINVAL for bad arguments, report_first + report_count past u32 (checked before any
 * arithmetic), a ledger maximum above the reader's ceiling
 * (PRG_*_MAX) or no terminal LOSS credit in the ledger.  begin/end/mark: ESTOPPED (not admitted), EINVAL (end: not
 * the innermost zone, a token of another BEGIN or descriptor, or reused), EOVERFLOW (seq/ticket exhausted, the
 * suppression counter full, or a MARK at depth 64: refused unnumbered), EFAULT (the publication failed: the fault).
 * begin: EOVERFLOW also when PRG_SUPPRESS_MAX suppressed levels are open or the suppressed serial is exhausted.
 * calibrate: EBUSY (no slot, no bracket credit, or the FIFO refused), ETIMEDOUT / ECLOCK (CANCELLED; ECLOCK also for a
 * timebase that did not advance or would overlap the last bracket), EANOMALY (a label outside the pinned window or a
 * poisoned report: the cell is BAD and the drainer latches the anomaly), ESTOPPED (not admitted, or the finalization
 * cancelled the cell first), EFAULT.  detach: ETIMEDOUT (stays DETACHING), ESTOPPED (the seal won), ECLOCK, EFAULT. */
int prg_attach(prg_dom *d, uint32_t domain_id, uint32_t report_first, uint32_t report_count, uint32_t label,
               const prg_port *port, const prg_out *out, prg_ledger *ledger, prg_meta *meta, prg_stage *stage,
               uint8_t *scratch, uint32_t scratch_events);
int prg_begin(prg_dom *d, uint32_t desc, prg_token *tok);
int prg_end(prg_dom *d, const prg_token *tok, uint32_t desc);
int prg_mark(prg_dom *d, uint32_t desc);
int prg_frame(prg_dom *d, uint32_t owner, uint64_t frame_id, uint8_t owner_ref);
int prg_calibrate(prg_dom *d, uint64_t timeout_us);
void prg_drain(prg_dom *d);
void prg_seal(prg_dom *d);
/* R2: the same, for a caller that already HOLDS the extension mutex (the runtime's drainer); no port lock calls */
void prg_drain_held(prg_dom *d);
void prg_seal_held(prg_dom *d);
/* R2 (drainer_finalizes): ATTACHED -> DETACHING, then wait until the DRAINER (prg_drain_held) has finalized it
 * and the runtime has ACKNOWLEDGED that finalizing turn (detach_ack): the caller never encodes.  Three separate
 * facts: `retired` (the frozen terminal decision, set inside the finalization before FINAL is offered), final_written
 * (the sink accepted FINAL) and the acknowledgement (the whole terminal turn completed, its closing extension unlock
 * included).  OK only on the acknowledgement 1 (also when FINAL was refused: an output-free retirement); EFAULT on 2
 * or a domain fault; ETIMEDOUT while pending (stays DETACHING / DETACH_FINAL; call again; a repeated wait after OK is
 * OK again); ESTOPPED (the seal won); ECLOCK.  The core's own prg_detach (finalizing on the caller) stays for the
 * standalone host model. */
int prg_detach_wait(prg_dom *d, uint64_t timeout_us, void (*poll_hook)(void *), void *poll_ctx);
int prg_detach(prg_dom *d, uint64_t timeout_us, void (*poll_hook)(void *), void *poll_ctx);
int prg_release(prg_dom *d);

#endif
