/* overhead-bench SPU section lifecycle (Plan A r2 3.1b/3.1e): portable C over a port; the target binds LV2 and the
 * profiler (source/spu_bind.c), the host controls a mock that fails any chosen step (host-tests/test_ob_life.c).
 *
 * Resources and their states (each recorded when its acquiring call SUCCEEDED, never assumed):
 *   image (imported), lane (created), group (created), thread (initialized), started (group start succeeded, OR
 *   a start failure that is not a proven non-start: the producer may run, so it is treated as started and settled by
 *   terminate + join before anything is released),
 *   alive (the SPU answered the last request in time with the right identity), exited (its fini reply arrived),
 *   joined (group join succeeded), uncertain (a started group whose exit was not proven and whose terminate failed,
 *   or whose join failed: the SPU may still run and DMA into the lane ring and the reply block).
 * Requests: one command word in the inbound mailbox, (seq << 16) | (case << 8) | tu; the reply carries it back
 * (identity), plus the reset status.  Every wait for a reply is outside the timed blocks and FINITE: at most
 * `polls` polls with one pause each; a missing or mismatched reply makes the SPU not alive and no later command is
 * sent.
 * Start classification (the adopted NOT_STARTED vs UNCERTAIN rule): group_start returns 0 (started),
 * OB_SPU_START_NOT_STARTED only when the binding has a documented proof that nothing started, or any other value:
 * UNCERTAIN -- never assumed to be free of side effects; the group counts as started-not-alive.  The LV2 binding
 * (source/spu_bind.c) has no such proof for any sysSpuThreadGroupStart error, so it never returns NOT_STARTED.
 * Close (ob_spu_close): started and alive -> exit command and the fini reply; if the exit is not proven -> terminate
 * (checked); terminate failed -> UNCERTAIN: nothing else is touched (no join, no lane_end, no destroy, no image
 * close).  Otherwise join (checked); a failed join -> UNCERTAIN likewise.  Only after a successful join (or when the
 * group never started): lane_end (a failure retains the lane), group destroy, image close (failures recorded).
 * Residual: the LV2 group join has no time bound; after a proven exit or a successful terminate it is expected to
 * return (an LV2 premise, not shown here); a join that never returns hangs the driver, it is not turned into a
 * timeout.
 * Result: OB_LIFE_CLEAN, OB_LIFE_FAILED (a failure, but no producer or lane is retained: the profiler may shut
 * down), OB_LIFE_RETAINED (a producer may still run or the lane is still charged: the caller must NOT shut the
 * profiler down or release storage the SPU references). */
#ifndef OB_SPU_LIFE_H
#define OB_SPU_LIFE_H

#include <stdint.h>

#include "ob_spu_res.h"

enum { OB_LIFE_CLEAN = 0, OB_LIFE_FAILED = 1, OB_LIFE_RETAINED = 2 };
#define OB_SPU_START_NOT_STARTED 0x7FFF0100 /* group_start: a documented, proven non-start (nothing runs) */
/* block rc values (written to the CSV rc column for a block that did not complete) */
#define OB_RC_SEND 0x7FFF0001     /* the command could not be sent */
#define OB_RC_NOREPLY 0x7FFF0002  /* no reply within the poll bound */
#define OB_RC_IDENTITY 0x7FFF0003 /* the reply carried another command */
#define OB_RC_DEAD 0x7FFF0004     /* the SPU section stopped earlier: not sent */

typedef struct ob_lane {
    uint32_t handle, ring_slots;
    uint64_t ea_ring, ea_ctl;
} ob_lane;

typedef struct ob_spu_port {
    void *ctx;
    int (*spu_init)(void *ctx);
    int (*image_import)(void *ctx);
    int (*image_register)(void *ctx, uint32_t *image); /* the profiler's register_elf */
    int (*lane_create)(void *ctx, uint32_t image, ob_lane *out);
    int (*group_create)(void *ctx);
    int (*thread_init)(void *ctx, const ob_lane *lane, uint64_t reply_ea);
    int (*group_start)(void *ctx);
    int (*write_mb)(void *ctx, uint32_t word);
    uint32_t (*reply_seq)(void *ctx);                 /* a volatile read of the reply block's seq */
    void (*reply_words)(void *ctx, uint32_t out[16]); /* after an lwsync: the words of that reply */
    void (*pause)(void *ctx);
    int (*terminate)(void *ctx);
    int (*join)(void *ctx);
    int (*lane_end)(void *ctx, uint32_t handle);
    int (*destroy)(void *ctx);
    int (*image_close)(void *ctx);
    uint64_t (*tb)(void *ctx);
    void (*log)(void *ctx, const char *what, int rc);
} ob_spu_port;

typedef struct ob_spu {
    const ob_spu_port *p;
    uint64_t reply_ea;
    uint32_t polls;
    int image, lane_owned, group, thread, started, alive, exited, joined, uncertain, start_uncertain;
    ob_lane lane;
    uint32_t expect, cmd_seq, dec_reset;
} ob_spu;

typedef struct ob_spu_reply {
    uint32_t w[8]; /* d0, d1, rc, sseq, tail_lost, staged, published, lost (spu/main.c) */
    uint32_t cmd, echo, dec_reset;
    uint64_t ppu_t0, ppu_t1;
    int rc; /* 0, or an OB_RC_* value */
} ob_spu_reply;

/* 0: open and the startup acknowledgement (attach rc 0, dec_write rc 0) received; else -1 (call close anyway) */
int ob_spu_open(ob_spu *s, const ob_spu_port *p, uint64_t reply_ea, uint32_t polls);
int ob_spu_block(ob_spu *s, uint32_t c, uint32_t tu, ob_spu_reply *out); /* 0 or -1 (out->rc says why) */
int ob_spu_close(ob_spu *s);                                              /* OB_LIFE_* */

#endif
