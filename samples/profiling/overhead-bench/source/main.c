/* overhead-bench target driver (Plan A r2 3.1 with probe-design-r1 11.2 A-1..A-4 and r2 D7): the portable driver
 * (common/ob_drv.c: order, lifecycle, publication rules) bound to the profiler, pthreads, POSIX files and the
 * SPU/GCM bindings (spu_bind.c, rsx_bind.c).  A1: SOURCE ONLY, built for the compile-only audit; no image is
 * linked or launched before the A2 gates.
 * Outputs (/dev_hdd0/tmp/overhead-bench-<tb>.*): .csv and .gpu.csv (or .csv.failed / .gpu.csv.failed), and the
 * receipt .bench-end.txt last, only when the whole run succeeded (common/ob_drv.h). */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/sys_time.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "../common/ob_cases.h"
#include "../common/ob_drv.h"
#include "../common/ob_timer.h"
#include "ob_bind.h"

typedef struct ob_t2 {
    uint64_t t0, t1;
} ob_t2;
#define DECL(tu)                                                                                                     \
    void bench_##tu##_hot(uint32_t, ob_t2 *);                                                                          \
    void bench_##tu##_nest8(uint32_t, ob_t2 *);                                                                        \
    void bench_##tu##_drop(uint32_t, ob_t2 *);                                                                         \
    void bench_##tu##_prefill(uint32_t);                                                                               \
    void bench_##tu##_frame(uint32_t, ob_t2 *);                                                                        \
    void bench_##tu##_empty(uint32_t, ob_t2 *);                                                                        \
    extern void (*const bench_##tu##_cold[64])(ob_t2 *);
DECL(on)
DECL(off)
DECL(off_b)

static void run_ppu(void *c, const ob_block *b, uint64_t *t0, uint64_t *t1)
{
    (void)c;
    ob_t2 t = {0, 0};
    uint32_t n = ob_case_table[b->c].n;
#define PICK(fn) (b->tu == OB_TU_ON ? bench_on_##fn : b->tu == OB_TU_OFF ? bench_off_##fn : bench_off_b_##fn)
    switch (b->c) {
    case OB_P_HOT: PICK(hot)(n, &t); break;
    case OB_P_NEST8: PICK(nest8)(n, &t); break;
    case OB_P_DROP: PICK(prefill)(ob_case_table[OB_P_PREFILL].n); PICK(drop)(n, &t); break;
    case OB_P_FRAME: PICK(frame)(n, &t); break;
    case OB_P_EMPTY: PICK(empty)(n, &t); break;
    case OB_P_COLD: PICK(cold)[b->rep % 64u](&t); break;
    }
#undef PICK
    *t0 = t.t0, *t1 = t.t1;
}

/* ---- files ---- */
static int f_open(void *c, const char *p)
{
    (void)c;
    return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
}
static long f_write(void *c, int fd, const void *b, unsigned long n)
{
    (void)c;
    return (long)write(fd, b, n);
}
static int f_fsync(void *c, int fd)
{
    (void)c;
    return fsync(fd);
}
static int f_close(void *c, int fd)
{
    (void)c;
    return close(fd);
}
static int f_rename(void *c, const char *a, const char *b)
{
    (void)c;
    return rename(a, b);
}
static const ob_fio g_io = {NULL, f_open, f_write, f_fsync, f_close, f_rename};

/* ---- profiler and threads ---- */
static uint64_t d_tb(void *c)
{
    (void)c;
    return ob_tb();
}
static int d_init(void *c)
{
    (void)c;
    return ob_prof_init();
}
static int d_begin(void *c, const char *name)
{
    (void)c;
    return ps3dk_prof_thread_begin(name);
}
static void d_end(void *c)
{
    (void)c;
    ps3dk_prof_thread_end();
}
static int d_drain(void *c)
{
    (void)c;
    return ps3dk_prof_drain();
}
static int d_shutdown(void *c)
{
    (void)c;
    return ps3dk_prof_shutdown();
}
static int d_mutex(void *c, void *m)
{
    (void)c;
    return pthread_mutex_init((pthread_mutex_t *)m, NULL);
}
static int d_cond(void *c, void *v)
{
    (void)c;
    return pthread_cond_init((pthread_cond_t *)v, NULL);
}
static pthread_t g_th[OB_PPU_WORKERS];
static unsigned g_nth;
static int d_create(void *c, void **h, void *(*fn)(void *), void *arg)
{
    (void)c;
    if (g_nth >= OB_PPU_WORKERS)
        return -1;
    int rc = pthread_create(&g_th[g_nth], NULL, fn, arg);
    if (rc == 0)
        *h = &g_th[g_nth++];
    return rc;
}
static int d_join(void *c, void *h)
{
    (void)c;
    return pthread_join(*(pthread_t *)h, NULL);
}
static void d_log(void *c, const char *what, int rc)
{
    (void)c;
    printf("OB %s rc=%d\n", what, rc);
}

int main(void)
{
    char base[96];
    uint64_t seed = ob_tb();
    snprintf(base, sizeof base, "/dev_hdd0/tmp/overhead-bench-%016llx", (unsigned long long)seed);
    ob_drv_port p = {NULL,     &g_io,   base,      seed,     sys_time_get_timebase_frequency(), OB_R,
                     d_tb,     d_init,  d_begin,   d_end,    d_drain,
                     d_shutdown, d_mutex, d_cond,  d_create, d_join,
                     run_ppu,  ob_spu_port_lv2(), ob_spu_reply_ea(), OB_SPU_POLLS,
                     ob_gpu_port_gcm(), OB_GPU_REPORT_COUNT, OB_LABEL_POLLS, d_log};
    ob_drv_result r;
    ob_drv_run(&p, &r);
    printf("OB_END rc=%d rows=%lu planned=%lu gpu_rows=%lu spu_life=%d gpu_life=%d workers_retained=%d shut_down=%d "
           "receipt=%d\n",
           r.rc, r.rows, r.planned, r.gpu_rows, r.spu_life, r.gpu_life, r.workers_retained, r.profiler_shut_down,
           r.receipt_published);
    return r.rc != 0;
}
