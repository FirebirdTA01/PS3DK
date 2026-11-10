/* FIFO wrap phase events (docs/design/ps3dk-prof-fifo.md): the one instance of a capture.
 *
 * Ownership and ordering:
 *  - immutable fields are written by attach on the owner before the state CAS ATTACHING -> ATTACHED (acq_rel); the
 *    drainer reads them only after an acquire load of `state` showed ATTACHED or later;
 *  - the ring is single producer (the owner's GCM wrap callback), single consumer (the drain thread): the owner fills a
 *    slot and RELEASES write_idx; the drainer ACQUIRES write_idx, encodes the slots, and RELEASES read_idx only after
 *    the sink accepted the whole FIFO_EVENTS chunk; the owner ACQUIRES read_idx before it reuses a slot;
 *  - the owner counters (attempted ... high_water, saturated) have one writer, the owner, which stores each with
 *    RELEASE; the drainer reads them with ACQUIRE loads, in the order that keeps attempted >= written + dropped and
 *    completed <= started even while the owner runs (fifo_writer.c);
 *  - o_* fields are owner-private, d_* fields drainer-private (drains are serialized by the core's drain_busy CAS);
 *  - the owner path takes no lock and calls no SDK function; the FIFO drain step takes no lock either.  Progress while
 *    the owner is parked in a wrap also relies on the owner holding no profiler mutex across a GCM reserve, which holds
 *    for every reserve this runtime makes (docs/design/ps3dk-prof-fifo.md, Runtime).
 * Lifetime: the drainer frees the block only after a DETACHED instance's FINAL (the owner removed the observer first,
 * on its own thread); a SEALED instance (the owner never acknowledged) keeps its block for the process lifetime. */
#ifndef PS3DK_PROF_FIFO_INTERNAL_H
#define PS3DK_PROF_FIFO_INTERNAL_H
#include "../core/prof_internal.h"
#include <ps3dk/prof_fifo.h>
#include "ps3tc_fifo_observe.h"

#ifndef PS3DK_PROF_WITH_SPU
#error "the FIFO wrap observer needs the domain extension (PS3DK_PROF_WITH_SPU)"
#endif

enum { C_FIFO_CONFIG = 31, C_FIFO_EVENTS = 32, C_FIFO_STATUS = 33 };
enum { FS_NONE = 0, FS_ATTACHING = 1, FS_ATTACHED = 2, FS_DETACHED = 3, FS_CLOSED = 4, FS_SEALED = 5 };
enum { FE_TOKEN = 1, FE_CLOCK_INVALID = 2 };
enum { FST_FINAL = 1, FST_LOWER_BOUND = 2, FST_SATURATED = 4, FST_OWNER_UNACK = 8 };

#define FIFO_REC_BYTES   96u
#define FIFO_CONTROL     4096u
#define FIFO_RING_MIN    16u
#define FIFO_RING_MAX    65536u
#define FIFO_RING_DEF    1024u
#define FIFO_MAX_DEF     1048576u
#define FIFO_MAX_MIN     (FIFO_RING_MIN * FIFO_REC_BYTES + FIFO_CONTROL)
#define FIFO_MAX_MAX     16777216u
#define FIFO_EVENTS_MAX  ((P_CHUNK_CAP - 16u) / FIFO_REC_BYTES)

/* One ring slot: the native record, encoded little-endian by the drainer. */
typedef struct fifo_rec {
    uint64_t seq, wrap_id, tb_before, tb_after, frame_id;
    uint32_t frame_owner, begin_off, tail_off, target_off, requested_words, current_off;
    uint16_t kind, flags;
    uint8_t pad[FIFO_REC_BYTES - 5 * 8 - 6 * 4 - 2 * 2];
} fifo_rec;
_Static_assert(sizeof(fifo_rec) == FIFO_REC_BYTES, "one ring slot is one accounted 96-byte event");

typedef struct fifo_capture {
    /* immutable after publication */
    uint32_t instance, generation, domain, owner_inc, owner_slot, owner_gen;
    uint32_t ring_records, max_bytes, allocated, begin_off, end_off, pad0;
    void *ctx;
    uint8_t *block;
    fifo_rec *ring;
    pal_owner owner;
    ps3tc_fifo_observer reg;
    /* atomics */
    uint32_t state;
    uint32_t saturated;                /* owner RELEASE, sticky */
    uint64_t write_idx;                /* owner RELEASE */
    uint8_t pad1[112];
    uint64_t read_idx;                 /* drainer RELEASE, after the sink accepted the chunk */
    uint8_t pad2[120];
    uint64_t attempted, dropped, refused, started, completed, open_wrap, high_water; /* owner RELEASE */
    /* owner-private */
    uint64_t o_wrap, o_last_tb, o_tok_frame;
    uint32_t o_tok, o_in_cb, o_clock_seen, o_pad;
    /* drainer-private */
    uint32_t d_config, d_closed, d_owner_seen, d_statuses;
    uint64_t d_read, d_written, d_last_seq, d_attempted, d_refused, d_dropped;
} fifo_capture;

#define p_fifo_drain ps3dk_prof__fifo_drain
#define p_fifo_emit_chunk ps3dk_prof__fifo_emit_chunk
#define p_fifo_payload ps3dk_prof__fifo_payload
#define p_fifo_seal_claim ps3dk_prof__fifo_seal_claim
#define p_fifo_seal_close ps3dk_prof__fifo_seal_close
#define g_fifo ps3dk_prof__g_fifo
extern fifo_capture g_fifo;
void p_fifo_drain(prof *p, int seal);  /* the drainer: an ordinary drain (0) or the seal (1) */
void p_fifo_seal_claim(void);          /* the seal, first: NONE / ATTACHING -> SEALED (the admission linearization) */
void p_fifo_seal_close(void);          /* the seal, last: an undrained ATTACHED -> SEALED */
int p_fifo_emit_chunk(prof *p, uint32_t type, const uint8_t *payload, size_t len); /* 0: the sink took it whole */
uint8_t *p_fifo_payload(prof *p);      /* the drainer's chunk payload buffer (P_CHUNK_CAP bytes) */

#ifdef PS3DK_PROF_TEST_HOOKS
/* point 1: inside the owner callback, before admission (kind in `v`); point 2: attach, every validation passed, before
 * the instance claim; point 3: attach, claimed and installed, before the publication */
extern void (*ps3dk_prof__fifo_test_hook)(unsigned point, unsigned v);
void ps3dk_prof__fifo_test_seed(uint64_t attempted); /* the owner's attempt counter (the ceiling controls) */
uint64_t ps3dk_prof__fifo_test_read_idx(void);
uint32_t ps3dk_prof__fifo_test_state(void);
const void *ps3dk_prof__fifo_test_block(void); /* the attach allocation, NULL when none is live */
#endif
#endif
