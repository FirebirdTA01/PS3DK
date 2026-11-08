/* overhead-bench GPU section (Plan A r2 3.1c, 3.1e, the 4.2 G rows): open, the seeded G blocks, close.  Portable C
 * over a port: the target binds GCM and the profiler (source/rsx_bind.c); the host controls bind the REAL runtime
 * and the REAL GCM port over fake device memory (host-tests/test_ob_gpu_rt.c) or a mock that fails any chosen step
 * (host-tests/test_ob_life.c).
 *
 * Capacity (the reviewed A1 finding): the domain retains one report slot per submitted endpoint until a drain reads
 * it, and the bench drains only between blocks.  The largest block is G-g1: 64 zones = 128 endpoints, no drain
 * inside.  A calibrate bracket is submitted only in the G-cal blocks, which hold no zone.  So the domain needs at
 * least OB_GPU_BLOCK_SLOTS report slots; it gets OB_GPU_REPORT_COUNT (twice that).  The host control shows the
 * complete block with every endpoint an EVENT at this size, and the exhaustion negative at OB_GPU_BLOCK_SLOTS - 1.
 *
 * Block discipline: before every block the GPU is idle (the previous block's bench label reached its fresh value)
 * and the drain thread has drained.  A label that does not reach its value within the poll bound stops the section
 * (the GPU is not known idle): that row is written with OB_GE_LABEL, END carries the rows actually written and the
 * run fails.  Row rc flags: OB_GE_WRAP (the command buffer wrapped inside the variant: the byte count is not the
 * variant's; the row is excluded, not a failure), OB_GE_ZONE / OB_GE_CAL (a profiler call refused: the run fails),
 * OB_GE_LABEL (above).
 * Close: label wait with a fresh value -> detach (with its wait) -> release only after both succeeded; any failure
 * retains the domain (OB_LIFE_RETAINED: the profiler must not shut down). */
#ifndef OB_GPU_RUN_H
#define OB_GPU_RUN_H

#include <stdint.h>

#include "ob_io.h"
#include "ob_rsx.h"
#include "ob_sched.h"
#include "ob_spu_life.h" /* OB_LIFE_* */

#define OB_GPU_REPORT_FIRST 32u
#define OB_GPU_BLOCK_SLOTS (2u * OB_DRAWS) /* G-g1: 128 endpoints submitted without a drain */
#define OB_GPU_REPORT_COUNT 256u
#define OB_GPU_LABEL 241u
#define OB_BENCH_REPORT 2u
#define OB_BENCH_LABEL 243u
#define OB_GPU_REPORT_AREA 2048u /* GCM local report indices 0..2047 */
_Static_assert(OB_GPU_REPORT_COUNT >= OB_GPU_BLOCK_SLOTS, "the domain holds the complete G-g1 block");
_Static_assert(OB_GPU_REPORT_FIRST + OB_GPU_REPORT_COUNT <= OB_GPU_REPORT_AREA, "the domain fits the report area");
_Static_assert(OB_BENCH_REPORT + 2u <= OB_GPU_REPORT_FIRST, "the bench reports lie below the domain");

#define OB_GE_LABEL 1u
#define OB_GE_WRAP 2u
#define OB_GE_ZONE 4u
#define OB_GE_CAL 8u

typedef struct ob_gpu_port {
    void *ctx;
    int (*gcm_init)(void *ctx);
    void (*label_store)(void *ctx, uint32_t value); /* a CPU store of the bench label (before any command) */
    uint32_t (*label_read)(void *ctx);
    void (*pause)(void *ctx);
    int (*attach)(void *ctx, uint32_t first, uint32_t count, uint32_t label, uint32_t *gpu);
    int (*calibrate)(void *ctx, uint32_t gpu);
    int (*detach)(void *ctx, uint32_t gpu);
    int (*release)(void *ctx, uint32_t gpu);
    int (*drain)(void *ctx);
    uint64_t (*tb)(void *ctx);
    uint32_t *(*cmd_current)(void *ctx);
    uint64_t (*report_time)(void *ctx, uint32_t index);
    void (*log)(void *ctx, const char *what, int rc);
    const ob_rsx_port *rsx; /* report, label, draw, zone_begin/end (the profiler's calls on gpu), nop, flush */
} ob_gpu_port;

typedef struct ob_gpu {
    const ob_gpu_port *p;
    uint32_t gpu, label_value, polls, report_count;
    int opened, attached;
    unsigned long rows;
} ob_gpu;

/* report_count: OB_GPU_REPORT_COUNT in production (a control may pass another).  0 or -1. */
int ob_gpu_open(ob_gpu *g, const ob_gpu_port *p, uint32_t report_count, uint32_t polls);
/* writes the G CSV through w (header, one row per executed block, END with the rows written); 0 only when every
 * planned block ran with no failure flag and every write succeeded */
int ob_gpu_run(ob_gpu *g, uint64_t seed, unsigned r, ob_fw *w);
int ob_gpu_close(ob_gpu *g); /* OB_LIFE_* */

#endif
