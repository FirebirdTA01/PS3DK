/* Function-path host harness: drives the real core + func runtime (not instrumented) against the instrumented
 * workload in func_work.c, with a fake deterministic timebase, and writes the capture for func-test.py to decode.
 * Usage: func_test <mode> <capture> */
#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include "func_internal.h"
#include <ps3dk/prof_func.h>
#ifdef PS3DK_PROF_WITH_SPU
#include "prof_spu.h"
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void tree_a(void); void rec(int); void deep(int); void deep_jump(int, int); void ov_root(void);
void lj_outer(void); void ra(int); void post_reset(void); void sp_outer(void); void w1(void);
void r_outer(void); void s_spin(void); void u_fn(void); void v_fn(void); void re_fn(void);
void reg_boot(void); void rp(void); void leaf(void);
void tree_b(void); void tree_c(void); void ov_f1(void); void ov_f2(void); void ov_f3(void); void ov_f4(void);
void ov_f5(void); void ov_g(void); void lj_mid(void); void lj_deep(void); void lj_after(void); void sp_inner(void);
void sp_leaf(void); void r_leaf(void); void reg_inner(void); void rq(void);
void deep_inl(int); void inl_nest(void); void inl_mix(void); void real_leaf(void); void k1(void); void boot3(void);
void *inline_addr(int);

static uint64_t g_now = 1000;
static int real_clock;
static uint64_t fake_clock(void) { return __atomic_load_n(&g_now, __ATOMIC_ACQUIRE); }
void advance(uint64_t n) { __atomic_fetch_add(&g_now, n, __ATOMIC_ACQ_REL); }
static void set_now(uint64_t v) { __atomic_store_n(&g_now, v, __ATOMIC_RELEASE); }

static unsigned checks, failed;
#define CHECK(x) do { __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED); if (!(x)) { \
    __atomic_fetch_add(&failed, 1, __ATOMIC_RELAXED); fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)

static unsigned char *cap;
static size_t used, capsz;
static long put(void *ctx, const void *p, unsigned long n)
{
    (void)ctx;
    if (used + n > capsz) {
        size_t nz = capsz ? capsz * 2 : 1 << 20;
        while (nz < used + n)
            nz *= 2;
        unsigned char *q = realloc(cap, nz);
        if (!q)
            return -1;
        cap = q;
        capsz = nz;
    }
    memcpy(cap + used, p, n);
    used += n;
    return (long)n;
}
static int close_sink(void *ctx) { (void)ctx; return 0; }

static const char *mode;
static void start(uint32_t actors, uint32_t nodes, uint32_t depth, int auto_reg, uint32_t quiesce_us)
{
    ps3dk_prof_config cfg;
    ps3dk_prof_func_config fc = { sizeof fc, 0, actors, nodes, depth, 0 };
    ps3dk_prof_sink_callbacks sink = { put, NULL, close_sink, NULL };
    if (!real_clock)
        ps3dk_prof_test_hooks.clock = fake_clock;
    ps3dk_prof_config_defaults(&cfg);
    cfg.app = "func";
    cfg.max_threads = 4;
    cfg.max_descriptors = 8;
    cfg.ring_records = 512;
    cfg.quiesce_timeout_us = quiesce_us;
    cfg.auto_register = auto_reg;
    CHECK(ps3dk_prof_func_configure(&fc) == 0);
    CHECK(ps3dk_prof_set_sink_callbacks(&sink) == 0);
#ifdef PS3DK_PROF_WITH_SPU
    {
        /* the extension runtime with SPU lanes enabled (file flags bits 0 and 1 beside bit 5) */
        ps3dk_prof_ext_config x;
        memset(&x, 0, sizeof x);
        x.size = sizeof x;
        x.spu_lanes_max = 1;
        CHECK(ps3dk_prof_init_ex(&cfg, &x) == 0);
    }
#else
    CHECK(ps3dk_prof_init(&cfg) == 0);
#endif
}
static int finish(const char *path, int want_rc)
{
    int rc = ps3dk_prof_shutdown();
    FILE *f;
    CHECK(rc == want_rc);
    f = fopen(path, "wb");
    CHECK(f && fwrite(cap, 1, used, f) == used && fclose(f) == 0);
    printf("checks=%u failed=%u bytes=%zu rc=%d\n", checks, failed, used, rc);
    return failed ? 1 : 0;
}

/* The registration-inside-an-entered-function control: called from reg_boot, which was entered before init. */
void work_note(int what)
{
    if (what == 1) {
        start(2, 64, 16, 0, 0);
        CHECK(ps3dk_prof_thread_begin("main") == 0);
    }
}

/* ---- thread controls -------------------------------------------------------------------------------------------- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int step, paused, released, stop_flag;
static void wait_for(int *v, int want)
{
    pthread_mutex_lock(&mu);
    while (*v < want)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}
static void set_to(int *v, int x)
{
    pthread_mutex_lock(&mu);
    *v = x;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}
static int pause_once;
static void pause_hook(unsigned point)
{
    if (point == 1 && __atomic_exchange_n(&pause_once, 0, __ATOMIC_ACQ_REL)) {
        set_to(&paused, 1);
        wait_for(&released, 1);
    }
}
static void *lag_worker(void *arg)
{
    (void)arg;
    CHECK(ps3dk_prof_thread_begin("lagw") == 0);
    set_to(&step, 1);
    wait_for(&step, 2);
    w1();                 /* its entry takes the epoch-2 cut, paused before the publication */
    set_to(&step, 3);
    wait_for(&step, 4);
    w1();                 /* its entry takes the epoch-4 cut, carrying the deferred count */
    set_to(&step, 5);
    wait_for(&step, 6);
    ps3dk_prof_thread_end();
    set_to(&step, 7);
    return NULL;
}
static int run_threads(const char *path)
{
    pthread_t t;
    start(2, 64, 16, 0, 0);
    set_now(1000);
    CHECK(pthread_create(&t, NULL, lag_worker, NULL) == 0);
    wait_for(&step, 1);
    set_now(2000);
    CHECK(ps3dk_prof_drain() == 0);   /* D1: no request yet; epoch 1 */
    set_now(3000);
    CHECK(ps3dk_prof_drain() == 0);   /* D2: the worker never reached a hook: LAGGING for epoch 1; epoch 2 */
    ps3dk_prof__func_test_hook = pause_hook;
    pause_once = 1;
    set_now(4000);                    /* the worker's own cut tick */
    set_to(&step, 2);
    wait_for(&paused, 1);
    set_now(5000);
    CHECK(ps3dk_prof_drain() == 0);   /* D3: cut taken but unpublished: LAGGING for epoch 2; epoch 3 */
    set_to(&released, 1);
    wait_for(&step, 3);               /* published epoch 2; its exit saw epoch 3 with no ack: one deferred cut */
    set_now(6000);
    CHECK(ps3dk_prof_drain() == 0);   /* D4: the stale epoch-2 cut, LAGGING; ack; epoch 4 */
    set_now(7000);
    set_to(&step, 4);
    wait_for(&step, 5);               /* cut for epoch 4 at 7000, with deferred = 1 */
    set_now(8000);
    CHECK(ps3dk_prof_drain() == 0);   /* D5 */
    set_now(8500);                    /* the worker's final tick */
    set_to(&step, 6);
    wait_for(&step, 7);
    set_now(9000);
    CHECK(ps3dk_prof_drain() == 0);   /* D6: FINAL */
    CHECK(pthread_join(t, NULL) == 0);
    ps3dk_prof__func_test_hook = NULL;
    return finish(path, PS3DK_PROF_OK);
}

static void *retire_worker(void *arg)
{
    (void)arg;
    CHECK(ps3dk_prof_thread_begin("retire") == 0);
    r_outer();                        /* ends the incarnation inside two instrumented activations */
    return NULL;
}
static void *spin_worker(void *arg)
{
    (void)arg;
    CHECK(ps3dk_prof_thread_begin("spin") == 0);
    set_to(&step, 1);
    while (!__atomic_load_n(&stop_flag, __ATOMIC_ACQUIRE))
        s_spin();                     /* acknowledges STOP from a hook */
    return NULL;
}
static void *idle_worker(void *arg)
{
    (void)arg;
    CHECK(ps3dk_prof_thread_begin("idle") == 0);
    set_to(&paused, 1);
    wait_for(&released, 1);           /* never reaches a hook before the seal: NOT_QUIESCED */
    return NULL;
}
static int run_stop(const char *path)
{
    pthread_t r, s, i;
    int rc;
    start(4, 64, 16, 0, 100000);
    CHECK(pthread_create(&r, NULL, retire_worker, NULL) == 0);
    CHECK(pthread_join(r, NULL) == 0);
    CHECK(pthread_create(&s, NULL, spin_worker, NULL) == 0);
    CHECK(pthread_create(&i, NULL, idle_worker, NULL) == 0);
    wait_for(&step, 1);
    wait_for(&paused, 1);
    CHECK(ps3dk_prof_drain() == 0);
    rc = finish(path, PS3DK_PROF_ENOTQUIESCED);
    set_to(&released, 1);
    __atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(s, NULL) == 0 && pthread_join(i, NULL) == 0);
    return rc | (failed ? 1 : 0);
}

static void *unreg_worker(void *arg)
{
    (void)arg;
    for (int k = 0; k < 5; k++)
        u_fn();                       /* 5 unregistered entries */
    return NULL;
}
static void *noactor_worker(void *arg)
{
    (void)arg;
    CHECK(ps3dk_prof_thread_begin("noactor") == 0);   /* slot 1: no actor storage with max_actors 1 */
    for (int k = 0; k < 3; k++)
        v_fn();
    ps3dk_prof_thread_end();
    return NULL;
}
static void *auto_worker(void *arg)
{
    (void)arg;
    v_fn();                           /* auto_register on the first entry */
    v_fn();
    ps3dk_prof_thread_end();
    return NULL;
}
static int run_unreg(const char *path, int autoreg)
{
    pthread_t t;
    start(1, 64, 16, autoreg, 0);
    if (autoreg) {
        CHECK(pthread_create(&t, NULL, auto_worker, NULL) == 0);
        CHECK(pthread_join(t, NULL) == 0);
    } else {
        CHECK(ps3dk_prof_thread_begin("main") == 0);
        CHECK(pthread_create(&t, NULL, unreg_worker, NULL) == 0);
        CHECK(pthread_join(t, NULL) == 0);
        CHECK(pthread_create(&t, NULL, noactor_worker, NULL) == 0);
        CHECK(pthread_join(t, NULL) == 0);
    }
    CHECK(ps3dk_prof_drain() == 0);
    return finish(path, PS3DK_PROF_OK);
}

static void reentry_hook(unsigned point)
{
    if (point == 1)
        re_fn();                      /* an instrumented call from inside a hook */
}
static void reentry_sat_hook(unsigned point)
{
    if (point == 1) {
        ps3dk_prof__func_test_seed_counter(FC_REENTRY, UINT64_MAX - 1); /* after the cut zeroed the interval */
        re_fn();
        re_fn();                      /* 4 refused hook calls onto UINT64_MAX - 1: saturates, never wraps */
    }
}
static int run_sat(const char *path)
{
    pthread_t t;
    start(1, 64, 16, 0, 0);
    ps3dk_prof__func_test_seed(UINT64_MAX - 2, UINT64_MAX - 1);
    CHECK(ps3dk_prof_thread_begin("main") == 0);
    CHECK(pthread_create(&t, NULL, unreg_worker, NULL) == 0);   /* 5 entries: +2 to the ceiling */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(pthread_create(&t, NULL, noactor_worker, NULL) == 0); /* 3 entries: +1 to the ceiling */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(ps3dk_prof_drain() == 0);
    CHECK(pthread_create(&t, NULL, unreg_worker, NULL) == 0);   /* saturated: the deltas stay 0, flagged */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(ps3dk_prof_drain() == 0);
    ps3dk_prof__func_test_hook = reentry_sat_hook;
    leaf();
    ps3dk_prof__func_test_hook = NULL;
    leaf();
    CHECK(ps3dk_prof_drain() == 0);
    return finish(path, PS3DK_PROF_OK);
}

/* ---- race: a running owner, a concurrent drainer ---------------------------------------------------------------- */
#define RACE_N 200000
static void *race_owner(void *arg)
{
    (void)arg;
    CHECK(ps3dk_prof_thread_begin("owner") == 0);
    set_to(&step, 1);
    for (int k = 0; k < RACE_N; k++)
        rp();
    ps3dk_prof_thread_end();
    __atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);
    return NULL;
}
static int run_race(const char *path)
{
    pthread_t t;
    struct timespec ts = { 0, 50000 };
    int drains = 0;
    real_clock = 1;
    start(2, 64, 16, 0, 0);
    CHECK(pthread_create(&t, NULL, race_owner, NULL) == 0);
    wait_for(&step, 1);
    while (!__atomic_load_n(&stop_flag, __ATOMIC_ACQUIRE)) {
        CHECK(ps3dk_prof_drain() == 0);
        drains++;
        nanosleep(&ts, NULL);
    }
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(ps3dk_prof_drain() == 0);
    printf("drains=%d\n", drains);
    return finish(path, PS3DK_PROF_OK);
}

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}
__attribute__((noinline, noipa)) static void plain_leaf(void) { }
static int run_cost(const char *path)
{
    const int n = 2000000;
    double t0, t1, t2;
    real_clock = 1;
    start(1, 64, 16, 0, 0);
    CHECK(ps3dk_prof_thread_begin("main") == 0);
    for (int k = 0; k < 1000; k++)
        leaf();
    t0 = now_ns();
    for (int k = 0; k < n; k++)
        plain_leaf();
    t1 = now_ns();
    for (int k = 0; k < n; k++)
        leaf();
    t2 = now_ns();
    printf("cost uninstrumented_ns_per_call=%.2f instrumented_ns_per_call=%.2f hook_pair_ns=%.2f\n",
           (t1 - t0) / n, (t2 - t1) / n, ((t2 - t1) - (t1 - t0)) / n);
    return finish(path, PS3DK_PROF_OK);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    mode = argv[1];
#define SYM(f) printf("sym %s %llu\n", #f, (unsigned long long)(uintptr_t)(void *)f)
    SYM(tree_a); SYM(tree_b); SYM(tree_c); SYM(rec); SYM(deep); SYM(deep_jump); SYM(ov_root); SYM(ov_f1);
    SYM(ov_f2); SYM(ov_f3); SYM(ov_f4); SYM(ov_f5); SYM(ov_g); SYM(lj_outer); SYM(lj_mid); SYM(lj_deep);
    SYM(lj_after); SYM(ra); SYM(post_reset); SYM(sp_outer); SYM(sp_inner); SYM(sp_leaf); SYM(w1); SYM(r_outer);
    SYM(r_leaf); SYM(s_spin); SYM(u_fn); SYM(v_fn); SYM(re_fn); SYM(reg_boot); SYM(reg_inner); SYM(rp); SYM(rq);
    SYM(leaf); SYM(deep_inl); SYM(inl_nest); SYM(inl_mix); SYM(real_leaf); SYM(k1); SYM(boot3);
    {
        static const char *const inl[] = {"tb_inl", "di_leaf", "in_inner", "in_outer", "im_a", "im_b"};
        for (int k = 0; k < 6; k++)
            printf("sym %s %llu\n", inl[k], (unsigned long long)(uintptr_t)inline_addr(k));
    }
    fflush(stdout);
    if (!strcmp(mode, "threads"))
        return run_threads(argv[2]);
    if (!strcmp(mode, "stop"))
        return run_stop(argv[2]);
    if (!strcmp(mode, "unreg"))
        return run_unreg(argv[2], 0);
    if (!strcmp(mode, "auto"))
        return run_unreg(argv[2], 1);
    if (!strcmp(mode, "race"))
        return run_race(argv[2]);
    if (!strcmp(mode, "cost"))
        return run_cost(argv[2]);
    if (!strcmp(mode, "sat"))
        return run_sat(argv[2]);
    if (!strcmp(mode, "reset")) {
        boot3();                      /* entered before init; k1's longjmp lands in it; its exit matches nothing */
        post_reset();
        return finish(argv[2], PS3DK_PROF_OK);
    }
    if (!strcmp(mode, "reg")) {
        reg_boot();                   /* entered before init; registers inside; its exit is an orphan */
        return finish(argv[2], PS3DK_PROF_OK);
    }
    if (!strcmp(mode, "depth"))
        start(1, 64, 8, 0, 0);
    else if (!strcmp(mode, "overflow"))
        start(1, 4, 16, 0, 0);
    else
        start(1, 64, 16, 0, 0);
    CHECK(ps3dk_prof_thread_begin("main") == 0);
    if (!strcmp(mode, "tree1")) {
        tree_a(); rec(3); tree_a();
    } else if (!strcmp(mode, "tree3")) {
        tree_a(); CHECK(ps3dk_prof_drain() == 0);
        rec(3); CHECK(ps3dk_prof_drain() == 0);
        tree_a(); CHECK(ps3dk_prof_drain() == 0);
    } else if (!strcmp(mode, "depth")) {
        deep(12);
        deep_jump(12, 12);
        deep_inl(12);
    } else if (!strcmp(mode, "overflow")) {
        ov_root();
    } else if (!strcmp(mode, "inline")) {
        inl_nest();
        inl_mix();
    } else if (!strcmp(mode, "longjmp")) {
        lj_outer();
        ra(3);
        post_reset();
    } else if (!strcmp(mode, "span")) {
        CHECK(ps3dk_prof_drain() == 0);   /* epoch 1 requested before the span */
        sp_outer();
    } else if (!strcmp(mode, "reentry")) {
        CHECK(ps3dk_prof_drain() == 0);
        ps3dk_prof__func_test_hook = reentry_hook;
        leaf();                           /* its entry takes the cut; the hook calls re_fn inside the cut */
        ps3dk_prof__func_test_hook = NULL;
        leaf();
    } else {
        return 2;
    }
    return finish(argv[2], PS3DK_PROF_OK);
}
