/* E1 SPU -> PPU report lines (design p3.3 E1 r4 delta sections 1-2; codex 05:30 carry 1).  Shared by the PPU, the
 * SPU core and the host tests.  Two 128-byte lines in PPU memory written ONLY by the SPU's DMA: PAYLOAD, then (after
 * a tag wait-all) COMMIT.  The PPU trusts a report only after: commit == s, sync, a private copy of the whole
 * payload, sync, commit re-read == s, and e1_report_check() on the private copy.  Same hardware ordering assumptions
 * as the W probe; not a coherence proof.
 *
 * A trusted report that carries an error is still a FAILED experiment: it never authorizes the next phase. */
#ifndef E1_REPORT_H
#define E1_REPORT_H

#include <stdint.h>

#define E1_MAGIC 0x45585431u /* 'EXT1' */
#define E1_GO 0x474f2121u    /* the inbound GO command ('GO!!'); the SPU refuses any other value */
#define E1_REPORT_TAG 5u     /* the report DMA tag (the writer owns tag 3) */
#define E1_WRITER_TAG 3u
#define E1_EVENTS_PER_EPOCH 132u /* 64 BEGIN + 64 END + 4 MARK */

enum { E1_KIND_EPOCH1 = 1, E1_KIND_FINAL = 2 };
enum { E1_CALL_NONE = 0, E1_CALL_ATTACH = 1, E1_CALL_FLUSH = 2, E1_CALL_DEC = 3, E1_CALL_FINI = 4, E1_CALL_EVENT = 5,
       E1_CALL_GO = 6 /* the inbound command was not E1_GO */ };
enum { E1_DEC_NOT_ATTEMPTED = 0, E1_DEC_OK = 1, E1_DEC_FAILED = 2 };
enum { E1_RIDX_UNAVAILABLE = 0, E1_RIDX_VALID = 1 };

/* PAYLOAD word indices (u32, big-endian on the target; native on the host) */
enum {
    E1_W_MAGIC = 0, E1_W_KIND, E1_W_SEQ, E1_W_FAIL_CALL, E1_W_FAIL_RC, E1_W_EV1, E1_W_EV2, E1_W_DEC_STATE, E1_W_DEC_RC,
    E1_W_RIDX_STATE, E1_W_RIDX_HI, E1_W_RIDX_LO, E1_W_DESC, E1_W_COUNT
};

typedef struct e1_line {
    uint32_t w[32];
} __attribute__((aligned(128))) e1_line;
_Static_assert(sizeof(e1_line) == 128, "one cache line");

/* The descriptor records the SPU image holds: the target image has exactly 3 (16-byte records: probe-e1, probe-e2,
 * probe-mark).  The HOST build overrides it (its 32-byte record shim gives 6 16-byte slots, 3 of them fillers). */
#ifndef E1_DESC_COUNT
#define E1_DESC_COUNT 3u
#endif
#define E1_FIRST_FLUSH 16u /* the core flushes once >= 16 events are pending: no flush (and no flush or event error)
                            * before 16 events of an epoch */

/* The writer's documented nonzero results (prof_spu_lane.h), as the int32 bit patterns the payload carries */
#define E1__EBUSY 0xFFFFFFFEu
#define E1__EINVAL 0xFFFFFFFBu
#define E1__ECLOSED 0xFFFFFFF1u
#define E1__ENOTSUP 0xFFFFFFEFu
#define E1__EAGAIN 0xFFFFFFECu
#define E1__EFAULT 0xFFFFFFEBu
#define E1__EEXHAUSTED 0xFFFFFFEAu

/* 1 when rc is a result the failing call can return (the core never records 0; GO's refusal is -1) */
static inline int e1_rc_allowed(uint32_t call, uint32_t rc)
{
    switch (call) {
    case E1_CALL_ATTACH: /* EINVAL before any write; EBUSY (events enabled); ECLOSED; EFAULT */
        return rc == E1__EINVAL || rc == E1__EBUSY || rc == E1__ECLOSED || rc == E1__EFAULT;
    case E1_CALL_FLUSH:
    case E1_CALL_FINI:
        return rc == E1__EBUSY || rc == E1__EINVAL || rc == E1__ECLOSED || rc == E1__ENOTSUP || rc == E1__EAGAIN ||
               rc == E1__EFAULT || rc == E1__EEXHAUSTED;
    case E1_CALL_DEC: /* never EEXHAUSTED (a flush result) */
        return rc == E1__EBUSY || rc == E1__EINVAL || rc == E1__ECLOSED || rc == E1__ENOTSUP || rc == E1__EAGAIN ||
               rc == E1__EFAULT;
    case E1_CALL_EVENT: /* a latched descriptor-index fault: EFAULT only */
        return rc == E1__EFAULT;
    case E1_CALL_GO: /* the core's own refusal of a wrong inbound command */
        return rc == 0xFFFFFFFFu;
    default:
        return 0;
    }
}

/* The PRODUCER-POSSIBLE predicate of a private payload copy for the expected sequence (1 or 2): 0 = one of the states
 * e1_spu_core.c can send; otherwise the first rule broken (the identity diagnostic).  The allowed states:
 *
 *  kind 1 (before GO; calls attach / flush / event only)
 *    none    ev1 132, desc E1_DESC_COUNT
 *    attach  ev1 0, desc 0 (nothing was attached, no event issued)
 *    flush   16 <= ev1 <= 132, desc E1_DESC_COUNT (no flush before 16 events)
 *    event   16 <= ev1 <= 132, desc E1_DESC_COUNT (reported at the next flush)
 *    and always: ev2 0, dec not-attempted with rc 0, ridx unavailable with zero words
 *  kind 2 (after a clean report 1 and GO: ev1 132, desc E1_DESC_COUNT; never attach)
 *    go      ev2 0, dec not-attempted, ridx unavailable (refused before any phase-2 action)
 *    flush   (a) the first post-GO flush: ev2 0, dec not-attempted, ridx unavailable
 *            (b) the flush after a dec EAGAIN: ev2 0, dec not-attempted, ridx valid
 *            (c) an epoch-2 flush: 16 <= ev2 <= 132, dec ok, ridx valid
 *    dec     ev2 0, dec failed with dec_rc == fail_rc, ridx valid
 *    event   16 <= ev2 <= 132, dec ok, ridx valid
 *    fini    ev2 132, dec ok, ridx valid
 *    none    ev2 132, dec ok, ridx valid
 *  dec_rc is 0 unless dec failed; unavailable ridx words are 0 (a valid ridx may be any value, 0 included);
 *  fail_rc is in the failing call's result set (e1_rc_allowed); the line tail is canonical zero. */
static inline int e1_report_check(const uint32_t *w, uint32_t seq)
{
    uint32_t kind = w[E1_W_KIND], call = w[E1_W_FAIL_CALL], rc = w[E1_W_FAIL_RC], dec = w[E1_W_DEC_STATE];
    uint32_t ridx = w[E1_W_RIDX_STATE], ev1 = w[E1_W_EV1], ev2 = w[E1_W_EV2], desc = w[E1_W_DESC];
    if (w[E1_W_MAGIC] != E1_MAGIC)
        return 1;
    if (w[E1_W_SEQ] != seq || kind != seq || (seq != 1u && seq != 2u))
        return 2;
    if (call > E1_CALL_GO || dec > E1_DEC_FAILED || ridx > E1_RIDX_VALID)
        return 3;
    if (call == E1_CALL_NONE ? rc != 0u : !e1_rc_allowed(call, rc))
        return 4; /* no error: rc 0; an error: a result that call can return */
    for (unsigned i = E1_W_COUNT; i < 32u; i++)
        if (w[i])
            return 6;
    if (dec != E1_DEC_FAILED && w[E1_W_DEC_RC])
        return 11;
    if (ridx == E1_RIDX_UNAVAILABLE && (w[E1_W_RIDX_HI] || w[E1_W_RIDX_LO]))
        return 13;
    if (kind == E1_KIND_EPOCH1) {
        if (ev2 || dec != E1_DEC_NOT_ATTEMPTED || ridx != E1_RIDX_UNAVAILABLE)
            return 7; /* nothing of phase 2 before GO */
        switch (call) {
        case E1_CALL_NONE:
            return ev1 == E1_EVENTS_PER_EPOCH && desc == E1_DESC_COUNT ? 0 : 8;
        case E1_CALL_ATTACH:
            return ev1 == 0u && desc == 0u ? 0 : 17;
        case E1_CALL_FLUSH:
        case E1_CALL_EVENT:
            return ev1 >= E1_FIRST_FLUSH && ev1 <= E1_EVENTS_PER_EPOCH && desc == E1_DESC_COUNT ? 0 : 18;
        default:
            return 19; /* dec / fini / go are post-GO calls */
        }
    }
    if (call == E1_CALL_ATTACH)
        return 9;
    if (ev1 != E1_EVENTS_PER_EPOCH || desc != E1_DESC_COUNT)
        return 10; /* report 2 exists only after a clean epoch 1 and GO */
    switch (call) {
    case E1_CALL_GO:
        return ev2 == 0u && dec == E1_DEC_NOT_ATTEMPTED && ridx == E1_RIDX_UNAVAILABLE ? 0 : 15;
    case E1_CALL_FLUSH:
        if (ev2 == 0u && dec == E1_DEC_NOT_ATTEMPTED)
            return 0; /* (a) ridx unavailable, or (b) ridx valid */
        return ev2 >= E1_FIRST_FLUSH && ev2 <= E1_EVENTS_PER_EPOCH && dec == E1_DEC_OK && ridx == E1_RIDX_VALID ? 0
                                                                                                              : 20;
    case E1_CALL_DEC:
        return ev2 == 0u && dec == E1_DEC_FAILED && w[E1_W_DEC_RC] == rc && ridx == E1_RIDX_VALID ? 0 : 12;
    case E1_CALL_EVENT:
        return ev2 >= E1_FIRST_FLUSH && ev2 <= E1_EVENTS_PER_EPOCH && dec == E1_DEC_OK && ridx == E1_RIDX_VALID ? 0
                                                                                                              : 21;
    case E1_CALL_FINI:
    case E1_CALL_NONE:
        return ev2 == E1_EVENTS_PER_EPOCH && dec == E1_DEC_OK && ridx == E1_RIDX_VALID ? 0 : 14;
    default:
        return 3;
    }
}

#endif
