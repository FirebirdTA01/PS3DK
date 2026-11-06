/* overhead-bench host ledger model, PPU cases (Plan A r2 4.2 A5; probe-design-r1 11.2 A-2/A-3; r2 D7): the bench's
 * own case functions (source/bench_ppu.inc, TU "on" with PS3DK_PROF=1 and TU "off" with PS3DK_PROF=0) run against
 * the REAL portable runtime core and the host PAL (built with PS3DK_PROF_TEST_HOOKS), in APP mode, ring_records
 * 4096, each case on its own registered thread, the main thread draining only between blocks, in the seeded order of
 * common/ob_sched.c.  Every block's ledger (sequence numbers assigned, dropped, registrations, maximum depth) is read
 * through the host-only test hooks and checked exactly against common/ob_cases.c.  Model evidence: it shows each case
 * reaches its intended path; it measures nothing.
 * usage: test_ob_ppu SINK_DIR CSV_OUT   (CSV_OUT: the blocks in the bench CSV format, counted ledgers) */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "prof_test_hooks.h"
#include <ps3dk/prof.h>

#include "../common/ob_cases.h"
#include "../common/ob_csv.h"
#include <fcntl.h>
#include <unistd.h>
#include "../common/ob_sched.h"

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
uint64_t g_max_depth;
uint64_t (*ob_host_tb)(void);
static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static unsigned long g_regs;
static void on_reg(void) { ++g_regs; }

static unsigned tests, failures;
static void check(int yes, const char *name)
{
    ++tests;
    if (!yes) {
        ++failures;
        printf("FAIL %s\n", name);
    }
}

/* one worker thread per case: runs the jobs main posts, one at a time */
typedef struct worker {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int c, has_job, done, quit, tu, kind, site, registered, pre_ok;
    ob_ledger got;
    ob_t2 t;
} worker;
static void read_ctr(uint64_t *seq, uint64_t *drop)
{
    *seq = ps3dk_prof_test_read(PS3DK_PROF_TEST_NEXT_SEQ);
    *drop = ps3dk_prof_test_read(PS3DK_PROF_TEST_DROPPED);
}
static void run_job(worker *w)
{
    const ob_case *k = &ob_case_table[w->c];
    int on = w->tu == OB_TU_ON;
    uint64_t s0, d0, s1, d1;
    w->pre_ok = 1;
    if (w->c == OB_P_DROP) { /* the untimed precondition: fill the ring to its capacity (its own ledger checked) */
        read_ctr(&s0, &d0);
        (on ? bench_on_prefill : bench_off_prefill)(ob_case_table[OB_P_PREFILL].n);
        read_ctr(&s1, &d1);
        ob_ledger pre;
        memset(&pre, 0, sizeof pre);
        pre.seq = s1 - s0, pre.dropped = d1 - d0, pre.depth = on ? 1 : 0;
        char why[160];
        w->pre_ok = on ? ob_ledger_check(OB_P_PREFILL, &pre, why, sizeof why) == 0 : pre.seq == 0;
        if (!w->pre_ok)
            printf("  prefill: %s\n", why);
    }
    read_ctr(&s0, &d0);
    unsigned long r0 = g_regs;
    g_max_depth = 0;
    switch (w->c) {
    case OB_P_HOT: (on ? bench_on_hot : bench_off_hot)(k->n, &w->t); break;
    case OB_P_NEST8: (on ? bench_on_nest8 : bench_off_nest8)(k->n, &w->t); break;
    case OB_P_DROP: (on ? bench_on_drop : bench_off_drop)(k->n, &w->t); break;
    case OB_P_FRAME: (on ? bench_on_frame : bench_off_frame)(k->n, &w->t); break;
    case OB_P_EMPTY: (on ? bench_on_empty : bench_off_empty)(k->n, &w->t); break;
    case OB_P_COLD: (on ? bench_on_cold : bench_off_cold)[w->site](&w->t); break;
    }
    read_ctr(&s1, &d1);
    memset(&w->got, 0, sizeof w->got);
    w->got.seq = s1 - s0;
    w->got.dropped = d1 - d0;
    w->got.regs = g_regs - r0;
    w->got.depth = g_max_depth;
}
static void *worker_main(void *arg)
{
    worker *w = arg;
    char name[32];
    snprintf(name, sizeof name, "ob-case-%d", w->c);
    w->registered = ps3dk_prof_thread_begin(name) == PS3DK_PROF_OK;
    pthread_mutex_lock(&w->mu);
    for (;;) {
        while (!w->has_job && !w->quit)
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->quit)
            break;
        pthread_mutex_unlock(&w->mu);
        run_job(w);
        pthread_mutex_lock(&w->mu);
        w->has_job = 0;
        w->done = 1;
        pthread_cond_broadcast(&w->cv);
    }
    pthread_mutex_unlock(&w->mu);
    ps3dk_prof_thread_end();
    return NULL;
}
static void post(worker *w, int tu, int kind, int site)
{
    pthread_mutex_lock(&w->mu);
    w->tu = tu, w->kind = kind, w->site = site, w->done = 0, w->has_job = 1;
    pthread_cond_broadcast(&w->cv);
    while (!w->done)
        pthread_cond_wait(&w->cv, &w->mu);
    pthread_mutex_unlock(&w->mu);
}

static int f_open(void *c, const char *p) { (void)c; return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
static long f_write(void *c, int fd, const void *b, unsigned long n) { (void)c; return (long)write(fd, b, n); }
static int f_fsync(void *c, int fd) { (void)c; return fsync(fd); }
static int f_close(void *c, int fd) { (void)c; return close(fd); }
static int f_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b); }

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    static const ob_fio io = {NULL, f_open, f_write, f_fsync, f_close, f_rename};
    ob_fw csvw, *csv = &csvw;
    if (ob_fw_open(csv, &io, argv[2]) != 0)
        return 3;
    ob_host_tb = host_ns;
    ps3dk_prof_test_hooks.between_desc_entry_and_publish = on_reg;
    ps3dk_prof_config cfg;
    ps3dk_prof_config_defaults(&cfg);
    cfg.mode = PS3DK_PROF_MODE_APP;
    cfg.ring_records = OB_RING_RECORDS;
    cfg.max_threads = 16;
    cfg.app = "obhost";
    check(ps3dk_prof_set_sink_path(argv[1]) == PS3DK_PROF_OK, "sink path");
    check(ps3dk_prof_init(&cfg) == PS3DK_PROF_OK, "init (APP mode, ring 4096)");
    const uint16_t cases[] = {OB_P_HOT, OB_P_NEST8, OB_P_COLD, OB_P_DROP, OB_P_FRAME, OB_P_EMPTY};
    const int cold[] = {0, 0, 1, 0, 0, 0};
    static ob_block sch[8192];
    long nb = ob_schedule(20261106u, cases, cold, 6, OB_R, sch, 8192);
    check(nb == 5 * 3 * (OB_R + 1) + 3 * 64, "schedule size: 5 cases x 3 TUs x (1 + R) + P-cold 3 x 64");
    static worker w[OB_CASES];
    for (unsigned i = 0; i < 6; ++i) {
        worker *x = &w[cases[i]];
        memset(x, 0, sizeof *x);
        x->c = cases[i];
        pthread_mutex_init(&x->mu, NULL);
        pthread_cond_init(&x->cv, NULL);
        pthread_create(&x->th, NULL, worker_main, x);
    }
    unsigned long bad[OB_CASES][OB_TUS], seen[OB_CASES][OB_TUS], invalid = 0, cold_regs = 0, prefill_bad = 0, warm_ok = 0;
    memset(bad, 0, sizeof bad);
    memset(seen, 0, sizeof seen);
    check(ob_csv_begin(csv, 20261106u, sch, (unsigned long)nb, 1000000000u) == 0, "csv header");
    char why[160], first_why[OB_CASES][160];
    memset(first_why, 0, sizeof first_why);
    for (long i = 0; i < nb; ++i) {
        const ob_block *b = &sch[i];
        worker *x = &w[b->c];
        check(ps3dk_prof_drain() == PS3DK_PROF_OK, "drain between blocks (ring at 0 records before each block)");
        post(x, b->tu, b->kind, b->rep);
        if (x->t.t1 < x->t.t0)
            ++invalid;
        ob_row row;
        memset(&row, 0, sizeof row);
        row.t0 = x->t.t0, row.t1 = x->t.t1, row.led = x->got, row.counted = 1;
        check(ob_csv_row(csv, (unsigned long)i, b, &row) == 0, "csv row written");
        ++seen[b->c][b->tu];
        int ok;
        if (!x->pre_ok)
            ++prefill_bad;
        if (b->tu == OB_TU_ON && b->kind == OB_BLK_WARM) {
            /* the warmup block registers the case's descriptors (1 site; P-nest8 8; P-frame / P-empty none) */
            ob_ledger g = x->got;
            uint64_t want_regs = b->c == OB_P_NEST8 ? 8 : (b->c == OB_P_HOT || b->c == OB_P_DROP) ? 1 : 0;
            ok = g.regs == want_regs;
            g.regs = 0;
            ok = ok && ob_ledger_check(b->c, &g, why, sizeof why) == 0;
            ++warm_ok;
        } else if (b->tu == OB_TU_ON) {
            ok = ob_ledger_check(b->c, &x->got, why, sizeof why) == 0;
            if (b->c == OB_P_COLD)
                cold_regs += x->got.regs;
        } else { /* OFF / OFF-b: the iteration count only; nothing reaches the runtime */
            ok = x->got.seq == 0 && x->got.dropped == 0 && x->got.regs == 0;
            snprintf(why, sizeof why, "%s OFF: seq %llu", ob_case_table[b->c].name, (unsigned long long)x->got.seq);
        }
        if (!ok) {
            if (!bad[b->c][b->tu])
                snprintf(first_why[b->c], sizeof first_why[b->c], "%s", why);
            ++bad[b->c][b->tu];
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        int c = cases[i];
        printf("  ledger %-10s ON %lu blocks %lu mismatches | OFF %lu blocks %lu mismatches%s%s\n", ob_case_table[c].name,
               seen[c][OB_TU_ON], bad[c][OB_TU_ON], seen[c][OB_TU_OFF] + seen[c][OB_TU_OFFB],
               bad[c][OB_TU_OFF] + bad[c][OB_TU_OFFB], first_why[c][0] ? " first: " : "", first_why[c]);
        check(bad[c][OB_TU_ON] == 0, "ON ledger exact for every block");
        check(bad[c][OB_TU_OFF] + bad[c][OB_TU_OFFB] == 0, "OFF ledger: nothing produced");
    }
    check(cold_regs == 64, "P-cold: 64 registrations over its 64 fresh sites (each executed once)");
    printf("  ledger P-prefill  before every P-drop block: %lu mismatches\n", prefill_bad);
    check(prefill_bad == 0, "P-prefill fills exactly the ring capacity with nothing dropped, before every P-drop block");
    check(invalid == 0, "host boundary readings never decrease");
    for (unsigned i = 0; i < 6; ++i) {
        worker *x = &w[cases[i]];
        pthread_mutex_lock(&x->mu);
        x->quit = 1;
        pthread_cond_broadcast(&x->cv);
        pthread_mutex_unlock(&x->mu);
        pthread_join(x->th, NULL);
        check(x->registered, "each case on its own registered thread");
    }
    check(ps3dk_prof_drain() == PS3DK_PROF_OK && ps3dk_prof_shutdown() == PS3DK_PROF_OK, "final drain and shutdown");
    check(ob_csv_end(csv, (unsigned long)nb) == 0 && ob_fw_finish(csv) == 0, "csv written");
    printf("test_ob_ppu: %u checks, %u failures\n", tests, failures);
    return failures ? 1 : 0;
}
