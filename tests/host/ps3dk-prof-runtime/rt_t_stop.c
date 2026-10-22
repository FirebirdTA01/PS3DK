/* The STOP batch, acks, retirement, the retirement race and the frozen frontier (format 5.1, 6, 7; E1; F2). */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

#include <string.h>

static ps3dk_prof_zone pz(void) { return PS3DK_PROF_ZONE_BEGIN("p"); }

static int load(const char *out, rt_cap *cap)
{
    char path[512];
    if (rt_capture_path(out, path, sizeof path) != 0)
        return -1;
    return rt_cap_load(path, cap);
}

/* Pause hooks fire only on threads that ask for it (the shutdown caller also passes the ack hook). */
static _Thread_local int t_pause;
static rt_event g_at_hook, g_release, g_t_done;
static void pause_here(void)
{
    if (!t_pause)
        return;
    rt_event_set(&g_at_hook);
    rt_event_wait(&g_release, 20000);
}

static void events_init(void)
{
    rt_event_init(&g_at_hook);
    rt_event_init(&g_release);
    rt_event_init(&g_t_done);
}

static int steps_are(int s1, uint64_t v1, int s2, uint64_t v2)
{
    if (g_nsteps != 2 || g_steps[0].step != s1 || g_steps[0].value != v1 || g_steps[1].step != s2 ||
        g_steps[1].value != v2) {
        int i;
        fprintf(stderr, "steps (%d):", g_nsteps);
        for (i = 0; i < g_nsteps; i++)
            fprintf(stderr, " (%d,%llu)", g_steps[i].step, (unsigned long long)g_steps[i].value);
        fprintf(stderr, "\n");
        return 1;
    }
    return 0;
}

/* ---- STOP admission (5.1) ------------------------------------------------------------------------------------------ */

int rt_stop_admit_exact_fit(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z[3];
    rt_cap cap;
    const rt_final *f;
    size_t i;
    uint64_t last = 0;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_steps_install();
    RT_PRESET(NEXT_SEQ, RT_CEIL - 9);
    for (i = 0; i < 3; i++)
        z[i] = pz();
    CHECK_EQ(RT_READ(NEXT_SEQ), RT_CEIL - 3);   /* k = 3 = 2^63 - produced */
    rt_steps_on();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_steps_off();
    CHECK_OK(steps_are(PS3DK_PROF_TEST_STEP_ASSIGN, 3, PS3DK_PROF_TEST_STEP_PUBLISH, 3));
    for (i = 3; i-- > 0;)
        PS3DK_PROF_ZONE_END(z[i]);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->produced, RT_CEIL);
    CHECK_EQ(f->limit, 0);
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].kind == RT_END)
            last = cap.recs[i].seq;
    CHECK_EQ(last, RT_CEIL - 1);
    CHECK_EQ(rt_count(&cap, 1, RT_END, 1), 3);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_stop_refuse_one_short(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z[3];
    rt_cap cap;
    const rt_final *f;
    int i;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_steps_install();
    RT_PRESET(NEXT_SEQ, RT_CEIL - 8);
    for (i = 0; i < 3; i++)
        z[i] = pz();
    CHECK_EQ(RT_READ(NEXT_SEQ), RT_CEIL - 2);   /* k = 3 = 2^63 - produced + 1 */
    rt_steps_on();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);
    rt_steps_off();
    CHECK_EQ(g_nsteps, 1);
    CHECK_EQ(g_steps[0].step, PS3DK_PROF_TEST_STEP_REFUSE);
    CHECK_EQ(g_steps[0].value, 3);
    CHECK_EQ(g_steps[0].next_seq, RT_CEIL - 2);   /* no sequence assigned */
    for (i = 3; i-- > 0;)
        PS3DK_PROF_ZONE_END(z[i]);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->produced, RT_CEIL - 2);
    CHECK_EQ(f->dropped, RT_CEIL - 8);
    CHECK_EQ(f->refused, 3);
    CHECK_EQ(f->limit, 1);
    CHECK_EQ(f->open_spans, 3);
    CHECK_EQ(rt_count(&cap, 1, RT_END, -1), 0);   /* the k spans stay OPEN */
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}

int rt_stop_refuse_prior_limit(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone p, q;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_steps_install();
    p = pz();
    RT_PRESET(NEXT_SEQ, RT_CEIL - 1);
    q = pz();                       /* a 2-slot BEGIN at 2^63 - 1: refused whole, SEQ_LIMIT set */
    CHECK_EQ(q.kind, 2);
    CHECK_EQ(RT_READ(LIMIT), 1);
    rt_steps_on();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);   /* k = 1 would fit, but the limit came first */
    rt_steps_off();
    CHECK_EQ(g_nsteps, 1);
    CHECK_EQ(g_steps[0].step, PS3DK_PROF_TEST_STEP_REFUSE);
    CHECK_EQ(g_steps[0].value, 1);
    PS3DK_PROF_ZONE_END(q);
    PS3DK_PROF_ZONE_END(p);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->refused, 2);
    CHECK_EQ(f->produced, RT_CEIL - 1);
    CHECK_EQ(f->open_spans, 1);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}

int rt_stop_k0_no_limit(const char *out)
{
    ps3dk_prof_config c;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_steps_install();
    {
        PS3DK_PROF_ZONE("z");
    }
    rt_steps_on();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_steps_off();
    CHECK_EQ(g_nsteps, 0);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->limit, 0);
    CHECK_EQ(f->refused, 0);
    CHECK_EQ(f->open_spans, 0);
    CHECK_EQ(f->produced, 3);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_stop_no_partial(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z[5];
    uint64_t s0;
    int i;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_steps_install();
    for (i = 0; i < 5; i++)
        z[i] = pz();
    s0 = RT_READ(NEXT_SEQ);
    rt_steps_on();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_steps_off();
    /* One assignment of all k (the counter moves by k at once), then one publication of all k. */
    CHECK_OK(steps_are(PS3DK_PROF_TEST_STEP_ASSIGN, 5, PS3DK_PROF_TEST_STEP_PUBLISH, 5));
    CHECK_EQ(g_steps[0].next_seq, s0 + 5);
    CHECK_EQ(g_steps[1].next_seq, s0 + 5);
    for (i = 5; i-- > 0;)
        PS3DK_PROF_ZONE_END(z[i]);
    return rt_expect(out, 0);
}

int rt_stop_admitted_ring_full_drops_all(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z[5];
    rt_cap cap;
    const rt_final *f;
    int i;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_steps_install();
    for (i = 0; i < 5; i++)
        z[i] = pz();   /* 10 slots */
    for (i = 0; i < 502; i++)
        ps3dk_prof_frame();   /* the 512-slot ring is now full */
    CHECK_EQ(RT_READ(DROPPED), 0);
    rt_steps_on();
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_steps_off();
    CHECK_OK(steps_are(PS3DK_PROF_TEST_STEP_ASSIGN, 5, PS3DK_PROF_TEST_STEP_DROP, 5));
    for (i = 5; i-- > 0;)
        PS3DK_PROF_ZONE_END(z[i]);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->dropped, 5);
    CHECK_EQ(f->produced, 517);
    CHECK_EQ(f->written, 512);
    CHECK_EQ(rt_count(&cap, 1, RT_END, -1), 0);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- the STOP batch seen from a worker ----------------------------------------------------------------------------- */

static rt_event g_w_ready;

static void *w_end_observes(void *a)
{
    ps3dk_prof_zone outer, inner;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    outer = pz();
    inner = pz();
    rt_event_set(&g_w_ready);
    WCHECK(rt_wait_latch() == 0);
    PS3DK_PROF_ZONE_END(inner);   /* the first call after the latch: the batch covers this span too */
    WCHECK(RT_READ(REFUSED) == 0);
    WCHECK(RT_READ(OPEN_SPANS) == 2);
    WCHECK(ps3dk_prof_thread_begin("again") == PS3DK_PROF_ESTOPPED);   /* acked */
    PS3DK_PROF_ZONE_END(outer);
    return NULL;
}

static int run_worker_case(const char *out, void *(*fn)(void *), rt_cap *cap)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_config(&c);
    c.quiesce_timeout_us = 5000000;
    rt_event_init(&g_w_ready);
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, fn, NULL);
    CHECK_EQ(rt_event_wait(&g_w_ready, 5000), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(load(out, cap), 0);
    return 0;
}

int rt_stop_batch_truncates_observing_end(const char *out)
{
    rt_cap cap;
    size_t i, n = 0;
    uint64_t ids[2] = {0, 0};
    CHECK_OK(run_worker_case(out, w_end_observes, &cap));
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].inc == 2 && cap.recs[i].kind == RT_END) {
            CHECK_EQ(cap.recs[i].flags, 1);   /* TRUNCATED_AT_STOP */
            if (n < 2)
                ids[n] = cap.recs[i].c;
            n++;
        }
    CHECK_EQ(n, 2);
    CHECK_EQ(ids[0], 2);   /* innermost first: the span whose END observed the latch */
    CHECK_EQ(ids[1], 1);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_end_observing_stop_runs_stop_batch_first(const char *out)
{
    rt_cap cap;
    const rt_final *f;
    CHECK_OK(run_worker_case(out, w_end_observes, &cap));
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_ACKED);
    CHECK_EQ(f->refused, 0);   /* no mismatch counted */
    CHECK_EQ(rt_count(&cap, 2, RT_END, 1), 2);   /* exactly one batch */
    CHECK_EQ(rt_count(&cap, 2, RT_END, 0), 0);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static void *w_ack_then_end(void *a)
{
    ps3dk_prof_zone h;
    uint64_t s, r, d, o;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    h = pz();
    rt_event_set(&g_w_ready);
    WCHECK(rt_wait_latch() == 0);
    ps3dk_prof_frame();   /* acks through another entry point */
    s = RT_READ(NEXT_SEQ);
    r = RT_READ(REFUSED);
    d = RT_READ(DROPPED);
    o = RT_READ(OPEN_SPANS);
    PS3DK_PROF_ZONE_END(h);
    WCHECK(h.kind == 3);   /* SPENT */
    WCHECK(RT_READ(NEXT_SEQ) == s && RT_READ(REFUSED) == r && RT_READ(DROPPED) == d && RT_READ(OPEN_SPANS) == o);
    WCHECK(o == 1);
    return NULL;
}

int rt_end_after_ack_changes_nothing(const char *out)
{
    rt_cap cap;
    const rt_final *f;
    CHECK_OK(run_worker_case(out, w_ack_then_end, &cap));
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_ACKED);
    CHECK_EQ(f->open_spans, 1);
    CHECK_EQ(rt_count(&cap, 2, RT_END, 1), 1);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static void *w_thread_end_observes(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    (void)pz();
    (void)pz();
    rt_event_set(&g_w_ready);
    WCHECK(rt_wait_latch() == 0);
    ps3dk_prof_thread_end();   /* the first call after the latch: STOP batch and ack, ends ACKED */
    WCHECK(ps3dk_prof_thread_begin("again") == PS3DK_PROF_ESTOPPED);
    return NULL;
}

int rt_thread_end_observes_stop_acks(const char *out)
{
    rt_cap cap;
    const rt_final *f;
    CHECK_OK(run_worker_case(out, w_thread_end_observes, &cap));
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_ACKED);
    CHECK_EQ(f->in_retired, 0);
    CHECK_EQ(f->open_spans, 2);
    CHECK_EQ(rt_count(&cap, 2, RT_END, 1), 2);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static void *w_ack_then_thread_end(void *a)
{
    uint64_t s, r;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    (void)pz();
    rt_event_set(&g_w_ready);
    WCHECK(rt_wait_latch() == 0);
    ps3dk_prof_frame();
    s = RT_READ(NEXT_SEQ);
    r = RT_READ(REFUSED);
    ps3dk_prof_thread_end();   /* refused silently: stays ACKED */
    WCHECK(RT_READ(NEXT_SEQ) == s && RT_READ(REFUSED) == r);
    WCHECK(ps3dk_prof_thread_begin("again") == PS3DK_PROF_ESTOPPED);
    return NULL;
}

int rt_thread_end_after_ack_refused(const char *out)
{
    rt_cap cap;
    const rt_final *f;
    CHECK_OK(run_worker_case(out, w_ack_then_thread_end, &cap));
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_ACKED);
    CHECK_EQ(f->in_retired, 0);
    CHECK_EQ(cap.nretired_chunks, 0);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static void *w_hammer_after_ack(void *a)
{
    uint64_t s, r, d;
    int i;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    (void)pz();
    rt_event_set(&g_w_ready);
    WCHECK(rt_wait_latch() == 0);
    ps3dk_prof_frame();
    s = RT_READ(NEXT_SEQ);
    r = RT_READ(REFUSED);
    d = RT_READ(DROPPED);
    for (i = 0; i < 20000; i++) {   /* runs while the drainer seals: TSan sees any shared write */
        ps3dk_prof_zone z = pz();
        PS3DK_PROF_ZONE_END(z);
        ps3dk_prof_frame();
        (void)ps3dk_prof_frame_token();
        ps3dk_prof_thread_end();
    }
    WCHECK(RT_READ(NEXT_SEQ) == s && RT_READ(REFUSED) == r && RT_READ(DROPPED) == d);
    return NULL;
}

int rt_after_ack_immutable(const char *out)
{
    rt_cap cap;
    CHECK_OK(run_worker_case(out, w_hammer_after_ack, &cap));
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- retirement before STOP (7) ------------------------------------------------------------------------------------- */

static void *w_retire_open(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    (void)pz();
    (void)pz();
    ps3dk_prof_thread_end();   /* two spans open: no END is synthesised */
    return NULL;
}

int rt_retire_open_spans_no_end(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, w_retire_open, NULL);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->in_retired, 1);
    CHECK_EQ(f->open_spans, 2);
    CHECK_EQ(rt_count(&cap, 2, RT_END, -1), 0);
    CHECK_EQ(rt_count(&cap, 2, RT_BEGIN, -1), 2);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- pauses at the final word (r2 10.1, E1) ------------------------------------------------------------------------ */

static void *w_pause_at_ack(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    (void)pz();
    t_pause = 1;
    rt_event_set(&g_w_ready);
    WCHECK(rt_wait_latch() == 0);
    ps3dk_prof_frame();   /* STOP batch, then the pause just before the ack store */
    WCHECK(RT_READ(DEPTH) == 0);        /* released: an empty stack, */
    WCHECK(RT_READ(OPEN_SPANS) == 1);   /* final counters */
    WCHECK(ps3dk_prof_thread_begin("again") == PS3DK_PROF_ESTOPPED);
    return NULL;
}

int rt_pause_at_ack(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    char path[512];
    uint64_t h0;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    c.quiesce_timeout_us = 100000;
    events_init();
    rt_event_init(&g_w_ready);
    ps3dk_prof_test_hooks.before_ack_store = pause_here;
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, w_pause_at_ack, NULL);
    CHECK_EQ(rt_event_wait(&g_w_ready, 5000), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ENOTQUIESCED);   /* stalled before its ack: never ACKED */
    CHECK_EQ(rt_event_wait(&g_at_hook, 5000), 0);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    h0 = rt_file_hash(path);
    rt_event_set(&g_release);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(rt_file_hash(path), h0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_NOT_QUIESCED);
    CHECK_EQ(f->produced, RT_UNKNOWN);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}

static void *w_pause_at_retire(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("w") == PS3DK_PROF_OK);
    (void)pz();
    t_pause = 1;
    rt_event_set(&g_w_ready);
    ps3dk_prof_thread_end();   /* pauses just before the RETIRED store */
    rt_event_set(&g_t_done);
    return NULL;
}

static void *release_after_latch(void *a)
{
    (void)a;
    WCHECK(rt_wait_latch() == 0);
    rt_event_set(&g_release);
    return NULL;
}

int rt_pause_at_retire(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t, r;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    c.quiesce_timeout_us = 5000000;
    events_init();
    rt_event_init(&g_w_ready);
    ps3dk_prof_test_hooks.before_retire_store = pause_here;
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, w_pause_at_retire, NULL);
    CHECK_EQ(rt_event_wait(&g_at_hook, 5000), 0);
    CHECK_EQ(RT_STAT(SLOT_STATE_BASE + 1), RT_SLOT_RETIRING);
    rt_spawn(&r, release_after_latch, NULL);   /* released during the quiescence wait */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_join(&r);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_RETIRED);
    CHECK_EQ(f->in_retired, 1);
    CHECK_EQ(f->open_spans, 1);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- the retirement race (E1) ------------------------------------------------------------------------------------- */

static void *w_retire_paused(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("t") == PS3DK_PROF_OK);
    {
        PS3DK_PROF_ZONE("t");
    }
    t_pause = 1;
    ps3dk_prof_thread_end();
    rt_event_set(&g_t_done);
    return NULL;
}

/* T paused (at the hook the test installs) while shutdown latches, waits 100 ms and takes the frontier. */
static int retire_race_timeout(const char *out, int live_to_retiring_hook, uint64_t *hash_ok)
{
    ps3dk_prof_config c;
    rt_thread t;
    char path[512];
    uint64_t h0;
    rt_config(&c);
    c.quiesce_timeout_us = 100000;
    events_init();
    if (live_to_retiring_hook)
        ps3dk_prof_test_hooks.before_live_to_retiring = pause_here;
    else
        ps3dk_prof_test_hooks.before_retire_store = pause_here;
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, w_retire_paused, NULL);
    CHECK_EQ(rt_event_wait(&g_at_hook, 5000), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ENOTQUIESCED);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    h0 = rt_file_hash(path);
    rt_event_set(&g_release);   /* T publishes RETIRED after the frontier */
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(rt_file_hash(path), h0);
    *hash_ok = 1;
    return 0;
}

static int check_t_not_quiesced(const char *out)
{
    rt_cap cap;
    const rt_final *f;
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_NOT_QUIESCED);
    CHECK_EQ(f->in_retired, 0);
    CHECK_EQ(cap.nretired_chunks, 0);
    CHECK_EQ(cap.end_flags & 1u, 1u);
    rt_cap_free(&cap);
    return 0;
}

int rt_retire_race_pause_before_live_to_retiring(const char *out)
{
    uint64_t ok = 0;
    CHECK_OK(retire_race_timeout(out, 1, &ok));
    CHECK_OK(check_t_not_quiesced(out));
    CHECK_EQ(RT_STAT(SLOT_STATE_BASE + 1), RT_SLOT_RETIRING);   /* LIVE-to-RETIRING ran after the latch (E1) */
    return rt_expect(out, 1);
}

int rt_retire_race_pause_before_retire_store_after_latch(const char *out)
{
    uint64_t ok = 0;
    CHECK_OK(retire_race_timeout(out, 0, &ok));
    CHECK_OK(check_t_not_quiesced(out));
    return rt_expect(out, 1);
}

int rt_frozen_not_quiesced_is_permanent(const char *out)
{
    uint64_t ok = 0;
    CHECK_OK(retire_race_timeout(out, 0, &ok));
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_ESTOPPED);
    CHECK_EQ(RT_STAT(FROZEN), 1);
    CHECK_EQ(RT_STAT(POST_FRONTIER_TRANSITIONS), 0);
    CHECK_EQ(RT_STAT(ORDINARY_DRAINS_AFTER_FRONTIER), 0);
    CHECK_EQ(RT_STAT(SLOT_STATE_BASE + 1), RT_SLOT_RETIRING);   /* its RETIRED word is never processed */
    CHECK_OK(check_t_not_quiesced(out));
    return rt_expect(out, 1);
}

static void release_and_wait_for_t(void)
{
    rt_event_set(&g_release);
    rt_event_wait(&g_t_done, 10000);
}

int rt_retire_race_resume_before_frontier(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    c.quiesce_timeout_us = 100000;
    events_init();
    ps3dk_prof_test_hooks.before_live_to_retiring = pause_here;
    ps3dk_prof_test_hooks.before_frontier = release_and_wait_for_t;   /* T retires before frontier step 1 */
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, w_retire_paused, NULL);
    CHECK_EQ(rt_event_wait(&g_at_hook, 5000), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 2);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_RETIRED);
    CHECK_EQ(f->in_retired, 1);   /* written by the FINAL drain from the frozen classification */
    CHECK_EQ(RT_STAT(SLOT_STATE_BASE + 1), RT_SLOT_RETIRING);   /* the slot was NOT freed */
    CHECK_EQ(RT_STAT(POST_FRONTIER_TRANSITIONS), 0);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- frames and tokens (5.6) ---------------------------------------------------------------------------------------- */

static ps3dk_prof_token g_tok;
static rt_event g_tok_ready;

static void *owner_frames(void *a)
{
    ps3dk_prof_token t;
    int i;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("owner") == PS3DK_PROF_OK);
    t = ps3dk_prof_frame_token();
    WCHECK(t.owner == 0 && t.frame == 0);   /* NONE before the first frame */
    ps3dk_prof_frame();
    t = ps3dk_prof_frame_token();
    WCHECK(t.owner == 2 && t.frame == 0);
    for (i = 0; i < 600; i++)
        ps3dk_prof_frame();   /* the ring fills: the later FRAMEs are dropped but still take ids */
    WCHECK(RT_READ(DROPPED) > 0);
    g_tok = ps3dk_prof_frame_token();
    WCHECK(g_tok.frame == 600);
    rt_event_set(&g_tok_ready);
    ps3dk_prof_thread_end();
    return NULL;
}

static void *token_worker(void *a)
{
    ps3dk_prof_zone z;
    (void)a;
    WCHECK(rt_event_wait(&g_tok_ready, 5000) == 0);
    WCHECK(ps3dk_prof_thread_begin("worker") == PS3DK_PROF_OK);
    z = PS3DK_PROF_ZONE_BEGIN_FOR("tok", g_tok);
    PS3DK_PROF_ZONE_END(z);
    ps3dk_prof_thread_end();
    return NULL;
}

static void *refused_frame_owner(void *a)
{
    ps3dk_prof_token t;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("owner2") == PS3DK_PROF_OK);
    RT_PRESET(LIMIT, 1);
    ps3dk_prof_frame();   /* refused: no id, current_frame unchanged */
    t = ps3dk_prof_frame_token();
    WCHECK(t.owner == 0);
    WCHECK(RT_READ(REFUSED) == 1);
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_frame_tokens(const char *out)
{
    ps3dk_prof_config c;
    rt_thread a, b, d;
    rt_cap cap;
    size_t i, found = 0;
    rt_config(&c);
    rt_event_init(&g_tok_ready);
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&a, owner_frames, NULL);
    rt_join(&a);
    rt_spawn(&b, token_worker, NULL);
    rt_join(&b);
    rt_spawn(&d, refused_frame_owner, NULL);
    rt_join(&d);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);   /* owner2's preset limit */
    CHECK_EQ(load(out, &cap), 0);
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].inc == 3 && cap.recs[i].kind == RT_BEGIN_PARENT) {
            CHECK_EQ(cap.recs[i].flags, 1);
            CHECK_EQ(cap.recs[i].a, 2);
            CHECK_EQ(cap.recs[i].c, 600);   /* a dropped FRAME's id: UNRESOLVED-LOSS in the reader */
            found++;
        }
    CHECK_EQ(found, 1);
    CHECK_EQ(rt_count(&cap, 4, RT_FRAME, -1), 0);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}
