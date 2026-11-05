#ifndef HETEROGENEOUS_WORKER_PROTO_H
#define HETEROGENEOUS_WORKER_PROTO_H
/* The SAMPLE_PROF_EXT SPU worker loop, production code with no SDK header: spu/main.c binds worker_io to the
 * mailbox, the lane writer and MFC; host-tests/test_prof_ext.c binds it to mocks.  Words: 0 exits (after fini, then
 * the final marker PE_SEQ_MASK with the final status), PE_FLAG_RESET resets the decrementer before the job,
 * PE_FLAG_RESET_ONLY resets without work.  A writer failure turns profiling off for this worker and is reported in
 * every later status word (PE_SPU_* bits). */
#include <stdint.h>
#include "prof_ext.h"

#define WP_OK 0
#define WP_EAGAIN (-20) /* PS3DK_PROF_SPU_EAGAIN (checked in the SPU binding) */
#define WP_RESET_TRIES 4

typedef struct worker_io {
    void *ctx;
    uint32_t (*read_mbox)(void *ctx);
    int (*attach)(void *ctx);
    int (*flush)(void *ctx);
    int (*dec_write)(void *ctx);
    int (*fini)(void *ctx);
    uint32_t (*zone_begin)(void *ctx);
    void (*zone_end)(void *ctx, uint32_t zone);
    void (*work)(void *ctx);   /* the job: computes into the slot (digest, count, begin/end decrementer) */
    void (*no_work)(void *ctx); /* a reset-only word: digest and count 0 */
    void (*publish)(void *ctx, uint32_t status, uint32_t sequence); /* payload, then the completion word */
} worker_io;

/* profiled: the thread arguments carried a ring (a lane exists).  Returns the final status word. */
uint32_t worker_run(const worker_io *io, int profiled);

#endif
