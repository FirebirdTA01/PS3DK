/* FIFO wrap phase events on the host (docs/design/ps3dk-prof-fifo.md): the real core, extension and FIFO runtime in
 * THREAD mode (the background drain thread), a GCM host model that runs the real protocol and bridge headers with the
 * observed variant's callback order, and a callback sink.  fifo-test.py builds it, runs every mode and decodes the
 * captures.  Usage: fifo_test <mode> <capture>.  Exit 0 when every CHECK held; harness waits are bounded iteration
 * counts and report HARNESS-TIMEOUT. */
#define _POSIX_C_SOURCE 200809L
#define PS3TC_FIFO_OBSERVE 1 /* the model runs the observed protocol, as libgcm_cmd_prof_fifo.a does */
#include "prof_internal.h"
#include "fifo_internal.h"
#include "gpu_internal.h"
#include "prof_spu.h"
#include <ps3dk/prof_fifo.h>
#include <ps3dk/prof_gpu.h>
#include "ps3tc_fifo_wrap_protocol.h"
#include "ps3tc_fifo_observe.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_uint checks, failed;
#define CHECK(x) do { atomic_fetch_add(&checks, 1); if (!(x)) { atomic_fetch_add(&failed, 1); \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)

static void sleep_us(long us)
{
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&t, NULL);
}
/* a harness wait: a bounded number of 1 ms polls, never a product timeout */
#define WAIT_FOR(cond, polls) do { int k_; for (k_ = 0; k_ < (polls) && !(cond); k_++) sleep_us(1000); \
    if (!(cond)) { fprintf(stderr, "HARNESS-TIMEOUT %d: %s\n", __LINE__, #cond); atomic_fetch_add(&failed, 1); } } while (0)

/* ---- the GCM host model ---------------------------------------------------------------------------------------- */
#define WORDS 1024u
#define IOBASE 0x100000u
struct _gcmCtxData {
    uint32_t *begin, *end, *current;
    uint32_t callback;
};
static uint32_t RING[WORDS];
static struct _gcmCtxData CTX = { RING + 4, RING + WORDS - 1, RING + 4, 0 };
static struct _gcmCtxData OTHER;
static volatile uint32_t PUT = IOBASE + 16, GET = IOBASE + 16;
static atomic_int g_hold;           /* 1: GET never reaches tail; 2: GET never reaches begin, until cleared */
static atomic_int g_parked;         /* the owner is inside a held pause */
static atomic_int g_bridge_refuse;
static ps3tc_fifo_obs_state OBS;
static uint64_t stat_count, stat_ticks;

static uint32_t off_of(const void *a) { return IOBASE + (uint32_t)((const uint8_t *)a - (const uint8_t *)RING); }

int ps3tc_fifo_observe_install(struct _gcmCtxData *ctx, const ps3tc_fifo_observer *o, uint32_t *b, uint32_t *e)
{
    if (!ctx || !ctx->begin || !ctx->end || atomic_load(&g_bridge_refuse))
        return -1;
    return ps3tc_fifo_obs_install(&OBS, ctx, o, off_of(ctx->begin), off_of(ctx->end),
                                  (uint64_t)((uint8_t *)ctx->end - (uint8_t *)ctx->begin), b, e);
}
int ps3tc_fifo_observe_remove(struct _gcmCtxData *ctx, const ps3tc_fifo_observer *o) { return ps3tc_fifo_obs_remove(&OBS, ctx, o); }
uint64_t ps3tc_fifo_observe_suppressed(void) { return OBS.suppressed; }

static atomic_uint fence_word; /* a seq_cst RMW: a full barrier TSan models (it does not model fences) */
static void m_barrier(void *a) { (void)a; (void)atomic_fetch_add(&fence_word, 0u); }
static uint64_t m_now(void *a) { (void)a; return 0; }
static uint64_t m_tb(void *a) { (void)a; return ps3dk_prof__tb(); }
static void m_pause(void *a, uint32_t want, uint32_t spins, unsigned phase, uint64_t el)
{
    (void)a, (void)spins, (void)el;
    if ((int)phase == atomic_load(&g_hold)) {
        atomic_store(&g_parked, (int)phase);
        sleep_us(50);
        return;
    }
    atomic_store(&g_parked, 0);
    GET = want; /* an instant consumer: the whole published frontier is fetched */
}
static ps3tc_fifo_obs_wrap g_w;
static void m_phase(void *a, unsigned kind, uint64_t b, uint64_t e) { (void)a; ps3tc_fifo_obs_emit(&OBS, kind, b, e, &g_w); }

/* ps3tc_fifo_wrap_callback's observed path, on the model: ENTER, the protocol with its stats window, rewind, EXIT */
static void model_wrap(uint32_t count)
{
    const uint32_t begin_off = off_of(CTX.begin), tail_off = off_of(CTX.current);
    const int observed = OBS.reg.event && OBS.ctx == (const void *)&CTX;
    ps3tc_fifo_obs_wrap w = { begin_off, tail_off, count, 0u }, saved = g_w;
    g_w = w;
    if (observed) {
        const uint64_t t = m_tb(NULL);
        ps3tc_fifo_obs_emit(&OBS, PS3TC_FIFO_B_WRAP_ENTER, t, t, &g_w);
    }
    const ps3tc_fifo_port port = { &PUT, &GET, m_barrier, m_now, m_pause, NULL, observed ? m_phase : NULL, m_tb };
    const uint64_t t0 = m_tb(NULL);
    ps3tc_fifo_wrap_protocol(CTX.current, tail_off, begin_off, &port);
    stat_count++;
    stat_ticks += m_tb(NULL) - t0;
    CTX.current = CTX.begin;
    if (observed) {
        const uint64_t t = m_tb(NULL);
        ps3tc_fifo_obs_emit(&OBS, PS3TC_FIFO_B_WRAP_EXIT, t, t, &g_w);
    }
    g_w = saved;
}
/* emit `words` command words (publishing some of them), then wrap as the reserve would */
static void lap(uint32_t words, uint32_t flush_at)
{
    uint32_t i;
    for (i = 0; i < words; i++) {
        *CTX.current++ = 0x00040000u | i;
        if (i + 1 == flush_at)
            PUT = off_of(CTX.current);
    }
    model_wrap(64);
}

/* ---- the emulated GPU device (the legacy prg_port test hook) ------------------------------------------------- */
static pthread_mutex_t dev_m = PTHREAD_MUTEX_INITIALIZER;
static uint64_t dev_timer[4096];
static uint32_t dev_label[256];
static int wrap_in_reserve; /* owner-private: the next GPU reserve wraps the FIFO (a reserve that ran out of room) */
static void lap(uint32_t words, uint32_t flush_at);
static int d_reserve(void *c, uint32_t r, uint32_t l, uint32_t v)
{
    (void)c;
    if (wrap_in_reserve) {
        wrap_in_reserve = 0;
        lap(100, 0); /* inside ps3dk_prof_gpu_begin: the owner is pinned in a GPU call while the wrap waits */
    }
    pthread_mutex_lock(&dev_m);
    dev_timer[r] = 1;
    dev_label[l] = v;
    pthread_mutex_unlock(&dev_m);
    return 0;
}
static void d_flush(void *c) { (void)c; }
static uint32_t d_rlabel(void *c, uint32_t l) { uint32_t v; (void)c; pthread_mutex_lock(&dev_m); v = dev_label[l]; pthread_mutex_unlock(&dev_m); return v; }
static void d_wlabel(void *c, uint32_t l, uint32_t v) { (void)c; pthread_mutex_lock(&dev_m); dev_label[l] = v; pthread_mutex_unlock(&dev_m); }
static uint64_t d_rtimer(void *c, uint32_t r) { uint64_t v; (void)c; pthread_mutex_lock(&dev_m); v = dev_timer[r]; pthread_mutex_unlock(&dev_m); return v; }
static void d_wtimer(void *c, uint32_t r, uint64_t v) { (void)c; pthread_mutex_lock(&dev_m); dev_timer[r] = v; pthread_mutex_unlock(&dev_m); }
static void d_sync(void *c) { (void)c; }
static const prg_port g_dev = { NULL, d_reserve, d_flush, d_rlabel, d_wlabel, d_rtimer, d_wtimer, d_sync,
                                NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* ---- the callback sink ---------------------------------------------------------------------------------------- */
static pthread_mutex_t sink_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sink_c = PTHREAD_COND_INITIALIZER;
static FILE *sink_f;
static int block_events, blocked_now, short_events, short_pending, fail_final, writes_after_fail, failed_once;
static uint32_t seen_kinds[64]; /* per wrap id (< 64): bit k for kind k in an accepted FIFO_EVENTS chunk */
static atomic_uint seen_any;
static unsigned seen_wraps(uint64_t wrap, uint32_t kinds)
{
    unsigned r;
    pthread_mutex_lock(&sink_m);
    r = wrap < 64 && (seen_kinds[wrap] & kinds) == kinds;
    pthread_mutex_unlock(&sink_m);
    return r;
}
static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint64_t le64(const uint8_t *b) { return (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32; }
static long s_write(void *ctx, const void *buf, unsigned long n)
{
    const uint8_t *b = buf;
    uint32_t type = n >= 16 ? le32(b) : 0;
    long rc = (long)n;
    (void)ctx;
    pthread_mutex_lock(&sink_m);
    if (failed_once)
        writes_after_fail++;
    if (short_pending) {
        /* the remainder of a short-written chunk: the sink fails; nothing may be retried or written after this */
        short_pending = 0;
        failed_once = 1;
        pthread_mutex_unlock(&sink_m);
        return -1;
    }
    if (type == C_FIFO_EVENTS && n > 16 + 16 && memcmp(b, "PS3DKPRF", 8) != 0) {
        if (block_events) {
            blocked_now = 1;
            pthread_cond_broadcast(&sink_c);
            while (block_events)
                pthread_cond_wait(&sink_c, &sink_m);
            blocked_now = 0;
        }
        if (short_events) {
            short_events = 0;
            short_pending = 1;
            rc = (long)n - 1; /* a short write: the core offers the rest, which then fails */
        }
    }
    if (type == C_FIFO_STATUS && n >= 16 + 80 && (b[16 + 2] & FST_FINAL) && fail_final) {
        failed_once = 1;
        rc = -1;
    }
    if (rc == (long)n) {
        if (type == C_FIFO_EVENTS && n > 32 && memcmp(b, "PS3DKPRF", 8) != 0) {
            uint32_t cnt = le32(b + 16 + 8), i;
            for (i = 0; i < cnt; i++) {
                const uint8_t *e = b + 32 + 96u * i;
                uint64_t wrap = le64(e + 8);
                if (wrap < 64)
                    seen_kinds[wrap] |= 1u << (e[36] | e[37] << 8);
            }
            atomic_fetch_add(&seen_any, 1);
        }
        fwrite(buf, 1, n, sink_f);
    } else if (rc > 0) {
        fwrite(buf, 1, (size_t)rc, sink_f);
    }
    pthread_mutex_unlock(&sink_m);
    return rc;
}
static int s_close(void *ctx) { (void)ctx; return 0; }
static void sink_release(void)
{
    pthread_mutex_lock(&sink_m);
    block_events = 0;
    pthread_cond_broadcast(&sink_c);
    pthread_mutex_unlock(&sink_m);
}
static int sink_blocked(void) { int r; pthread_mutex_lock(&sink_m); r = blocked_now; pthread_mutex_unlock(&sink_m); return r; }

/* ---- profiler setup ------------------------------------------------------------------------------------------- */
static int start(uint32_t mode, uint32_t gpus)
{
    ps3dk_prof_config c;
    ps3dk_prof_ext_config x;
    ps3dk_prof_sink_callbacks s = { s_write, NULL, s_close, NULL };
    ps3dk_prof_config_defaults(&c);
    c.mode = mode;
    c.app = "fifo";
    c.build_id = "fifo";
    c.max_threads = 8;
    c.drain_interval_us = 1000;
    c.quiesce_timeout_us = 50000;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.gpu_domains_max = gpus;
    ps3dk_prof_test_hooks.gpu_port = &g_dev;
    CHECK(ps3dk_prof_set_sink_callbacks(&s) == 0);
    return ps3dk_prof_init_ex(&c, &x);
}

static const char *mode;
static uint32_t g_gpu, g_inst;
static ps3dk_prof_fifo_options g_opt = PS3DK_PROF_FIFO_OPTIONS_INIT;
static atomic_int owner_phase; /* 1 attached, 2 wraps done, 3 detached */
static atomic_int go_on;       /* main -> owner: continue */
static int detach_rc = 99, incb_rc = 99;
static uint64_t seed;

static atomic_int at_hook, resume_hook; /* attachrace / attachseal: the owner is held at a hook point */
static void hook(unsigned point, unsigned kind)
{
    if (point != 1) {
        int n = 0;
        if (((point == 2 && !strcmp(mode, "attachrace")) || (point == 3 && !strcmp(mode, "attachseal"))) &&
            !atomic_load(&at_hook)) {
            atomic_store(&at_hook, 1);
            while (!atomic_load(&resume_hook) && n++ < 20000)
                sleep_us(1000);
        }
        return;
    }
    if (!strcmp(mode, "incb") && kind == PS3TC_FIFO_B_PHASE1_BEGIN && incb_rc == 99)
        incb_rc = ps3dk_prof_fifo_detach(g_inst);
    if (!strcmp(mode, "recursion") && kind == PS3TC_FIFO_B_PHASE1_BEGIN && stat_count == 2)
        model_wrap(8); /* a nested wrap while the observer runs: every boundary of it is suppressed */
}

static void owner_attach(void)
{
    CHECK(ps3dk_prof_thread_begin("owner") == 0);
    CHECK(ps3dk_prof_gpu_attach((ps3dk_prof_gcm_ctx *)&CTX, 0, 2, 64, &g_gpu) == 0);
    CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, &g_opt, &g_inst) == 0 && g_inst == 1);
    if (seed)
        ps3dk_prof__fifo_test_seed(seed);
    atomic_store(&owner_phase, 1);
}
static void owner_finish(int want_detach)
{
    atomic_store(&owner_phase, 2);
    detach_rc = ps3dk_prof_fifo_detach(g_inst);
    CHECK(detach_rc == want_detach);
    atomic_store(&owner_phase, 3);
    ps3dk_prof_thread_end();
}

static void *owner_main(void *arg)
{
    int i, n = 0;
    (void)arg;
    owner_attach();
    if (!strcmp(mode, "basic") || !strcmp(mode, "incb") || !strcmp(mode, "recursion") || !strcmp(mode, "lostfinal")) {
        for (i = 0; i < 20; i++) {
            if (i % 4 == 0)
                ps3dk_prof_frame();
            lap(200 + i, (uint32_t)(i % 3) * 50);
        }
        owner_finish(0);
    } else if (!strcmp(mode, "parked1") || !strcmp(mode, "parked2") || !strcmp(mode, "gpuzone")) {
        lap(100, 0);
        lap(100, 50);
        atomic_store(&g_hold, !strcmp(mode, "parked2") ? 2 : 1);
        if (!strcmp(mode, "gpuzone")) {
            /* wrap 3 happens inside a GPU zone's reserve: the owner holds its domain pin and in_gpu across the wait */
            static ps3dk_prof_desc zd = { "zone", "", __FILE__, (uint32_t)__LINE__, 2u, 0u, 0u };
            ps3dk_prof_gpu_zone z;
            wrap_in_reserve = 1;
            CHECK(ps3dk_prof_gpu_begin(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, &zd, &z) == 0);
            CHECK(ps3dk_prof_gpu_end(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, &z) == 0);
        } else {
            lap(100, 0); /* wrap 3: parked until main releases it */
        }
        lap(100, 0);
        owner_finish(0);
    } else if (!strcmp(mode, "stop1") || !strcmp(mode, "stop2")) {
        lap(100, 0);
        atomic_store(&g_hold, !strcmp(mode, "stop2") ? 2 : 1);
        lap(100, 0); /* wrap 2: parked across the shutdown */
        lap(100, 0); /* refused: the instance is sealed */
        owner_finish(PS3DK_PROF_ESTOPPED);
    } else if (!strcmp(mode, "blocked") || !strcmp(mode, "shortwrite")) {
        for (i = 0; i < 200; i++)
            lap(50, 0);
        atomic_store(&owner_phase, 2);
        while (!atomic_load(&go_on) && n++ < 20000)
            sleep_us(1000);
        owner_finish(0);
    } else if (!strcmp(mode, "nearmax")) {
        for (i = 0; i < 3; i++)
            lap(50, 0);
        owner_finish(0);
    } else if (!strcmp(mode, "race") || !strcmp(mode, "pace")) {
        for (i = 0; i < 20000; i++) {
            if (i % 64 == 0)
                ps3dk_prof_frame();
            lap(20 + (uint32_t)(i % 7), 0);
            /* pace: the ring never fills, so the drainer reads slots no later drop or counter release orders */
            if (!strcmp(mode, "pace") && i % 4 == 3)
                sleep_us(100);
        }
        owner_finish(0);
    }
    return NULL;
}

/* ---- attach against the seal: the owner held before the claim (attachrace) or before the publication (attachseal),
 * shutdown completes meanwhile; the attach must then refuse with nothing installed and nothing live ---- */
static int race_rc = 99;
static uint32_t race_inst = 77;
static void *race_owner(void *arg)
{
    (void)arg;
    CHECK(ps3dk_prof_thread_begin("owner") == 0);
    CHECK(ps3dk_prof_gpu_attach((ps3dk_prof_gcm_ctx *)&CTX, 0, 2, 64, &g_gpu) == 0);
    race_rc = ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &race_inst);
    lap(100, 0); /* not observed */
    ps3dk_prof_thread_end();
    return NULL;
}

/* ---- refusal controls (all from one THREAD-mode capture) ----------------------------------------------------- */
static void *foreign_attach(void *arg)
{
    uint32_t inst = 77;
    int *rc = arg;
    rc[0] = ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst); /* unregistered */
    rc[1] = (int)inst;
    CHECK(ps3dk_prof_thread_begin("foreign") == 0);
    inst = 77;
    rc[2] = ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst); /* not the domain's owner */
    rc[3] = (int)inst;
    rc[4] = ps3dk_prof_fifo_detach(1);
    ps3dk_prof_thread_end();
    return NULL;
}
static void refusals(void)
{
    ps3dk_prof_fifo_options o;
    uint32_t inst = 77;
    int rc[5];
    pthread_t t;
    struct { uint32_t size, ring, max, res; int want; } bad[] = {
        { 15, 0, 0, 0, PS3DK_PROF_EINVAL }, { 16, 0, 0, 1, PS3DK_PROF_EINVAL }, { 16, 15, 0, 0, PS3DK_PROF_EINVAL },
        { 16, 65537, 16777216, 0, PS3DK_PROF_EINVAL }, { 16, 16, 5631, 0, PS3DK_PROF_EINVAL },
        { 16, 1024, 50000, 0, PS3DK_PROF_EINVAL }, { 16, 0, 16777217, 0, PS3DK_PROF_EINVAL },
    };
    unsigned i;
    CHECK(ps3dk_prof_thread_begin("owner") == 0);
    CHECK(ps3dk_prof_gpu_attach((ps3dk_prof_gcm_ctx *)&CTX, 0, 2, 64, &g_gpu) == 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        o = (ps3dk_prof_fifo_options){ bad[i].size, bad[i].ring, bad[i].max, bad[i].res };
        inst = 77;
        CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, &o, &inst) == bad[i].want && inst == 0);
    }
    inst = 77;
    CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&OTHER, NULL, &inst) == PS3DK_PROF_EINVAL && !inst);
    CHECK(ps3dk_prof_fifo_attach(g_gpu + 1, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst) == PS3DK_PROF_EINVAL && !inst);
    CHECK(ps3dk_prof_fifo_attach(g_gpu, NULL, NULL, &inst) == PS3DK_PROF_EINVAL);
    CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, NULL) == PS3DK_PROF_EINVAL);
    atomic_store(&g_bridge_refuse, 1);
    CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst) == PS3DK_PROF_EINVAL && !inst);
    atomic_store(&g_bridge_refuse, 0);
    CHECK(ps3dk_prof__fifo_test_state() == FS_NONE);
    CHECK(ps3dk_prof_fifo_detach(1) == PS3DK_PROF_EINVAL);
    CHECK(pthread_create(&t, NULL, foreign_attach, rc) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(rc[0] == PS3DK_PROF_ETHREAD && rc[1] == 0);
    CHECK(rc[2] == PS3DK_PROF_EPERM && rc[3] == 0);
    CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst) == 0 && inst == 1);
    g_inst = inst;
    CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst) == PS3DK_PROF_EFULL && !inst);
    CHECK(pthread_create(&t, NULL, foreign_attach, rc) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(rc[4] == PS3DK_PROF_EPERM);
    CHECK(ps3dk_prof_fifo_detach(2) == PS3DK_PROF_EINVAL);
    lap(100, 0);
    CHECK(ps3dk_prof_fifo_detach(1) == 0);
    CHECK(ps3dk_prof_fifo_detach(1) == PS3DK_PROF_EALREADY);
    lap(100, 0); /* not observed: removed */
    inst = 77;
    CHECK(ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst) == PS3DK_PROF_EFULL && !inst);
    ps3dk_prof_thread_end();
}

int main(int argc, char **argv)
{
    pthread_t t;
    int rc, want = 0;
    if (argc != 3)
        return 2;
    mode = argv[1];
    sink_f = fopen(argv[2], "wb");
    if (!sink_f)
        return 2;
    ps3dk_prof__fifo_test_hook = hook;
    if (!strcmp(mode, "app")) {
        uint32_t inst = 77;
        CHECK(start(PS3DK_PROF_MODE_APP, 1) == 0);
        CHECK(ps3dk_prof_thread_begin("owner") == 0);
        CHECK(ps3dk_prof_gpu_attach((ps3dk_prof_gcm_ctx *)&CTX, 0, 2, 64, &g_gpu) == 0);
        rc = ps3dk_prof_fifo_attach(g_gpu, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst);
        printf("app attach rc=%d instance=%u\n", rc, (unsigned)inst);
        CHECK(rc == PS3DK_PROF_EPERM && inst == 0);
        ps3dk_prof_thread_end();
        CHECK(ps3dk_prof_drain() == 0);
        (void)ps3dk_prof_shutdown();
        goto done;
    }
    if (!strcmp(mode, "nogpu")) {
        uint32_t inst = 77;
        CHECK(start(PS3DK_PROF_MODE_THREAD, 0) == 0);
        CHECK(ps3dk_prof_fifo_attach(1026, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst) == PS3DK_PROF_ENOTSUP && !inst);
        (void)ps3dk_prof_shutdown();
        goto done;
    }
    {
        uint32_t inst = 77;
        CHECK(ps3dk_prof_fifo_attach(1026, (ps3dk_prof_gcm_ctx *)&CTX, NULL, &inst) == PS3DK_PROF_ENOTINIT && !inst);
    }
    if (!strcmp(mode, "blocked") || !strcmp(mode, "shortwrite"))
        g_opt.ring_records = 16;
    if (!strcmp(mode, "race"))
        g_opt.ring_records = 64;
    if (!strcmp(mode, "pace"))
        g_opt.ring_records = 4096;
    if (!strcmp(mode, "nearmax"))
        seed = (UINT64_C(1) << 63) - 4u;
    if (!strcmp(mode, "blocked"))
        block_events = 1;
    if (!strcmp(mode, "shortwrite"))
        short_events = 1;
    if (!strcmp(mode, "lostfinal"))
        fail_final = 1;
    CHECK(start(PS3DK_PROF_MODE_THREAD, 1) == 0);
    if (!strcmp(mode, "attachrace") || !strcmp(mode, "attachseal")) {
        CHECK(pthread_create(&t, NULL, race_owner, NULL) == 0);
        WAIT_FOR(atomic_load(&at_hook), 10000);
        rc = ps3dk_prof_shutdown(); /* the owner is held inside attach and never acknowledges */
        printf("shutdown rc=%d\n", rc);
        atomic_store(&resume_hook, 1);
        CHECK(pthread_join(t, NULL) == 0);
        printf("attach rc=%d instance=%u observer=%d block=%d state=%u\n", race_rc, (unsigned)race_inst,
               OBS.reg.event != NULL, ps3dk_prof__fifo_test_block() != NULL, (unsigned)ps3dk_prof__fifo_test_state());
        CHECK(race_rc == PS3DK_PROF_ESTOPPED && race_inst == 0 && OBS.reg.event == NULL &&
              ps3dk_prof__fifo_test_block() == NULL && ps3dk_prof__fifo_test_state() == FS_SEALED);
        goto done;
    }
    if (!strcmp(mode, "refusals")) {
        refusals();
        rc = ps3dk_prof_shutdown();
        CHECK(rc == 0);
        goto done;
    }
    CHECK(pthread_create(&t, NULL, owner_main, NULL) == 0);
    if (!strcmp(mode, "parked1") || !strcmp(mode, "parked2") || !strcmp(mode, "gpuzone")) {
        unsigned kinds = !strcmp(mode, "parked2") ? 0x1Eu : 0x06u; /* ENTER+P1B, or ENTER..P2B */
        WAIT_FOR(atomic_load(&g_parked) != 0, 10000);
        /* the owner is parked inside wrap 3; the independent drain thread must persist its boundaries */
        WAIT_FOR(seen_wraps(3, kinds), 10000);
        CHECK(atomic_load(&g_parked) != 0 && atomic_load(&owner_phase) == 1);
        printf("parked phase=%d seen=1\n", atomic_load(&g_parked));
        atomic_store(&g_hold, 0);
    } else if (!strcmp(mode, "stop1") || !strcmp(mode, "stop2")) {
        unsigned kinds = !strcmp(mode, "stop2") ? 0x1Eu : 0x06u;
        WAIT_FOR(atomic_load(&g_parked) != 0 && seen_wraps(2, kinds), 10000);
        rc = ps3dk_prof_shutdown(); /* the owner is parked and never acknowledges: NOT_QUIESCED */
        printf("shutdown rc=%d\n", rc);
        CHECK(rc == PS3DK_PROF_ENOTQUIESCED);
        CHECK(ps3dk_prof__fifo_test_state() == FS_SEALED);
        atomic_store(&g_hold, 0);
        CHECK(pthread_join(t, NULL) == 0);
        goto done;
    } else if (!strcmp(mode, "blocked")) {
        WAIT_FOR(sink_blocked(), 10000);
        /* the drain thread is stuck in the sink; the owner must still finish every wrap */
        WAIT_FOR(atomic_load(&owner_phase) == 2, 20000);
        CHECK(sink_blocked() && atomic_load(&owner_phase) == 2);
        printf("blocked owner_done=%d sink_blocked=%d\n", atomic_load(&owner_phase) == 2, sink_blocked());
        sink_release();
        atomic_store(&go_on, 1);
    } else if (!strcmp(mode, "shortwrite")) {
        WAIT_FOR(atomic_load(&owner_phase) == 2, 20000);
        sleep_us(20000);
        /* nothing was acknowledged: the failed chunk's slots are still the drainer's, never released to the owner */
        printf("shortwrite read_idx=%llu\n", (unsigned long long)ps3dk_prof__fifo_test_read_idx());
        CHECK(ps3dk_prof__fifo_test_read_idx() == 0);
        atomic_store(&go_on, 1);
        want = PS3DK_PROF_ESINKFAIL;
    } else if (!strcmp(mode, "lostfinal")) {
        want = PS3DK_PROF_ESINKFAIL;
    }
    CHECK(pthread_join(t, NULL) == 0);
    rc = ps3dk_prof_shutdown();
    printf("shutdown rc=%d\n", rc);
    CHECK(rc == want);
done:
    pthread_mutex_lock(&sink_m);
    printf("writes_after_fail=%d\n", writes_after_fail);
    pthread_mutex_unlock(&sink_m);
    if (!strcmp(mode, "incb"))
        CHECK(incb_rc == PS3DK_PROF_EBUSY);
    printf("stats count=%llu ticks=%llu suppressed=%llu incb=%d detach=%d\n", (unsigned long long)stat_count,
           (unsigned long long)stat_ticks, (unsigned long long)ps3tc_fifo_observe_suppressed(), incb_rc, detach_rc);
    fclose(sink_f);
    printf("checks=%u failed=%u\n", atomic_load(&checks), atomic_load(&failed));
    return atomic_load(&failed) ? 1 : 0;
}
