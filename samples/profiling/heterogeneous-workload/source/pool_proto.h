#ifndef HETEROGENEOUS_POOL_PROTO_H
#define HETEROGENEOUS_POOL_PROTO_H
/* The SAMPLE_PROF_EXT pool protocol (PPU side), production code with no SDK header: spu_pool.c binds pool_io to
 * the LV2 SPU syscalls and the work slots; host-tests/test_prof_ext.c binds it to mocks.  Every completion and the
 * final marker are read through an acquire barrier before the slot's status, and every outcome feeds the epoch
 * rule (prof_ext.h). */
#include <stdint.h>
#include "prof_ext.h"

typedef struct pool_io {
    void *ctx;
    int (*write_mb)(void *ctx, unsigned worker, uint32_t word);
    uint32_t (*slot_sequence)(void *ctx, unsigned worker); /* a volatile read of the completion word */
    uint32_t (*slot_status)(void *ctx, unsigned worker);   /* read only after barrier() */
    void (*barrier)(void *ctx);                            /* sync: order the completion before the status */
    uint64_t (*now_us)(void *ctx);
    void (*sleep_us)(void *ctx, unsigned us);
    int (*join)(void *ctx);           /* join a group whose workers exited on their own */
    int (*close_terminate)(void *ctx); /* terminate (if still running) + join + destroy, as today */
    void (*log)(void *ctx, const char *what, int value);
} pool_io;

/* The EXT thread arguments of every worker, into out[0..workers): slot_ea[i] is worker i's work slot, lane[i] its
 * lane (NULL: none, the worker runs unprofiled).  A lane whose words do not pack (P-EA32 failed: an EA above 4 GiB,
 * or a misaligned one) is NEVER silent: it is logged ("pack-refused", worker), that worker gets the unprofiled words
 * (ring EA 0), eligibility is cleared, and the return is 1: the caller must report SPU profiling off for the run.
 * -1: a work slot EA itself does not pack (nothing the SPU program could use; the pool must not start).  0: every
 * worker got its lane's words or, without a lane, the unprofiled ones. */
int pp_worker_args(pe_epochs *e, const pool_io *io, const uint64_t *slot_ea, const pe_lane *const *lane,
                   unsigned workers, pe_args *out);
/* One job (work) word per worker.  A failed mailbox write (partial send) invalidates eligibility. */
int pp_submit(pe_epochs *e, const pool_io *io, unsigned workers, uint32_t sequence);
/* After the caller saw every worker complete `sequence`: barrier, then each status into the epoch rule. */
void pp_done(pe_epochs *e, const pool_io *io, unsigned workers);
/* A job that failed (the caller's deadline): invalidates eligibility. */
void pp_failed(pe_epochs *e, const char *why);
/* Reset-only words to the workers whose reset is due, then a bounded wait for exactly those. */
int pp_keepalive(pe_epochs *e, const pool_io *io, unsigned workers, uint32_t sequence);
/* Exit words, the final markers (bounded), their final statuses (after a barrier), then join (all markers) or
 * terminate-close.  *join_ok only when every marker arrived and the join succeeded; otherwise eligibility is
 * cleared and the lanes must be retained. */
int pp_close(pe_epochs *e, const pool_io *io, unsigned workers, int running, int *join_ok);

#endif
