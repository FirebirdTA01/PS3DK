/* The sink (format 8, r2 8 / B5) and publication boundaries and chunking (r2 7 / Q9). */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

#include <string.h>

static char g_path[600];
static const char *cb_path(const char *out)
{
    snprintf(g_path, sizeof g_path, "%s/cb.prof", out);
    return g_path;
}

static ps3dk_prof_zone pz(void) { return PS3DK_PROF_ZONE_BEGIN("p"); }

/* ---- file sink faults ----------------------------------------------------------------------------------------------- */

static unsigned g_calls, g_zero_calls;
static int g_zero_mode;
static int wf_short_then_zero(unsigned long n, long *ret, int *err)
{
    (void)err;
    g_calls++;
    if (g_zero_mode) {
        g_zero_calls++;
        *ret = 0;   /* no progress */
        return 1;
    }
    if (g_calls <= 64) {
        *ret = n > 1 ? 1 : (long)n;   /* a short write of one byte: progress, retried for the rest */
        return 1;
    }
    return 0;
}

int rt_sink_short_write_limit(const char *out)
{
    ps3dk_prof_config c;
    char path[512];
    rt_config(&c);
    ps3dk_prof_test_hooks.write_fault = wf_short_then_zero;
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);   /* the first 64 writes were 1-byte short writes */
    CHECK(g_calls > 64);
    g_zero_mode = 1;
    ps3dk_prof_frame();
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_ESINKFAIL);
    CHECK_EQ(g_zero_calls, 16);   /* 16 tries in a row without progress */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESINKFAIL);
    CHECK_EQ(g_zero_calls, 16);   /* nothing more is written after a failure */
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    rt_expect_path(path, 1);      /* no trailer */
    return 0;
}

int rt_sink_close_failure_fails_run(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    ps3dk_prof_test_hooks.close_fail = 5;   /* EIO */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ECLOSE);   /* the trailer exists, yet the run FAILS */
    return rt_expect(out, 0);
}

/* ---- callback sink -------------------------------------------------------------------------------------------------- */

int rt_sink_callback_negative(const char *out)
{
    ps3dk_prof_config c;
    unsigned w;
    rt_config(&c);
    CHECK_EQ(rt_use_mem_sink(0), 0);
    g_mem.neg_after = 5;   /* magic, HEADER, CLOCKS, THREADS, then DESCRIPTORS fails */
    CHECK_OK(rt_start(NULL, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_ESINKFAIL);
    w = g_mem.writes;
    CHECK_EQ(w, 5);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESINKFAIL);
    CHECK_EQ(g_mem.writes, w);   /* never called again */
    CHECK_EQ(g_mem.closes, 1);
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    rt_expect_path(cb_path(out), 1);
    return 0;
}

int rt_callback_count_over_n_fails(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_use_mem_sink(0), 0);
    g_mem.over_n = 1;
    CHECK_OK(rt_start(NULL, &c, 1));
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_ESINKFAIL);
    CHECK_EQ(g_mem.writes, 1);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESINKFAIL);
    CHECK_EQ(g_mem.writes, 1);
    CHECK_EQ(g_mem.closes, 1);
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    rt_expect_path(cb_path(out), 1);   /* an empty file */
    return 0;
}

int rt_callback_flush_close_order_and_errors(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_EQ(rt_use_mem_sink(1), 0);
    g_mem.flush_ret = -1;
    CHECK_OK(rt_start(NULL, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESINKFAIL);   /* a flush failure is a sink failure before close */
    CHECK_EQ(strcmp(g_mem.order, "wfc"), 0);                  /* writes, then flush once, then close once */
    CHECK_EQ(g_mem.flushes, 1);
    CHECK_EQ(g_mem.closes, 1);
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    rt_expect_path(cb_path(out), 0);   /* the bytes are complete; the run result still fails */
    return 0;
}

int rt_blocking_callback_holds_shutdown(const char *out)
{
    ps3dk_prof_config c;
    uint64_t t0;
    rt_config(&c);
    c.quiesce_timeout_us = 10000;
    CHECK_EQ(rt_use_mem_sink(0), 0);
    g_mem.block_ms = 300;   /* every write after the latch blocks */
    CHECK_OK(rt_start(NULL, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    t0 = rt_now_ms();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK(rt_now_ms() - t0 >= 300);   /* not bounded by quiesce_timeout_us (B5) */
    CHECK_EQ(rt_mem_dump(cb_path(out)), 0);
    rt_expect_path(cb_path(out), 0);
    return 0;
}

/* ---- publications and chunks ---------------------------------------------------------------------------------------- */

int rt_publication_boundaries_gap_event_stop_batch(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z[5];
    char path[512];
    rt_cap cap;
    size_t i, k;
    int found = 0;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_steps_install();
    for (i = 0; i < 170; i++) {
        ps3dk_prof_zone h = pz();
        PS3DK_PROF_ZONE_END(h);
    }
    ps3dk_prof_frame();
    ps3dk_prof_frame();   /* 512 slots: full */
    rt_steps_on();
    ps3dk_prof_frame();   /* dropped */
    rt_steps_off();
    CHECK_EQ(g_nsteps, 2);
    CHECK_EQ(g_steps[1].step, PS3DK_PROF_TEST_STEP_DROP);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    rt_steps_on();
    ps3dk_prof_frame();   /* GAP + FRAME: one publication of 2 */
    rt_steps_off();
    CHECK_EQ(g_nsteps, 2);
    CHECK_EQ(g_steps[1].step, PS3DK_PROF_TEST_STEP_PUBLISH);
    CHECK_EQ(g_steps[1].value, 2);
    for (i = 0; i < 5; i++)
        z[i] = pz();
    for (i = 0; i < 500; i++)
        ps3dk_prof_frame();   /* full again */
    ps3dk_prof_frame();       /* dropped: a pending GAP */
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    rt_steps_on();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_steps_off();
    CHECK_EQ(g_nsteps, 2);
    CHECK_EQ(g_steps[0].step, PS3DK_PROF_TEST_STEP_ASSIGN);
    CHECK_EQ(g_steps[0].value, 5);
    CHECK_EQ(g_steps[1].step, PS3DK_PROF_TEST_STEP_PUBLISH);
    CHECK_EQ(g_steps[1].value, 6);   /* GAP + the whole STOP batch */
    for (i = 5; i-- > 0;)
        PS3DK_PROF_ZONE_END(z[i]);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    for (i = 0; i + 5 < cap.nrecs; i++) {
        if (cap.recs[i].kind != RT_GAP || cap.recs[i + 1].kind != RT_END)
            continue;
        for (k = 1; k <= 5; k++) {
            CHECK_EQ(cap.recs[i + k].kind, RT_END);
            CHECK_EQ(cap.recs[i + k].flags, 1);
            CHECK_EQ(cap.recs[i + k].chunk, cap.recs[i].chunk);   /* never split */
        }
        found++;
    }
    CHECK_EQ(found, 1);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_chunk_split_never_mid_publication(const char *out)
{
    ps3dk_prof_config c;
    char path[512];
    rt_cap cap;
    size_t i;
    rt_config(&c);
    c.max_threads = 2;
    c.ring_records = 65536;
    CHECK_OK(rt_start(out, &c, 1));
    for (i = 0; i < 20000; i++) {
        ps3dk_prof_zone h = pz();   /* publications of 2, 1, 2, 1, ... */
        PS3DK_PROF_ZONE_END(h);
    }
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK(cap.nrecchunks >= 2);
    CHECK_EQ(cap.recchunk_n[0], 32766);   /* 32767 would split a BEGIN pair */
    for (i = 1; i < cap.nrecs; i++)
        if (cap.recs[i].chunk != cap.recs[i - 1].chunk)
            CHECK(cap.recs[i].kind != RT_BEGIN_PARENT);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_pub_len_corruption_detected(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    {
        PS3DK_PROF_ZONE("z");
    }
    ps3dk_prof_test_hooks.corrupt_next_pub_len = 1;
    ps3dk_prof_frame();
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_ESINKFAIL);   /* detected: fails closed, never written */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESINKFAIL);
    return rt_expect(out, 1);
}
