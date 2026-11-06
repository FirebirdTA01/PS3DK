/* HOST-ONLY fakes for the E1 probe's PPU flow (E1_HOST_TEST; design r3 F + r4 section 4).  MODEL ONLY: nothing here
 * describes LV2, the SPU, the MFC, the RSX or the console's filesystem.
 *
 *  - The SPU program is the REAL e1_spu_core.c with the REAL writer (prof_spu_writer.c, PS3DK_PROF_SPU_HOST_TEST) and
 *    the REAL psp_spu core, run on a host thread started by the fake sysSpuThreadGroupStart.  Its port follows the I2
 *    host emulation of the line premises (psp_host_emul.h: line locks, the line-A version for putllc).
 *  - The report lines are C11 atomics through accessors (r4 section 4): the producer's payload stores relaxed, its
 *    commit store release; the PPU's commit loads acquire, its payload copy relaxed loads after the acquire, its
 *    recheck an acquire load.  This validates the MODEL's ordering only; the target uses volatile accesses and the W
 *    sync points (main.c), which a host run does not exercise.
 *  - The GPU is the REAL runtime + GCM port (gpu_port_gcm.c) over fake device memory and a fake command buffer whose
 *    6-word pairs a small executor decodes at flush on this thread (the R4a Mode S pattern).  NOP blocks are
 *    target-only (a no-op here).
 *  - The mailbox: a nonzero sysSpuThreadWriteMb return delivers NOTHING in this model (the target treats it as
 *    unknown; the probe does too).
 *
 * Faults: env E1_FAULTS, comma-separated key[=value]:
 *   mkdir.base=<hex errno>  mkdir.run=<hex errno|exist>
 *   setup.<spu-init|import|group-create|thread-init|group-start>=<hex>
 *   cleanup.<terminate|join|destroy|image-close>=<hex>
 *   mb=<err:<hex>|value:<hex>>         the GO write fails (nothing delivered) / delivers another value
 *   rep<k>=<stale|identity|torn|nocommit>   report k: commit never written (producer then blocks) / wrong magic /
 *                                       commit changed after the PPU's copy / payload written without commit
 *   gcm.<host-buffer|init>=<hex>   gpu.drop@<k>   (the k-th app flush executes nothing: its pairs never land)
 *   mk<NN>.<open|fsync|close>=<hex errno>   mk<NN>.write=<zero|<hex errno>>
 *   cap.<open|read>=<hex errno>
 * and in e1_wrap.c (every profiler API and writer call the probe makes): p.<api>[@k]=<hex>, w.<call>[@k]=<rc>, ...
 * Every fake call is appended to env E1_TRACE as one line. */
#ifndef E1_HOST_H
#define E1_HOST_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "gpu_port_gcm.h"
#include "prof_gpu.h"
#include "prof_spu.h"
#include "prof_test_hooks.h"
#include "psp_host_emul.h"
#include "psp_layout.h"

#include "../source/e1_report.h"
#include "e1_host_api.h"

/* ---- the fault script and the trace (e1_host_api.h declares; e1_wrap.c defines) ---- */
#define e1_f e1h_f
#define e1_fval e1h_fval
#define e1_trace e1h_trace

/* ---- clocks: a fake timebase for the records, the real monotonic clock for the deadlines ---- */
static uint64_t e1_tb_now = 0x100000;
#define E1_TB() (e1_tb_now += 1000u)
#define E1_SYNC() atomic_thread_fence(memory_order_seq_cst)
static void e1_clock_init(void) {}
static uint64_t e1_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
#define E1_POLL_PAUSE() sched_yield()
#define E1_SLEEP_US(us) usleep(us)
#define E1_BASE e1h_base()

/* ---- the report lines: atomics through accessors (r4 section 4) ---- */
static e1_line e1_payload_line, e1_commit_line;
static uint32_t e1_commit_ld(void) { return __atomic_load_n(&e1_commit_line.w[0], __ATOMIC_ACQUIRE); }
static uint32_t e1_payload_ld(unsigned i) { return __atomic_load_n(&e1_payload_line.w[i], __ATOMIC_RELAXED); }
static void e1_lines_zero(void) /* before the producer thread exists: its creation orders these */
{
    for (unsigned i = 0; i < 32u; i++) {
        __atomic_store_n(&e1_payload_line.w[i], 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&e1_commit_line.w[i], 0u, __ATOMIC_RELAXED);
    }
}
static uint64_t e1_report_eas(void) { return 0; } /* the host producer uses the lines directly */
/* the torn-commit step (e1_host.c): the PPU announces its copy, the producer changes the commit, the PPU rechecks */
#define E1_AFTER_COPY(s) e1h_after_copy(s)

/* the PPU's read of line A: under the line's emulation lock (the drainer's line-A store takes it too) */
static uint64_t e1_line_a_read_idx(uint64_t ea_ctl)
{
    uint64_t v;
    psp_emul_line_lock(ea_ctl);
    v = psp_rd64((const uint8_t *)(uintptr_t)ea_ctl + PSP_A_READ_IDX);
    psp_emul_line_unlock(ea_ctl);
    e1_trace("ppu-line-a read_idx %016llx", (unsigned long long)v);
    return v;
}

/* ---- LV2 SPU fakes ---- */
typedef uint32_t sys_addr_t;
typedef struct {
    int id;
} sysSpuImage;
typedef struct {
    const char *name;
    uint32_t nsize;
} sysSpuThreadGroupAttribute;
typedef struct {
    const char *name;
    uint32_t nsize;
} sysSpuThreadAttribute;
typedef struct {
    uint64_t arg0, arg1, arg2, arg3;
} sysSpuThreadArgument;
static const uint8_t e1_spu_bin[16] = {0x7f, 'E', 'L', 'F'}; /* the host never loads it */

static int e1h_setup(const char *k)
{
    uint32_t v;
    char key[48];
    snprintf(key, sizeof key, "setup.%s", k);
    int rc = e1_fval(key, &v) ? (int)v : 0;
    e1_trace("spu %s -> %08x", k, (unsigned)rc);
    return rc;
}
static int sysSpuInitialize(int a, int b) { (void)a, (void)b; return e1h_setup("spu-init"); }
static int sysSpuImageImport(sysSpuImage *i, const void *bin, int t) { (void)i, (void)bin, (void)t; return e1h_setup("import"); }
static int sysSpuThreadGroupCreate(uint32_t *g, int n, int p, sysSpuThreadGroupAttribute *a)
{
    (void)n, (void)p, (void)a;
    *g = 7;
    return e1h_setup("group-create");
}
static int sysSpuThreadInitialize(uint32_t *t, uint32_t g, int i, sysSpuImage *im, sysSpuThreadAttribute *a,
                                  sysSpuThreadArgument *args)
{
    (void)g, (void)i, (void)im, (void)a;
    *t = 9;
    e1h_spu_args(args->arg0, (uint32_t)(args->arg1 >> 32), args->arg2, args->arg3);
    return e1h_setup("thread-init");
}
static int sysSpuThreadGroupStart(uint32_t g)
{
    (void)g;
    int rc = e1h_setup("group-start");
    if (!rc)
        e1h_spu_start(&e1_payload_line, &e1_commit_line);
    return rc;
}
static int sysSpuThreadWriteMb(uint32_t thread, uint32_t v)
{
    (void)thread;
    return e1h_spu_write_mb(v);
}
static int e1h_cleanup(const char *k)
{
    uint32_t v;
    char key[48];
    snprintf(key, sizeof key, "cleanup.%s", k);
    int rc = e1_fval(key, &v) ? (int)v : 0;
    e1_trace("cleanup %s -> %08x", k, (unsigned)rc);
    return rc;
}
static int sysSpuThreadGroupTerminate(uint32_t g, int v)
{
    (void)g, (void)v;
    int rc = e1h_cleanup("terminate");
    if (!rc)
        e1h_spu_terminate();
    return rc;
}
static int sysSpuThreadGroupJoin(uint32_t g, uint32_t *c, uint32_t *s)
{
    (void)g;
    int rc = e1h_cleanup("join");
    if (!rc) {
        e1h_spu_join(); /* returns only when the producer thread ended (the probe never joins a running one) */
        *c = 1; /* the model's "all threads exited" */
        *s = 0;
    }
    return rc;
}
static int sysSpuThreadGroupDestroy(uint32_t g) { (void)g; return e1h_cleanup("destroy"); }
static int sysSpuImageClose(sysSpuImage *i) { (void)i; return e1h_cleanup("image-close"); }

/* the image: the REAL descriptor records of the host-built e1_spu_core.c, as a table (no SPU ELF on the host) */
#define E1_IMAGE_REGISTER(out) e1h_register_image(out)

/* ---- GCM fakes over the R4a Mode S device ---- */
#define E1_GCM_CTX() e1h_gcm_ctx()
#define E1_NOP_BLOCK(ctx) ((void)(ctx), e1_trace("nop-block (target only)"))
static int gcm_host_buffer_alloc(uint32_t size, sys_addr_t *h)
{
    uint32_t v;
    (void)size;
    *h = 0x10000000u;
    int rc = e1_fval("gcm.host-buffer", &v) ? (int)v : 0;
    e1_trace("gcm host-buffer -> %08x", (unsigned)rc);
    return rc;
}
static int cellGcmInit(uint32_t a, uint32_t b, void *h)
{
    uint32_t v;
    (void)a, (void)b, (void)h;
    int rc = e1_fval("gcm.init", &v) ? (int)v : 0;
    e1_trace("gcm init -> %08x", (unsigned)rc);
    return rc;
}
#define cellGcmFlush(ctx) e1h_gcm_flush(ctx)
#define cellGcmFinish(ctx, ref) e1h_gcm_finish((ctx), (ref))

/* ---- the file seams: markers, the run directory, the capture read-back ---- */
#define E1_OPEN(p, seq) e1h_mk_open((p), (seq))
#define E1_WRITE(fd, b, n, seq) e1h_mk_write((fd), (b), (n), (seq))
#define E1_FSYNC(fd, seq) e1h_mk_fsync((fd), (seq))
#define E1_CLOSE(fd, seq) e1h_mk_close((fd), (seq))
#define E1_MKDIR(p) e1h_mkdir(p)
#define E1_CAP_OPEN(p) e1h_cap_open(p)
#define E1_CAP_READ(fd, b, n) e1h_cap_read((fd), (b), (n))
#define E1_CAP_CLOSE(fd) close(fd)

#endif
