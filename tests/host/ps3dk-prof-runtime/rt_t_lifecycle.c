/* Lifecycle, API entry checks, ownership and the drain thread (format 5.4, 7; r2 10.1; E3). */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

#include <errno.h>
#include <string.h>

static char g_cb_path[600];

static const char *cb_path(const char *out)
{
    snprintf(g_cb_path, sizeof g_cb_path, "%s/cb.prof", out);
    return g_cb_path;
}

/* ---- APP mode: EPERM from a non-owner, EBUSY from a reentrant drain ---------------------------------------------- */

static int g_reent_rc = 99;
static void reent_drain(void)
{
    if (ALD(g_reent_rc) == 99)
        AST(g_reent_rc, ps3dk_prof_drain());
}

static void *other_drain(void *a)
{
    int *rc = (int *)a;
    rc[0] = ps3dk_prof_drain();
    rc[1] = ps3dk_prof_shutdown();
    return NULL;
}

int rt_app_drain_eperm_ebusy(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    int rc[2] = {99, 99};
    rt_config(&c);
    CHECK_EQ(rt_use_mem_sink(0), 0);
    AST(g_mem.in_write, reent_drain);
    CHECK_OK(rt_start(NULL, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    rt_spawn(&t, other_drain, rc);
    rt_join(&t);
    CHECK_EQ(rc[0], PS3DK_PROF_EPERM);
    CHECK_EQ(rc[1], PS3DK_PROF_EPERM);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ALD(g_reent_rc), PS3DK_PROF_EBUSY);
    AST(g_mem.in_write, (void (*)(void))NULL);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    rt_expect_path(cb_path(out), 0);
    return 0;
}

static void *drain_other_thread(void *a)
{
    *(int *)a = ps3dk_prof_drain();
    return NULL;
}

int rt_owner_pthread_equal_opaque(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    int rc = 99;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, drain_other_thread, &rc);
    rt_join(&t);
    CHECK_EQ(rc, PS3DK_PROF_EPERM);   /* identity compared with pthread_equal only */
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}

int rt_owner_identity_failure_is_eperm(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    ps3dk_prof_test_hooks.id_fail = 1;   /* a failed identity read is never the owner */
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_EPERM);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_EPERM);
    ps3dk_prof_test_hooks.id_fail = 0;
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}

/* R3 (codex 2026-10-22): a failed OS thread id read refuses the registration with ETHREAD before anything changes.
 * The slot stays FREE, no incarnation id or slot generation is consumed, and the thread's TLS stays unregistered, so
 * the retry is the first registration the capture ever sees. */
int rt_thread_begin_os_id_failure_refused(const char *out)
{
    ps3dk_prof_config c;
    char path[512];
    rt_cap cap;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 0));
    ps3dk_prof_test_hooks.os_id_fail = 1;
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_ETHREAD);
    CHECK_EQ(RT_READ(SLOT), 0);
    CHECK_EQ(RT_READ(INCARNATION), 0);
    CHECK_EQ(RT_READ(GENERATION), 0);
    CHECK_EQ(ps3dk_prof_test_stat(PS3DK_PROF_TEST_STAT_SLOT_STATE_BASE + 0), RT_SLOT_FREE);
    ps3dk_prof_test_hooks.os_id_fail = 0;
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_OK);
    CHECK_EQ(RT_READ(SLOT), 1);
    CHECK_EQ(RT_READ(INCARNATION), 1);   /* the refused attempt consumed no incarnation id */
    CHECK_EQ(RT_READ(GENERATION), 1);    /* nor a slot generation */
    {
        PS3DK_PROF_ZONE("z");
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(cap.nthreads, 1);
    CHECK_EQ(cap.threads_inc[0], 1);
    CHECK_EQ(cap.threads_slot[0], 0);
    CHECK_EQ(cap.unreg, 0);   /* an ETHREAD refusal is not counted as unregistered (open question in PAL-PREP-r1) */
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static void *os_id_fail_then_retry(void *a)
{
    (void)a;
    ps3dk_prof_test_hooks.os_id_fail = 1;   /* main waits in rt_join: only this thread runs the profiler now */
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_ETHREAD);
    WCHECK(RT_READ(SLOT) == 0);
    ps3dk_prof_test_hooks.os_id_fail = 0;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    WCHECK(RT_READ(INCARNATION) == 2);
    {
        PS3DK_PROF_ZONE("w");
    }
    ps3dk_prof_thread_end();
    return NULL;
}

/* The same refusal on a second thread while main is registered: main's incarnation is unaffected, and the retry gets
 * the next incarnation id (2), not one past a consumed id. */
int rt_thread_begin_os_id_failure_second_thread(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    char path[512];
    rt_cap cap;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("m");
    }
    rt_spawn(&t, os_id_fail_then_retry, NULL);
    rt_join(&t);
    CHECK(!rt_worker_failed());
    CHECK_EQ(RT_READ(INCARNATION), 1);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(cap.nthreads, 2);
    CHECK_EQ(cap.threads_inc[0], 1);
    CHECK_EQ(cap.threads_inc[1], 2);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_thread_second_shutdown_ealready(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    c.mode = PS3DK_PROF_MODE_THREAD;
    c.drain_interval_us = 1000;
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_EBUSY);   /* THREAD mode: the drain thread is the only drainer */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_EALREADY);
    return rt_expect(out, 0);
}

/* ---- shutdown from inside a sink callback: EBUSY, no state change, in both modes --------------------------------- */

static int g_cb_sd = 99;
static void reent_shutdown(void)
{
    if (ALD(g_cb_sd) == 99)
        AST(g_cb_sd, ps3dk_prof_shutdown());
}

static int shutdown_from_callback(const char *out, uint32_t mode)
{
    ps3dk_prof_config c;
    rt_cap cap;
    uint64_t end;
    rt_config(&c);
    c.mode = mode;
    c.drain_interval_us = 1000;
    CHECK_EQ(rt_use_mem_sink(0), 0);
    AST(g_mem.in_write, reent_shutdown);
    CHECK_OK(rt_start(NULL, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    if (mode == PS3DK_PROF_MODE_APP)
        CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    end = rt_now_ms() + 5000;
    while (ALD(g_cb_sd) == 99 && rt_now_ms() < end)
        rt_sleep_ms(1);
    CHECK_EQ(ALD(g_cb_sd), PS3DK_PROF_EBUSY);
    CHECK_EQ(RT_STAT(LATCH), 0);   /* no state change */
    AST(g_mem.in_write, (void (*)(void))NULL);
    {
        PS3DK_PROF_ZONE("z2");   /* still recording */
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);   /* a later proper shutdown works */
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    CHECK_EQ(rt_cap_load(cb_path(out), &cap), 0);
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 2);
    rt_cap_free(&cap);
    rt_expect_path(cb_path(out), 0);
    return 0;
}

int rt_shutdown_from_callback_ebusy(const char *out) { return shutdown_from_callback(out, PS3DK_PROF_MODE_APP); }
int rt_shutdown_from_callback_ebusy_thread(const char *out)
{
    return shutdown_from_callback(out, PS3DK_PROF_MODE_THREAD);
}

/* ---- init once ----------------------------------------------------------------------------------------------------- */

int rt_init_once(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EALREADY);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_EALREADY);   /* no restart */
    return rt_expect(out, 0);
}

/* ---- registration after the latch ---------------------------------------------------------------------------------- */

static int g_late_rc = 99;
static void *late_register(void *a)
{
    (void)a;
    AST(g_late_rc, ps3dk_prof_thread_begin("late"));
    return NULL;
}

static void hook_register_during_seal(void)
{
    rt_thread t;
    rt_spawn(&t, late_register, NULL);
    rt_join(&t);
}

int rt_register_after_latch_refused(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    char path[512];
    rt_cap cap;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    ps3dk_prof_test_hooks.before_frontier = hook_register_during_seal;
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(ALD(g_late_rc), PS3DK_PROF_ESTOPPED);
    AST(g_late_rc, 99);
    rt_spawn(&t, late_register, NULL);
    rt_join(&t);
    CHECK_EQ(ALD(g_late_rc), PS3DK_PROF_ESTOPPED);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(cap.nthreads, 1);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- self-quiescence ----------------------------------------------------------------------------------------------- */

int rt_shutdown_caller_self_quiesces(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone a, b, d;
    uint64_t t0;
    char path[512];
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    c.quiesce_timeout_us = 10000000;
    CHECK_OK(rt_start(out, &c, 1));
    a = PS3DK_PROF_ZONE_BEGIN("a");
    b = PS3DK_PROF_ZONE_BEGIN("b");
    d = PS3DK_PROF_ZONE_BEGIN("d");
    t0 = rt_now_ms();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK(rt_now_ms() - t0 < 2000);   /* never waits for itself */
    PS3DK_PROF_ZONE_END(d);
    PS3DK_PROF_ZONE_END(b);
    PS3DK_PROF_ZONE_END(a);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(rt_count(&cap, 1, RT_END, 1), 3);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_ACKED);
    CHECK_EQ(f->open_spans, 3);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static int g_drain_reg = 99;
static void register_in_drain(void)
{
    if (ALD(g_drain_reg) == 99)
        AST(g_drain_reg, ps3dk_prof_thread_begin("drain"));
}

int rt_drain_thread_self_acks(const char *out)
{
    ps3dk_prof_config c;
    uint64_t t0, end;
    rt_cap cap;
    uint32_t i;
    rt_config(&c);
    c.mode = PS3DK_PROF_MODE_THREAD;
    c.drain_interval_us = 1000;
    c.quiesce_timeout_us = 10000000;
    CHECK_EQ(rt_use_mem_sink(0), 0);
    AST(g_mem.in_write, register_in_drain);
    CHECK_OK(rt_start(NULL, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    end = rt_now_ms() + 5000;
    while (ALD(g_drain_reg) == 99 && rt_now_ms() < end)
        rt_sleep_ms(1);
    CHECK_EQ(ALD(g_drain_reg), PS3DK_PROF_OK);
    t0 = rt_now_ms();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK(rt_now_ms() - t0 < 3000);   /* the drain thread acked itself: no timeout */
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    CHECK_EQ(rt_cap_load(cb_path(out), &cap), 0);
    CHECK_EQ(cap.nthreads, 2);
    CHECK_EQ(cap.nfinal, 2);
    for (i = 0; i < cap.nfinal; i++)
        CHECK_EQ(cap.finals[i].state, RT_ACKED);
    rt_cap_free(&cap);
    rt_expect_path(cb_path(out), 0);
    return 0;
}

/* ---- the drain thread's wake flag (E5) ----------------------------------------------------------------------------- */

int rt_thread_mode_wake_flag_observed_within_nominal_interval(const char *out)
{
    ps3dk_prof_config c;
    uint64_t seen, set;
    int i;
    rt_config(&c);
    c.mode = PS3DK_PROF_MODE_THREAD;
    c.drain_interval_us = 1000;
    CHECK_EQ(rt_use_mem_sink(0), 0);
    CHECK_OK(rt_start(NULL, &c, 1));
    for (i = 0; i < 20; i++) {
        PS3DK_PROF_ZONE("z");
        rt_sleep_ms(1);
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    seen = RT_STAT(WAKE_SEEN_ITERATION);
    set = RT_STAT(WAKE_SET_ITERATION);
    CHECK(seen > 0);
    CHECK(seen >= set);
    CHECK(seen - set <= 1);   /* observed at the next post-sleep check: counted in iterations, not wall time */
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    rt_expect_path(cb_path(out), 0);
    return 0;
}

int rt_drain_thread_join_failure(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    c.mode = PS3DK_PROF_MODE_THREAD;
    c.drain_interval_us = 1000;
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    ps3dk_prof_test_hooks.join_fail = 1;
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ETHREAD);   /* folded in after every sink and trailer severity */
    return rt_expect(out, 0);
}

int rt_monotonic_clock_failure(const char *out)
{
    ps3dk_prof_config c;
    uint64_t t0;
    rt_config(&c);
    c.quiesce_timeout_us = 10000000;
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    ps3dk_prof_test_hooks.mono_clock_fail = 1;
    t0 = rt_now_ms();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ECLOCK);   /* the wait ends at once; everyone quiesced */
    CHECK(rt_now_ms() - t0 < 2000);
    return rt_expect(out, 0);
}

/* ---- quiescence timeout: late data is absent, Fd and Ft do not grow ----------------------------------------------- */

static rt_event g_w_ready, g_w_go;
static void *blocked_worker(void *a)
{
    ps3dk_prof_zone h, late;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    h = PS3DK_PROF_ZONE_BEGIN("w");
    rt_event_set(&g_w_ready);
    WCHECK(rt_event_wait(&g_w_go, 20000) == 0);
    PS3DK_PROF_ZONE_END(h);                    /* observes the latch after the frontier: STOP batch + ack */
    late = PS3DK_PROF_ZONE_BEGIN("w-late");    /* acked: INERT, no registration */
    WCHECK(late.kind == 0);
    PS3DK_PROF_ZONE_END(late);
    return NULL;
}

int rt_quiesce_timeout_frontier(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    char path[512];
    uint64_t h0;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    c.quiesce_timeout_us = 100000;
    rt_event_init(&g_w_ready);
    rt_event_init(&g_w_go);
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("m");
    }
    rt_spawn(&t, blocked_worker, NULL);
    CHECK_EQ(rt_event_wait(&g_w_ready, 5000), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ENOTQUIESCED);
    {
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("m-late");   /* acked owner: nothing registered */
        CHECK_EQ(z.kind, 0);
        PS3DK_PROF_ZONE_END(z);
    }
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    h0 = rt_file_hash(path);
    rt_event_set(&g_w_go);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(rt_file_hash(path), h0);   /* data published after the frontier is never drained */
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_NOT_QUIESCED);
    CHECK_EQ(f->limit, 2);
    CHECK_EQ(f->produced, RT_UNKNOWN);
    CHECK_EQ(f->dropped, RT_UNKNOWN);
    CHECK_EQ(f->refused, RT_UNKNOWN);
    CHECK_EQ(f->regerr, RT_UNKNOWN);
    CHECK_EQ(f->open_spans, RT_UNKNOWN);
    CHECK_EQ(f->written, 2);
    CHECK_EQ(cap.end_flags & 1u, 1u);
    CHECK_EQ(cap.desc_count, 2);   /* Fd did not grow */
    CHECK_EQ(cap.ndesc, 2);
    CHECK_EQ(cap.nthreads, 2);     /* Ft did not grow */
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}

/* ---- the shutdown result: SEQ_LIMIT from a thread retired before shutdown outranks a recovered sink error -------- */

static int g_eintr_once;
static int wf_eintr_once(unsigned long n, long *ret, int *err)
{
    (void)n;
    if (g_eintr_once)
        return 0;
    g_eintr_once = 1;
    *ret = -1;
    *err = EINTR;   /* retried, recovered */
    return 1;
}

static void *limit_then_retire(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("lim") == PS3DK_PROF_OK);
    RT_PRESET(NEXT_SEQ, RT_CEIL);
    ps3dk_prof_frame();   /* refused: the sequence limit */
    WCHECK(RT_READ(LIMIT) == 1);
    WCHECK(RT_READ(REFUSED) == 1);
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_shutdown_severity_order(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    char path[512];
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    ps3dk_prof_test_hooks.write_fault = wf_eintr_once;
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, limit_then_retire, NULL);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);   /* writes its RETIRED entry, with SEQ_LIMIT */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);   /* bit1 outranks bit2 */
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(cap.end_flags, 6u);
    CHECK_EQ(cap.sink_error_count, 1);
    CHECK_EQ(cap.first_error, EINTR);
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->in_retired, 1);
    CHECK_EQ(f->limit, 1);
    CHECK_EQ(f->produced, RT_CEIL);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}
