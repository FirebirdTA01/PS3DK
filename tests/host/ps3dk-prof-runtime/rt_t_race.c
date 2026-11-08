/* Concurrency and ownership: descriptors, slots, the latch, the frontier, late calls (5.3, 5.4, 7; r2 3.1; Q3, Q5, Q6,
 * Q10).  These are the tests TSan matters for. */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

#include <string.h>

static int load(const char *out, rt_cap *cap)
{
    char path[512];
    if (rt_capture_path(out, path, sizeof path) != 0)
        return -1;
    return rt_cap_load(path, cap);
}

static ps3dk_prof_zone pz(void) { return PS3DK_PROF_ZONE_BEGIN("p"); }

static rt_event g_e1, g_e2, g_e3;

/* ---- descriptor registration (Q5) ----------------------------------------------------------------------------------- */

static ps3dk_prof_desc g_shared;
static int g_hook_calls;
static rt_event g_go;

static void slow_publish(void)
{
    __atomic_add_fetch(&g_hook_calls, 1, __ATOMIC_SEQ_CST);
    rt_sleep_ms(50);   /* widen the window between the entry and its publication */
}

static void *racer(void *a)
{
    ps3dk_prof_zone z;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("r") == PS3DK_PROF_OK);
    WCHECK(rt_event_wait(&g_go, 5000) == 0);
    z = ps3dk_prof_zone_begin(&g_shared, PS3DK_PROF_TOKEN_NONE);
    WCHECK(z.kind == 1);
    ps3dk_prof_zone_end(&z);
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_desc_first_use_race(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t[4];
    rt_cap cap;
    size_t i;
    int k;
    rt_config(&c);
    rt_desc(&g_shared, "shared", 7);
    rt_event_init(&g_go);
    ps3dk_prof_test_hooks.between_desc_entry_and_publish = slow_publish;
    CHECK_OK(rt_start(out, &c, 0));
    for (k = 0; k < 4; k++)
        rt_spawn(&t[k], racer, NULL);
    rt_sleep_ms(20);
    rt_event_set(&g_go);
    for (k = 0; k < 4; k++)
        rt_join(&t[k]);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ALD(g_hook_calls), 1);   /* the re-check inside desc_mutex: registered once */
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(cap.ndesc, 1);
    CHECK_EQ(rt_count(&cap, 0, RT_BEGIN, -1), 4);
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].kind == RT_BEGIN)
            CHECK_EQ(cap.recs[i].a, 0);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static ps3dk_prof_desc g_d1;
static void pause_in_registration(void)
{
    rt_event_set(&g_e1);
    rt_event_wait(&g_e2, 10000);
}

static void *registrant(void *a)
{
    ps3dk_prof_zone z;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("reg") == PS3DK_PROF_OK);
    z = ps3dk_prof_zone_begin(&g_d1, PS3DK_PROF_TOKEN_NONE);   /* pauses inside desc_mutex */
    ps3dk_prof_zone_end(&z);
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_desc_entry_complete_before_publish(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_cap cap;
    rt_config(&c);
    rt_desc(&g_d1, "d1", 11);
    rt_event_init(&g_e1);
    rt_event_init(&g_e2);
    ps3dk_prof_test_hooks.between_desc_entry_and_publish = pause_in_registration;
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&t, registrant, NULL);
    CHECK_EQ(rt_event_wait(&g_e1, 5000), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);   /* the drainer must not see the unpublished count */
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(cap.ndesc, 0);
    CHECK_EQ(cap.nthreads, 1);
    rt_cap_free(&cap);
    rt_event_set(&g_e2);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(cap.ndesc, 1);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- every cold path at once, for the TSan lock-order detector (Q5) ------------------------------------------------- */

static ps3dk_prof_desc g_many[32];
static int g_stop_churn, g_exited;

static void *churn(void *a)
{
    int w = (int)(intptr_t)a, i = 0, rc;
    for (;;) {
        ps3dk_prof_zone z;
        rc = ps3dk_prof_thread_begin("churn");
        if (rc == PS3DK_PROF_ESTOPPED)
            break;
        if (rc == PS3DK_PROF_EFULL) {   /* every slot retired but not yet drained */
            rt_sleep_ms(1);
            continue;
        }
        WCHECK(rc == PS3DK_PROF_OK);
        z = ps3dk_prof_zone_begin(&g_many[(i++ * 3 + w) % 32], PS3DK_PROF_TOKEN_NONE);
        ps3dk_prof_frame();
        ps3dk_prof_zone_end(&z);
        ps3dk_prof_thread_end();
        if (ALD(g_stop_churn) && i > 50)
            break;
    }
    __atomic_add_fetch(&g_exited, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

int rt_lock_order_no_inversion(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t[3];
    int k;
    uint64_t end;
    rt_config(&c);
    for (k = 0; k < 32; k++)
        rt_desc(&g_many[k], "many", (uint32_t)(100 + k));
    CHECK_OK(rt_start(out, &c, 0));
    for (k = 0; k < 3; k++)
        rt_spawn(&t[k], churn, (void *)(intptr_t)k);
    end = rt_now_ms() + 300;
    while (rt_now_ms() < end)
        CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    AST(g_stop_churn, 1);
    while (ALD(g_exited) < 3) {   /* keep draining so EFULL clears and the churners can leave */
        CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
        rt_sleep_ms(1);
    }
    for (k = 0; k < 3; k++)
        rt_join(&t[k]);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}

/* ---- the frontier racing registration (B1) -------------------------------------------------------------------------- */

static void *churn_until_stop(void *a)
{
    int i = (int)(intptr_t)a, rc;
    for (;;) {
        ps3dk_prof_zone z;
        rc = ps3dk_prof_thread_begin("fr");
        if (rc == PS3DK_PROF_ESTOPPED)
            break;   /* the latch, or this thread's own ack */
        if (rc == PS3DK_PROF_EFULL) {
            rt_sleep_ms(1);
            continue;
        }
        WCHECK(rc == PS3DK_PROF_OK);
        z = ps3dk_prof_zone_begin(&g_many[i++ % 32], PS3DK_PROF_TOKEN_NONE);   /* first uses race Fd */
        ps3dk_prof_zone_end(&z);
        ps3dk_prof_thread_end();
    }
    return NULL;
}

int rt_frontier_vs_registration_race(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t[2];
    int k;
    uint64_t end;
    rt_config(&c);
    for (k = 0; k < 32; k++)
        rt_desc(&g_many[k], "fr", (uint32_t)(200 + k));
    CHECK_OK(rt_start(out, &c, 0));
    for (k = 0; k < 2; k++)
        rt_spawn(&t[k], churn_until_stop, (void *)(intptr_t)(k * 16));
    end = rt_now_ms() + 50;
    while (rt_now_ms() < end)
        CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    for (k = 0; k < 2; k++)
        rt_join(&t[k]);
    CHECK_EQ(rt_worker_failed(), 0);
    return rt_expect(out, 0);
}

/* ---- slot reuse against the drainer's snapshot (B1) ----------------------------------------------------------------- */

static int g_rc_snap = 99, g_rc_free = 99;
static void *try_register(void *a)
{
    *(int *)a = ps3dk_prof_thread_begin("B");
    return NULL;
}
static void hook_after_snapshot(void)
{
    rt_thread t;
    rt_spawn(&t, try_register, &g_rc_snap);
    rt_join(&t);
}
static void hook_before_free(void)
{
    rt_thread t;
    rt_spawn(&t, try_register, &g_rc_free);
    rt_join(&t);
}

static void *retire_one(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("A") == PS3DK_PROF_OK);
    {
        PS3DK_PROF_ZONE("a");
    }
    ps3dk_prof_thread_end();
    return NULL;
}

static void *register_after(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("B") == PS3DK_PROF_OK);
    WCHECK(RT_READ(GENERATION) == 2);
    {
        PS3DK_PROF_ZONE("b");
    }
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_reuse_race_drainer_vs_register(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_config(&c);
    c.max_threads = 1;
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&t, retire_one, NULL);
    rt_join(&t);
    ps3dk_prof_test_hooks.drainer_after_snapshot = hook_after_snapshot;
    ps3dk_prof_test_hooks.drainer_before_free = hook_before_free;
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(g_rc_snap, PS3DK_PROF_EFULL);   /* the snapshot holds the slot: no reset */
    CHECK_EQ(g_rc_free, PS3DK_PROF_EFULL);   /* still held until the RETIRED entry is written and it is freed */
    ps3dk_prof_test_hooks.drainer_after_snapshot = NULL;
    ps3dk_prof_test_hooks.drainer_before_free = NULL;
    rt_spawn(&t, register_after, NULL);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}

/* ---- adversarial drain ordering: registrations, descriptors and records land between snapshot steps --------------- */

static ps3dk_prof_desc g_adv[16];
static int g_adv_i;
static void *adv_step(void *a)
{
    ps3dk_prof_zone z;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("adv") == PS3DK_PROF_OK);
    z = ps3dk_prof_zone_begin(&g_adv[g_adv_i % 16], PS3DK_PROF_TOKEN_NONE);
    ps3dk_prof_frame();
    ps3dk_prof_zone_end(&z);
    ps3dk_prof_thread_end();
    return NULL;
}
static void hook_adv(void)
{
    rt_thread t;
    rt_spawn(&t, adv_step, NULL);
    rt_join(&t);
    g_adv_i++;
}

static void *steady_producer(void *a)
{
    int i;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("steady") == PS3DK_PROF_OK);
    for (i = 0; i < 2000; i++) {
        ps3dk_prof_zone z = pz();
        PS3DK_PROF_ZONE_END(z);
        if (i % 100 == 0)
            rt_sleep_ms(1);
    }
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_drain_ordering_adversarial(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    int k;
    rt_config(&c);
    for (k = 0; k < 16; k++)
        rt_desc(&g_adv[k], "adv", (uint32_t)(300 + k));
    ps3dk_prof_test_hooks.drainer_after_snapshot = hook_adv;
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&t, steady_producer, NULL);
    for (k = 0; k < 12; k++) {
        CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
        rt_sleep_ms(2);
    }
    rt_join(&t);
    ps3dk_prof_test_hooks.drainer_after_snapshot = NULL;
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);   /* the reader enforces declaration before use */
}

/* ---- latch visibility (Q3) ------------------------------------------------------------------------------------------ */

static void *spin_until_acked(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("spin") == PS3DK_PROF_OK);
    rt_event_set(&g_e1);
    while (ps3dk_prof_thread_begin("x") == PS3DK_PROF_EALREADY) {   /* ESTOPPED once acked */
        ps3dk_prof_zone z = pz();
        PS3DK_PROF_ZONE_END(z);
    }
    return NULL;
}

int rt_latch_acquire_visibility(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t[4];
    rt_cap cap;
    uint32_t i;
    int k;
    rt_config(&c);
    c.quiesce_timeout_us = 5000000;
    rt_event_init(&g_e1);
    CHECK_OK(rt_start(out, &c, 1));
    for (k = 0; k < 4; k++) {
        rt_event_reset(&g_e1);
        rt_spawn(&t[k], spin_until_acked, NULL);
        CHECK_EQ(rt_event_wait(&g_e1, 5000), 0);
    }
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);   /* every spinner saw the latch and acked */
    for (k = 0; k < 4; k++)
        rt_join(&t[k]);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(cap.nfinal, 5);
    for (i = 0; i < cap.nfinal; i++)
        CHECK_EQ(cap.finals[i].state, RT_ACKED);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- calls after SEALED (r2 10.1) ----------------------------------------------------------------------------------- */

static rt_event g_sealed, g_n_ready, g_r_ready, g_a_ready;

static int all_calls(void)
{
    ps3dk_prof_zone z = pz();
    ps3dk_prof_token t;
    PS3DK_PROF_ZONE_END(z);
    ps3dk_prof_frame();
    t = ps3dk_prof_frame_token();
    ps3dk_prof_thread_end();
    if (t.owner != 0 || z.kind != 0)
        return 1;   /* INERT on every thread here */
    if (ps3dk_prof_thread_begin("again") != PS3DK_PROF_ESTOPPED)
        return 2;
    if (ps3dk_prof_drain() != PS3DK_PROF_EPERM)
        return 3;
    if (ps3dk_prof_shutdown() != PS3DK_PROF_EPERM)
        return 4;
    if (ps3dk_prof_init(NULL) != PS3DK_PROF_EALREADY)
        return 5;
    return 0;
}

static void *late_acked(void *a)
{
    uint64_t s, r;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("acked") == PS3DK_PROF_OK);
    (void)pz();
    rt_event_set(&g_a_ready);
    WCHECK(rt_wait_latch() == 0);
    ps3dk_prof_frame();   /* acks */
    WCHECK(rt_event_wait(&g_sealed, 10000) == 0);
    s = RT_READ(NEXT_SEQ);
    r = RT_READ(REFUSED);
    WCHECK(all_calls() == 0);
    WCHECK(RT_READ(NEXT_SEQ) == s && RT_READ(REFUSED) == r);
    return NULL;
}

static void *late_retired(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("retired") == PS3DK_PROF_OK);
    (void)pz();
    ps3dk_prof_thread_end();
    rt_event_set(&g_r_ready);
    WCHECK(rt_event_wait(&g_sealed, 10000) == 0);
    WCHECK(all_calls() == 0);
    WCHECK(RT_READ(NEXT_SEQ) == RT_UNKNOWN);   /* a retired thread never reads its old slot */
    return NULL;
}

static void *late_not_quiesced(void *a)
{
    ps3dk_prof_zone h;
    uint64_t s, r;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("nq") == PS3DK_PROF_OK);
    h = pz();
    rt_event_set(&g_n_ready);
    WCHECK(rt_event_wait(&g_sealed, 10000) == 0);
    PS3DK_PROF_ZONE_END(h);   /* the specified late change: its STOP batch into its own ring, then its ack */
    s = RT_READ(NEXT_SEQ);
    r = RT_READ(REFUSED);
    WCHECK(RT_READ(OPEN_SPANS) == 1);
    WCHECK(all_calls() == 0);
    WCHECK(RT_READ(NEXT_SEQ) == s && RT_READ(REFUSED) == r);   /* nothing after the ack */
    return NULL;
}

static void *late_unregistered(void *a)
{
    (void)a;
    WCHECK(rt_event_wait(&g_sealed, 10000) == 0);
    WCHECK(all_calls() == 0);
    WCHECK(RT_READ(SLOT) == 0);
    return NULL;
}

int rt_calls_after_sealed(const char *out)
{
    ps3dk_prof_config c;
    rt_thread ta, tr, tn, tu;
    char path[512];
    uint64_t h0;
    rt_config(&c);
    c.quiesce_timeout_us = 200000;
    rt_event_init(&g_sealed);
    rt_event_init(&g_n_ready);
    rt_event_init(&g_r_ready);
    rt_event_init(&g_a_ready);
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&ta, late_acked, NULL);
    rt_spawn(&tr, late_retired, NULL);
    rt_spawn(&tn, late_not_quiesced, NULL);
    rt_spawn(&tu, late_unregistered, NULL);
    CHECK_EQ(rt_event_wait(&g_a_ready, 5000), 0);
    CHECK_EQ(rt_event_wait(&g_r_ready, 5000), 0);
    CHECK_EQ(rt_event_wait(&g_n_ready, 5000), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ENOTQUIESCED);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    h0 = rt_file_hash(path);
    rt_event_set(&g_sealed);
    rt_join(&ta);
    rt_join(&tr);
    rt_join(&tn);
    rt_join(&tu);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_ESTOPPED);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_EALREADY);
    CHECK_EQ(rt_file_hash(path), h0);   /* the sealed bytes never change */
    return rt_expect(out, 1);
}

/* ---- pre-STOP slot reuse (r2 10.1) ---------------------------------------------------------------------------------- */

static void *reuse_a(void *a)
{
    ps3dk_prof_token t;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("A") == PS3DK_PROF_OK);
    WCHECK(RT_READ(INCARNATION) == 1);
    {
        PS3DK_PROF_ZONE("a");
    }
    ps3dk_prof_thread_end();
    rt_event_set(&g_e1);
    WCHECK(rt_event_wait(&g_e2, 5000) == 0);   /* B now owns A's old slot */
    {
        ps3dk_prof_zone z = pz();
        WCHECK(z.kind == 0);
        PS3DK_PROF_ZONE_END(z);
    }
    ps3dk_prof_frame();
    t = ps3dk_prof_frame_token();
    WCHECK(t.owner == 0);
    ps3dk_prof_thread_end();
    rt_event_set(&g_e3);
    WCHECK(ps3dk_prof_thread_begin("A2") == PS3DK_PROF_OK);   /* before the latch: a NEW incarnation id */
    WCHECK(RT_READ(INCARNATION) == 3);
    ps3dk_prof_thread_end();
    return NULL;
}

static rt_event g_b_check;
static void *reuse_b(void *a)
{
    ps3dk_prof_zone hb;
    uint64_t s, r, d;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("B") == PS3DK_PROF_OK);
    WCHECK(RT_READ(SLOT) == 1 && RT_READ(GENERATION) == 2 && RT_READ(INCARNATION) == 2);
    hb = pz();
    s = RT_READ(NEXT_SEQ);
    r = RT_READ(REFUSED);
    d = RT_READ(DEPTH);
    rt_event_set(&g_e2);
    WCHECK(rt_event_wait(&g_e3, 5000) == 0);
    WCHECK(RT_READ(NEXT_SEQ) == s && RT_READ(REFUSED) == r && RT_READ(DEPTH) == d);
    PS3DK_PROF_ZONE_END(hb);
    ps3dk_prof_thread_end();
    rt_event_set(&g_b_check);
    return NULL;
}

int rt_prestop_slot_reuse(const char *out)
{
    ps3dk_prof_config c;
    rt_thread ta, tb;
    rt_cap cap;
    rt_config(&c);
    c.max_threads = 2;
    rt_event_init(&g_e1);
    rt_event_init(&g_e2);
    rt_event_init(&g_e3);
    rt_event_init(&g_b_check);
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&ta, reuse_a, NULL);
    CHECK_EQ(rt_event_wait(&g_e1, 5000), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);   /* A drained: its slot is free */
    rt_spawn(&tb, reuse_b, NULL);
    rt_join(&tb);
    rt_join(&ta);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(cap.nthreads, 3);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- auto-registration (Q10) ---------------------------------------------------------------------------------------- */

static void *unregistered_calls(void *a)
{
    (void)a;
    {
        ps3dk_prof_zone z = pz();
        WCHECK(z.kind == 0);
        PS3DK_PROF_ZONE_END(z);
    }
    ps3dk_prof_frame();
    WCHECK(RT_READ(SLOT) == 0);
    return NULL;
}

int rt_autoreg_default_off(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_cap cap;
    rt_config(&c);
    CHECK_EQ(c.auto_register, 0);   /* the default */
    CHECK_OK(rt_start(out, &c, 1));
    rt_spawn(&t, unregistered_calls, NULL);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(cap.nthreads, 1);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static void *auto_then_retire(void *a)
{
    (void)a;
    {
        ps3dk_prof_zone z = pz();   /* auto-registered */
        WCHECK(z.kind == 1);
        PS3DK_PROF_ZONE_END(z);
    }
    WCHECK(RT_READ(INCARNATION) == 1);
    ps3dk_prof_thread_end();
    {
        ps3dk_prof_zone z = pz();   /* retired: never revived */
        WCHECK(z.kind == 0);
        PS3DK_PROF_ZONE_END(z);
    }
    ps3dk_prof_frame();
    WCHECK(RT_READ(INCARNATION) == 1);
    WCHECK(ps3dk_prof_thread_begin("explicit") == PS3DK_PROF_OK);   /* only an explicit begin */
    WCHECK(RT_READ(INCARNATION) == 2);
    ps3dk_prof_thread_end();
    return NULL;
}

static void *auto_then_ack(void *a)
{
    (void)a;
    {
        ps3dk_prof_zone z = pz();
        WCHECK(z.kind == 1);
        PS3DK_PROF_ZONE_END(z);
    }
    WCHECK(RT_READ(INCARNATION) == 3);
    rt_event_set(&g_e1);
    WCHECK(rt_wait_latch() == 0);
    ps3dk_prof_frame();   /* acks */
    {
        ps3dk_prof_zone z = pz();   /* acked: never revived */
        WCHECK(z.kind == 0);
        PS3DK_PROF_ZONE_END(z);
    }
    WCHECK(RT_READ(INCARNATION) == 3);
    return NULL;
}

int rt_autoreg_never_revives_acked_or_retired(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t, u;
    rt_cap cap;
    rt_config(&c);
    c.auto_register = 1;
    c.quiesce_timeout_us = 5000000;
    rt_event_init(&g_e1);
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&t, auto_then_retire, NULL);
    rt_join(&t);
    rt_spawn(&u, auto_then_ack, NULL);
    CHECK_EQ(rt_event_wait(&g_e1, 5000), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    rt_join(&u);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(cap.nthreads, 3);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- the timebase (Q4) and span ids (Q6) ---------------------------------------------------------------------------- */

static uint64_t g_tbz;
static uint64_t clk_zero_low(void) { return __atomic_add_fetch(&g_tbz, 1ull << 32, __ATOMIC_RELAXED); }

int rt_tb_zero_low_word_not_anomaly(const char *out)
{
    ps3dk_prof_config c;
    rt_cap cap;
    size_t i;
    uint64_t last = 0;
    int k;
    rt_config(&c);
    ps3dk_prof_test_hooks.clock = clk_zero_low;
    CHECK_OK(rt_start(out, &c, 1));
    for (k = 0; k < 10; k++) {
        ps3dk_prof_zone z = pz();
        ps3dk_prof_frame();
        PS3DK_PROF_ZONE_END(z);
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    for (i = 0; i < cap.nrecs; i++) {
        const rt_rec *r = &cap.recs[i];
        if (r->kind != RT_BEGIN && r->kind != RT_END && r->kind != RT_FRAME)
            continue;
        CHECK_EQ(r->b & 0xFFFFFFFFull, 0);   /* every low word is zero */
        CHECK(r->b >= last);                  /* and nothing decreases: no CLOCK_ANOMALY */
        last = r->b;
    }
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

static ps3dk_prof_zone refz(void) { return PS3DK_PROF_ZONE_BEGIN("full"); }

int rt_span_ids_dropped_get_id_refused_not(const char *out)
{
    ps3dk_prof_config c;
    rt_cap cap;
    size_t i;
    uint64_t prev = 0, last = 0;
    int k;
    rt_config(&c);
    c.max_descriptors = 1;
    CHECK_OK(rt_start(out, &c, 1));
    for (k = 0; k < 300; k++) {   /* 900 slots into a 512-slot ring: the later ones are dropped */
        ps3dk_prof_zone z = pz();
        PS3DK_PROF_ZONE_END(z);
    }
    CHECK(RT_READ(DROPPED) > 0);
    CHECK_EQ(RT_READ(NEXT_SPAN_ID), 301);   /* dropped BEGINs used their ids */
    {
        ps3dk_prof_zone r = refz();          /* refused (table full): no id */
        CHECK_EQ(r.kind, 2);
        PS3DK_PROF_ZONE_END(r);
    }
    CHECK_EQ(RT_READ(NEXT_SPAN_ID), 301);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    {
        ps3dk_prof_zone z = pz();
        CHECK_EQ(z.id, 301);
        PS3DK_PROF_ZONE_END(z);
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].kind == RT_BEGIN) {
            CHECK(cap.recs[i].c != 0);
            CHECK(cap.recs[i].c > prev);   /* increasing, no reuse, no wrap */
            prev = last = cap.recs[i].c;
        }
    CHECK_EQ(last, 301);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- table full plus ring full (5.3) and the sequence-limit pair (5.1) ---------------------------------------------- */

int rt_table_full_ring_full_not_clean(const char *out)
{
    ps3dk_prof_config c;
    rt_cap cap;
    const rt_final *f;
    int k;
    rt_config(&c);
    c.max_descriptors = 1;
    CHECK_OK(rt_start(out, &c, 1));
    for (k = 0; k < 200; k++) {
        ps3dk_prof_zone z = pz();
        PS3DK_PROF_ZONE_END(z);
    }
    CHECK(RT_READ(DROPPED) > 0);   /* the ring is full */
    {
        ps3dk_prof_zone r = refz();   /* TABLE_FULL: regerr counted, its REGERR dropped */
        PS3DK_PROF_ZONE_END(r);
    }
    CHECK_EQ(RT_READ(REGERR), 1);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(load(out, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->regerr, 1);
    CHECK(f->dropped > 0);
    CHECK_EQ(f->refused, 1);
    CHECK_EQ(rt_count(&cap, 1, RT_REGERR, -1), 0);   /* never looks clean: regerr > 0 and dropped > 0 */
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_seq_limit_1slot_then_refuse(const char *out)
{
    ps3dk_prof_config c;
    rt_cap cap;
    const rt_final *f;
    size_t i, frames = 0;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    RT_PRESET(NEXT_SEQ, RT_CEIL - 1);
    ps3dk_prof_frame();   /* takes 2^63 - 1: legal */
    CHECK_EQ(RT_READ(NEXT_SEQ), RT_CEIL);
    CHECK_EQ(RT_READ(LIMIT), 0);
    ps3dk_prof_frame();   /* refused: SEQ_LIMIT */
    CHECK_EQ(RT_READ(LIMIT), 1);
    CHECK_EQ(RT_READ(REFUSED), 1);
    CHECK_EQ(RT_READ(NEXT_SEQ), RT_CEIL);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);
    CHECK_EQ(load(out, &cap), 0);
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].kind == RT_FRAME) {
            CHECK_EQ(cap.recs[i].seq, RT_CEIL - 1);
            frames++;
        }
    CHECK_EQ(frames, 1);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->produced, RT_CEIL);
    CHECK_EQ(f->limit, 1);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}

int rt_seq_limit_2slot_refused_whole(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z;
    rt_cap cap;
    const rt_final *f;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    RT_PRESET(NEXT_SEQ, RT_CEIL - 1);
    z = pz();   /* BEGIN + BEGIN_PARENT would need 2^63: refused whole */
    CHECK_EQ(z.kind, 2);
    CHECK_EQ(RT_READ(NEXT_SEQ), RT_CEIL - 1);
    CHECK_EQ(RT_READ(LIMIT), 1);
    PS3DK_PROF_ZONE_END(z);
    CHECK_EQ(RT_READ(REFUSED), 1);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);
    CHECK_EQ(load(out, &cap), 0);
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->produced, RT_CEIL - 1);
    CHECK_EQ(f->written, 0);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}
