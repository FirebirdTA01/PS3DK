/* overhead-bench driver (the orchestration of source/main.c, Plan A r2 3.1 and 3.1e; probe-design-r1 11.2 A-1/A-2):
 * portable C over a port.  The target binds the profiler, pthreads, POSIX files, the LV2/SPU and GCM bindings; the
 * host controls (host-tests/test_ob_life.c) bind mocks that fail any chosen call, and drive THIS code.
 *
 * Order: the schedule; the CSV opened as <base>.csv.tmp; profiler init; the drain thread's registration; the SPU
 * section open (startup acknowledgement checked); the GPU section open; the PPU workers, one per PPU case: mutex and
 * condition init, create, and a startup acknowledgement carrying the worker's registration result (a worker whose
 * registration failed exits at once and is joined).  Any startup failure skips the blocks.  Blocks: a checked drain
 * between blocks, the block, a checked row write; a failed SPU block (send, reply, identity) stops the blocks (the
 * row is written with its rc).  Then the GPU section to <base>.gpu.csv.tmp.
 * Teardown: every started worker is told to quit and joined (checked: a failed join retains it); the SPU and GPU
 * sections close by their own state machines.  ONLY when no worker, producer, lane or domain is retained: the final
 * drain, the drain thread's end and the profiler shutdown (both checked).  A retained resource leaves the profiler
 * running (its storage stays referenced) and fails the run.
 * Publication: every output is written to its .tmp with checked writes, fsync and close; renamed to its final name
 * only when the WHOLE run succeeded, else to .failed (kept apart).  The receipt <base>.bench-end.txt
 * ("BENCH_END rows=<rows written> invalid=<K> ledger_mismatches=<M> receipt=complete") is published last, through
 * its own tmp/rename, only after everything else succeeded. */
#ifndef OB_DRV_H
#define OB_DRV_H

#include <stdint.h>

#include "ob_gpu_run.h"
#include "ob_io.h"
#include "ob_sched.h"
#include "ob_spu_life.h"

#define OB_PPU_WORKERS 6u
#define OB_RC_NOWORKER 0x7FFF0010

typedef struct ob_drv_port {
    void *ctx;
    const ob_fio *io;
    const char *base; /* the output stem: <base>.csv, <base>.gpu.csv, <base>.bench-end.txt */
    uint64_t seed, tb_freq;
    unsigned r; /* OB_R in production */
    uint64_t (*tb)(void *ctx);
    int (*prof_init)(void *ctx);
    int (*thread_begin)(void *ctx, const char *name);
    void (*thread_end)(void *ctx);
    int (*drain)(void *ctx);
    int (*shutdown)(void *ctx);
    int (*mutex_init)(void *ctx, void *mutex); /* pthread_mutex_t * */
    int (*cond_init)(void *ctx, void *cond);   /* pthread_cond_t * */
    int (*thread_create)(void *ctx, void **handle, void *(*fn)(void *), void *arg);
    int (*thread_join)(void *ctx, void *handle);
    void (*run_ppu)(void *ctx, const ob_block *b, uint64_t *t0, uint64_t *t1);
    const ob_spu_port *spu;
    uint64_t reply_ea;
    uint32_t spu_polls;
    const ob_gpu_port *gpu;
    uint32_t gpu_report_count, gpu_polls;
    void (*log)(void *ctx, const char *what, int rc);
} ob_drv_port;

typedef struct ob_drv_result {
    int rc;                    /* 0: the whole run succeeded and the receipt was published */
    int spu_life, gpu_life;    /* OB_LIFE_* (-1: never opened) */
    int workers_retained, profiler_shut_down, receipt_published;
    unsigned long rows, planned, gpu_rows, invalid, mismatches;
} ob_drv_result;

void ob_drv_run(const ob_drv_port *p, ob_drv_result *out);

#endif
