/* Named per-frame counters inside the runtime (format section 11).  Built only with PS3DK_PROF_WITH_COUNTERS.
 *
 * One capture-wide table under one mutex: the dense name table and a ring of 32-byte samples.  Producers append
 * under the mutex.  The drainer snapshots (names, head) under the mutex at the START of a drain, before the thread
 * table snapshot, so every sample below the snapshot was made by an incarnation that the same drain declares
 * (section 5.4 order).  It copies the samples into the chunk payload under the mutex, releases it, and writes the
 * chunk; the sink is never called under the mutex.  The final drain freezes the table (later calls are refused,
 * uncounted, as ESTOPPED) and writes the FINAL chunk before the TRAILER. */
#ifndef PS3DK_PROF_COUNTER_INTERNAL_H
#define PS3DK_PROF_COUNTER_INTERNAL_H
#include "../core/prof_internal.h"
#include <ps3dk/prof_counter.h>

#define CNT_NAMES PS3DK_PROF_COUNTER_NAMES_MAX
#define CNT_NAME_LEN PS3DK_PROF_COUNTER_NAME_MAX
#define CNT_RING PS3DK_PROF_COUNTER_BUFFER
#define CNT_SAMPLE 32u
#define CNT_CHUNK 39u
#define CNT_HEADER 32u
_Static_assert((CNT_RING & (CNT_RING - 1)) == 0, "the sample ring is a power of two");
_Static_assert(CNT_HEADER + CNT_NAMES * (6u + CNT_NAME_LEN) + CNT_RING * CNT_SAMPLE <= P_CHUNK_CAP, "one chunk holds a drain");

typedef struct counter_capture {
    pal_mutex mu;
    uint32_t names;               /* declared names (dense ids 0..names-1), under mu */
    uint32_t names_emitted;       /* drainer-private */
    uint64_t head, tail;          /* samples [tail, head) in the ring; head under mu, tail drainer-private + mu */
    uint64_t dropped, refused;    /* cumulative, under mu */
    uint32_t frozen;              /* under mu: the final drain took the frontier */
    uint32_t final_done;          /* drainer-private */
    uint32_t failed;              /* drainer-private: a drainer lock failed; nothing more is emitted */
    uint64_t snap_head;           /* drainer-private: this drain's snapshot */
    uint32_t snap_names;
    uint16_t len[CNT_NAMES];
    char name[CNT_NAMES][CNT_NAME_LEN];
    uint8_t ring[CNT_RING][CNT_SAMPLE];
} counter_capture;

#define p_counter_init ps3dk_prof__counter_init
#define p_counter_unwind ps3dk_prof__counter_unwind
#define p_counter_snapshot ps3dk_prof__counter_snapshot
#define p_counter_freeze ps3dk_prof__counter_freeze
#define p_counter_emit ps3dk_prof__counter_emit
#define p_counter_emit_chunk ps3dk_prof__counter_emit_chunk

int p_counter_init(prof *p);
void p_counter_unwind(prof *p);
/* The drainer's calls return 0, or -1 when the capture must fail: a failed counter-mutex lock (which also sets
 * lock_failed and stops all later emission) or, for emit, a sink failure.  The callers fail the sink on -1. */
/* the drain's snapshot of (names, head); at the START of an ordinary drain */
int p_counter_snapshot(prof *p);
/* the final frontier: snapshot and freeze, once; without it there is no FINAL */
int p_counter_freeze(prof *p);
/* writes one COUNTERS chunk for the snapshot when it holds anything new (always when final; FINAL only frozen) */
int p_counter_emit(prof *p, int final);
/* framing, in prof_drain.c */
int p_counter_emit_chunk(prof *p, const uint8_t *payload, size_t len);
#endif
