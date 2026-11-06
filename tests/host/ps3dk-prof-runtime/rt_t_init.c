/* Init, the gate and the sink setters (r2 3.3, E3, F3, Q7, Q9 caps). */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int gate(void) { return (int)RT_STAT(GATE); }

static uint64_t clk_const(void) { return 0x1234; }

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int finish_ok(const char *dir)
{
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_OK);
    {
        PS3DK_PROF_ZONE("z");
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(dir, 0);
}

static int precreate(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fputc('x', f);
    return fclose(f);
}

/* ---- EEXIST and open errors ----------------------------------------------------------------------------------------- */

int rt_sink_eexist_explicit(const char *out)
{
    ps3dk_prof_config c;
    char path[512];
    rt_config(&c);
    ps3dk_prof_test_hooks.clock = clk_const;   /* the session id, so the name, is known */
    snprintf(path, sizeof path, "%s/rt-0000000000001234.prof", out);
    CHECK_EQ(precreate(path), 0);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EEXIST);
    CHECK_EQ(gate(), RT_G_UNINIT);
    CHECK_EQ(rt_file_size(path), 1);   /* nothing written into the existing file */
    return 0;
}

int rt_init_eexist_retryable(const char *out)
{
    ps3dk_prof_config c;
    char path[512], dir2[512];
    rt_config(&c);
    ps3dk_prof_test_hooks.clock = clk_const;
    snprintf(path, sizeof path, "%s/rt-0000000000001234.prof", out);
    snprintf(dir2, sizeof dir2, "%s/b", out);
    CHECK_EQ(mkdir(dir2, 0755), 0);
    CHECK_EQ(precreate(path), 0);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EEXIST);
    CHECK_EQ(gate(), RT_G_UNINIT);
    CHECK_EQ(rt_set_dir(dir2), PS3DK_PROF_OK);   /* a new path, then a retry */
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(dir2);
}

int rt_init_open_error_retryable(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.open_errno = EACCES;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ESINKFAIL);
    CHECK_EQ(gate(), RT_G_UNINIT);
    ps3dk_prof_test_hooks.open_errno = 0;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}

/* ---- step-5 failures (F3) and allocation ---------------------------------------------------------------------------- */

int rt_init_owner_identity_failure_retryable(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.id_fail = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ETHREAD);
    CHECK_EQ(gate(), RT_G_UNINIT);
    ps3dk_prof_test_hooks.id_fail = 0;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}

int rt_init_clock_failure_eclock_retryable(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.freq_zero = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ECLOCK);
    CHECK_EQ(gate(), RT_G_UNINIT);
    ps3dk_prof_test_hooks.freq_zero = 0;
    ps3dk_prof_test_hooks.mono_clock_fail = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ECLOCK);
    CHECK_EQ(gate(), RT_G_UNINIT);
    ps3dk_prof_test_hooks.mono_clock_fail = 0;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}

int rt_init_failure_retry(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.alloc_fail = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ENOMEM);
    CHECK_EQ(gate(), RT_G_UNINIT);
    ps3dk_prof_test_hooks.alloc_fail = 0;
    ps3dk_prof_test_hooks.freq_zero = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ECLOCK);
    CHECK_EQ(gate(), RT_G_UNINIT);
    ps3dk_prof_test_hooks.freq_zero = 0;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}

/* ---- step-7 failures ------------------------------------------------------------------------------------------------ */

static int terminal_case(const char *out)
{
    ps3dk_prof_config c;
    char path[512];
    rt_config(&c);
    c.mode = PS3DK_PROF_MODE_THREAD;
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.thread_create_fail = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ETHREAD);
    CHECK_EQ(gate(), RT_G_TERMINAL);   /* the file exists: a retry would hit EEXIST */
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_file_size(path), 0);   /* kept, nothing written */
    ps3dk_prof_test_hooks.thread_create_fail = 0;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EALREADY);
    CHECK_EQ(ps3dk_prof_set_sink_path(out), PS3DK_PROF_EALREADY);
    rt_expect_path(path, 1);   /* an empty file is a torn tail: INCOMPLETE */
    return 0;
}

int rt_init_terminal_after_sink_created(const char *out) { return terminal_case(out); }
int rt_drain_thread_create_failure(const char *out) { return terminal_case(out); }

int rt_init_callback_sink_thread_create_failure_retryable(const char *out)
{
    ps3dk_prof_config c;
    char path[512];
    rt_config(&c);
    c.mode = PS3DK_PROF_MODE_THREAD;
    c.drain_interval_us = 1000;
    CHECK_EQ(rt_use_mem_sink(0), 0);
    ps3dk_prof_test_hooks.thread_create_fail = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ETHREAD);
    CHECK_EQ(gate(), RT_G_UNINIT);   /* no external state was created */
    CHECK_EQ(g_mem.writes, 0);       /* no callback runs during init */
    ps3dk_prof_test_hooks.thread_create_fail = 0;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    snprintf(path, sizeof path, "%s/cb.prof", out);
    CHECK_EQ(rt_mem_dump(path), 0);
    rt_expect_path(path, 0);
    return 0;
}

/* ---- concurrency at the gate ---------------------------------------------------------------------------------------- */

static rt_event g_in_init, g_release, g_init_done, g_finish;
static int g_blocked;
static ps3dk_prof_config g_cfg;

static uint64_t clk_block_once(void)
{
    if (!__atomic_exchange_n(&g_blocked, 1, __ATOMIC_SEQ_CST)) {   /* init's tb_at_init read */
        rt_event_set(&g_in_init);
        rt_event_wait(&g_release, 10000);
    }
    return mono_ns();
}

/* The APP owner is the thread that initialized (plan 5.4), so it stays alive and finishes the capture itself:
   init, publish the result, wait for main's checks, then register, emit a zone and shut down. */
typedef struct owner_rc {
    int init, begin, shutdown, waited;
} owner_rc;

static void *init_thread(void *a)
{
    owner_rc *r = a;
    r->init = ps3dk_prof_init(&g_cfg);
    rt_event_set(&g_init_done);
    r->waited = rt_event_wait(&g_finish, 10000);
    if (r->init != PS3DK_PROF_OK || r->waited != 0)
        return NULL;
    r->begin = ps3dk_prof_thread_begin("owner");
    {
        PS3DK_PROF_ZONE("z");
    }
    r->shutdown = ps3dk_prof_shutdown();
    return NULL;
}

static int blocked_init_case(const char *out, int setters_only)
{
    rt_thread t;
    owner_rc r = {99, 99, 99, 99};
    ps3dk_prof_sink_callbacks cb;
    rt_config(&g_cfg);
    rt_event_init(&g_in_init);
    rt_event_init(&g_release);
    rt_event_init(&g_init_done);
    rt_event_init(&g_finish);
    ps3dk_prof_test_hooks.clock = clk_block_once;
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    rt_spawn(&t, init_thread, &r);
    CHECK_EQ(rt_event_wait(&g_in_init, 5000), 0);
    CHECK_EQ(gate(), RT_G_INITIALIZING);
    if (!setters_only)
        CHECK_EQ(ps3dk_prof_init(&g_cfg), PS3DK_PROF_EBUSY);   /* never waits */
    CHECK_EQ(ps3dk_prof_set_sink_path("/nonexistent-dir"), PS3DK_PROF_EBUSY);
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(ps3dk_prof_set_sink_callbacks(&cb), PS3DK_PROF_EBUSY);   /* EBUSY before validation */
    rt_event_set(&g_release);
    CHECK_EQ(rt_event_wait(&g_init_done, 5000), 0);   /* the owner's init has returned; the owner is still alive */
    CHECK_EQ(r.init, PS3DK_PROF_OK);
    CHECK_EQ(gate(), RT_G_READY);
    CHECK_EQ(ps3dk_prof_init(&g_cfg), PS3DK_PROF_EALREADY);
    CHECK_EQ(ps3dk_prof_set_sink_path("/nonexistent-dir"), PS3DK_PROF_EALREADY);
    /* Non-owner control: main (never registered) is refused before any state change.  The latch stays clear, and the
       owner's shutdown below returning OK (not EALREADY) shows shutdown_once was not consumed. */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_EPERM);
    CHECK_EQ(RT_STAT(LATCH), 0);
    CHECK_EQ(gate(), RT_G_READY);
    rt_event_set(&g_finish);
    rt_join(&t);
    CHECK_EQ(r.waited, 0);
    CHECK_EQ(r.begin, PS3DK_PROF_OK);
    CHECK_EQ(r.shutdown, PS3DK_PROF_OK);
    return rt_expect(out, 0);   /* the capture is in the pre-race directory */
}

int rt_init_concurrent_ebusy(const char *out) { return blocked_init_case(out, 0); }
int rt_setter_vs_init_race(const char *out) { return blocked_init_case(out, 1); }

int rt_setter_after_ready_ealready(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_sink_callbacks cb;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_set_sink_path(out), PS3DK_PROF_EALREADY);
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(ps3dk_prof_set_sink_callbacks(&cb), PS3DK_PROF_EALREADY);
    return finish_ok(out);
}

static long dummy_write(void *ctx, const void *b, unsigned long n)
{
    (void)ctx;
    (void)b;
    return (long)n;
}
static int dummy_close(void *ctx)
{
    (void)ctx;
    return 0;
}

int rt_setter_validation(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_sink_callbacks cb;
    char longp[300];
    memset(longp, 'a', sizeof longp);
    longp[256] = 0;   /* length 256: too long */
    CHECK_EQ(ps3dk_prof_set_sink_path(NULL), PS3DK_PROF_EINVAL);
    CHECK_EQ(ps3dk_prof_set_sink_path(""), PS3DK_PROF_EINVAL);
    CHECK_EQ(ps3dk_prof_set_sink_path(longp), PS3DK_PROF_EINVAL);
    longp[255] = 0;   /* length 255: accepted */
    CHECK_EQ(ps3dk_prof_set_sink_path(longp), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_set_sink_callbacks(NULL), PS3DK_PROF_EINVAL);
    memset(&cb, 0, sizeof cb);
    cb.close = dummy_close;
    CHECK_EQ(ps3dk_prof_set_sink_callbacks(&cb), PS3DK_PROF_EINVAL);   /* no write */
    cb.write = dummy_write;
    cb.close = NULL;
    CHECK_EQ(ps3dk_prof_set_sink_callbacks(&cb), PS3DK_PROF_EINVAL);   /* no close */
    cb.close = dummy_close;
    CHECK_EQ(ps3dk_prof_set_sink_callbacks(&cb), PS3DK_PROF_OK);       /* flush optional */
    CHECK_EQ(gate(), RT_G_UNINIT);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);   /* the last setter wins: the file sink */
    rt_config(&c);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}

/* ---- the name (Q7) and the caps (Q9) -------------------------------------------------------------------------------- */

int rt_sink_name_validation(const char *out)
{
    ps3dk_prof_config c;
    char dir[300];
    size_t n;
    static const char *bad[] = {"", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "a/b", "a b", "a.b", "\xc3\xa9"};
    unsigned i;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        c.app = bad[i];
        CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EINVAL);
        CHECK_EQ(gate(), RT_G_UNINIT);
    }
    c.app = NULL;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EINVAL);
    /* A directory that fits the setter but not the formatted name: snprintf truncation is EINVAL. */
    n = (size_t)snprintf(dir, sizeof dir, "%s/", out);
    while (n < 250)
        dir[n++] = 'x';
    dir[n] = 0;
    CHECK_EQ(ps3dk_prof_set_sink_path(dir), PS3DK_PROF_OK);
    c.app = "rt";
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EINVAL);
    CHECK_EQ(gate(), RT_G_UNINIT);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    c.app = "rt_OK-9";   /* 1..32 of [A-Za-z0-9_-] */
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}

static int bad_cfg(ps3dk_prof_config *c)
{
    int rc = ps3dk_prof_init(c);
    if (rc != PS3DK_PROF_EINVAL || gate() != RT_G_UNINIT) {
        fprintf(stderr, "bad config accepted: rc %d gate %d (threads %u desc %u ring %u interval %u stack %u)\n", rc,
                gate(), c->max_threads, c->max_descriptors, c->ring_records, c->drain_interval_us,
                c->drain_stack_bytes);
        return 1;
    }
    rt_config(c);
    return 0;
}

int rt_capacity_arithmetic_caps(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    c.ring_records = 256;     CHECK_OK(bad_cfg(&c));
    c.ring_records = 511;     CHECK_OK(bad_cfg(&c));
    c.ring_records = 513;     CHECK_OK(bad_cfg(&c));
    c.ring_records = 768;     CHECK_OK(bad_cfg(&c));
    c.ring_records = 1u << 21; CHECK_OK(bad_cfg(&c));
    c.max_threads = 0;        CHECK_OK(bad_cfg(&c));
    c.max_threads = 1025;     CHECK_OK(bad_cfg(&c));
    c.max_descriptors = 0;    CHECK_OK(bad_cfg(&c));
    c.max_descriptors = 65537; CHECK_OK(bad_cfg(&c));
    c.drain_interval_us = 999; CHECK_OK(bad_cfg(&c));
    c.drain_interval_us = 1000001; CHECK_OK(bad_cfg(&c));
    c.drain_stack_bytes = 16383; CHECK_OK(bad_cfg(&c));
    c.drain_stack_bytes = 1048577; CHECK_OK(bad_cfg(&c));
    c.quiesce_timeout_us = 60000001; CHECK_OK(bad_cfg(&c));
    c.mode = 0;               CHECK_OK(bad_cfg(&c));
    c.mode = 3;               CHECK_OK(bad_cfg(&c));
    c.ring_records = 512;     /* the smallest legal ring: 256 ENDs + GAP + 1 fit (r2 3.3) */
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}

int rt_config_caps_overflow(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_init(NULL), PS3DK_PROF_EINVAL);
    c.max_threads = 1024;
    c.ring_records = 1u << 20;   /* 32 GiB of rings: over the 256 MiB cap (and over a 32-bit size_t) */
    CHECK_OK(bad_cfg(&c));
    c.max_threads = 8;
    c.ring_records = 1u << 20;   /* 8 x 32 MiB = 256 MiB of rings + the rest: just over the cap */
    CHECK_OK(bad_cfg(&c));
    c.max_threads = 1024;
    c.max_descriptors = 65536;
    c.ring_records = 1u << 16;   /* 2 GiB */
    CHECK_OK(bad_cfg(&c));
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    return finish_ok(out);
}
