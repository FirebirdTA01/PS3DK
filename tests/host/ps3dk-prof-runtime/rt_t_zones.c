/* Zone handles: LIFO identity, refused begins, stale and foreign handles, guards (r2 3.2, E2, F1). */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

#include <inttypes.h>
#include <string.h>

/* One registered site and one site that fails registration (max_descriptors = 1, "p" registered first). */
static ps3dk_prof_zone pz(void) { return PS3DK_PROF_ZONE_BEGIN("p"); }
static ps3dk_prof_zone refz(void) { return PS3DK_PROF_ZONE_BEGIN("full"); }

static int start_one_desc(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z;
    rt_config(&c);
    c.max_descriptors = 1;
    c.ring_records = 2048;
    CHECK_OK(rt_start(out, &c, 1));
    z = pz();   /* registers "p": the table is now full */
    PS3DK_PROF_ZONE_END(z);
    return 0;
}

static int finish(const char *out, uint64_t refused)
{
    char path[512];
    rt_cap cap;
    const rt_final *f;
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->refused, refused);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_refused_end_duplicate(const char *out)
{
    ps3dk_prof_zone r0, a, acopy;
    CHECK_OK(start_one_desc(out));
    r0 = refz();
    a = refz();
    CHECK_EQ(r0.kind, 2);
    CHECK_EQ(a.kind, 2);
    acopy = a;
    PS3DK_PROF_ZONE_END(a);       /* valid */
    PS3DK_PROF_ZONE_END(acopy);   /* the duplicate: a misuse, and it consumes nothing else */
    CHECK_EQ(RT_READ(REFUSED_TOP), r0.id);
    PS3DK_PROF_ZONE_END(r0);
    CHECK_EQ(RT_READ(REFUSED_TOP), 0);
    CHECK_EQ(RT_READ(REGERR), 1);
    return finish(out, 3);
}

int rt_refused_end_out_of_order(const char *out)
{
    ps3dk_prof_zone a, acopy, b;
    CHECK_OK(start_one_desc(out));
    a = refz();
    acopy = a;
    b = refz();
    PS3DK_PROF_ZONE_END(a);   /* not innermost: a misuse, no change */
    CHECK_EQ(RT_READ(REFUSED_TOP), b.id);
    PS3DK_PROF_ZONE_END(b);
    CHECK_EQ(RT_READ(REFUSED_TOP), acopy.id);
    PS3DK_PROF_ZONE_END(acopy);   /* a fresh copy of A, now innermost: valid */
    CHECK_EQ(RT_READ(REFUSED_TOP), 0);
    return finish(out, 3);
}

int rt_refused_copy_aba_interleaved(const char *out)
{
    ps3dk_prof_zone a, acopy, b;
    CHECK_OK(start_one_desc(out));
    a = refz();
    acopy = a;
    PS3DK_PROF_ZONE_END(a);
    b = refz();
    CHECK(b.id != acopy.id);
    PS3DK_PROF_ZONE_END(acopy);   /* the saved copy of A must not consume B */
    CHECK_EQ(RT_READ(REFUSED_TOP), b.id);
    PS3DK_PROF_ZONE_END(b);
    CHECK_EQ(RT_READ(REFUSED_TOP), 0);
    return finish(out, 3);   /* two refused begins and one misuse */
}

int rt_refused_nested_valid_control(const char *out)
{
    ps3dk_prof_zone r[3];
    int i;
    CHECK_OK(start_one_desc(out));
    for (i = 0; i < 3; i++)
        r[i] = refz();
    for (i = 3; i-- > 0;)
        PS3DK_PROF_ZONE_END(r[i]);
    CHECK_EQ(RT_READ(REFUSED_TOP), 0);
    return finish(out, 3);
}

int rt_mixed_pushed_refused_lifo(const char *out)
{
    ps3dk_prof_zone p, r, q, p2, p2copy, r2;
    CHECK_OK(start_one_desc(out));
    p = pz();
    r = refz();
    q = pz();
    PS3DK_PROF_ZONE_END(q);
    PS3DK_PROF_ZONE_END(r);
    PS3DK_PROF_ZONE_END(p);
    CHECK_EQ(RT_READ(REFUSED), 1);   /* all three valid */
    CHECK_EQ(RT_READ(DEPTH), 0);
    p2 = pz();
    p2copy = p2;
    r2 = refz();
    PS3DK_PROF_ZONE_END(p2);   /* before R2: a misuse */
    CHECK_EQ(RT_READ(DEPTH), 1);
    PS3DK_PROF_ZONE_END(r2);
    PS3DK_PROF_ZONE_END(p2copy);
    CHECK_EQ(RT_READ(DEPTH), 0);
    return finish(out, 3);
}

int rt_pushed_copy_after_pop_no_aba(const char *out)
{
    ps3dk_prof_zone p, pcopy, q;
    CHECK_OK(start_one_desc(out));
    p = pz();
    pcopy = p;
    PS3DK_PROF_ZONE_END(p);
    q = pz();
    CHECK(q.id != pcopy.id);
    PS3DK_PROF_ZONE_END(pcopy);   /* a misuse: Q stays open */
    CHECK_EQ(RT_READ(DEPTH), 1);
    PS3DK_PROF_ZONE_END(q);
    CHECK_EQ(RT_READ(DEPTH), 0);
    return finish(out, 1);
}

int rt_nesting_misuse_refused(const char *out)
{
    ps3dk_prof_zone p, pcopy, q;
    CHECK_OK(start_one_desc(out));
    p = pz();
    pcopy = p;
    q = pz();
    PS3DK_PROF_ZONE_END(p);   /* not on top: refused, the stack unchanged */
    CHECK_EQ(RT_READ(REFUSED), 1);
    CHECK_EQ(RT_READ(DEPTH), 2);
    PS3DK_PROF_ZONE_END(q);
    PS3DK_PROF_ZONE_END(pcopy);
    CHECK_EQ(RT_READ(DEPTH), 0);
    return finish(out, 1);
}

int rt_handle_one_shot_same_object(const char *out)
{
    ps3dk_prof_zone h;
    char path[512];
    rt_cap cap;
    CHECK_OK(start_one_desc(out));
    h = pz();
    PS3DK_PROF_ZONE_END(h);
    CHECK_EQ(h.kind, 3);
    PS3DK_PROF_ZONE_END(h);   /* the same object again: a no-op */
    CHECK_EQ(RT_READ(REFUSED), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(rt_count(&cap, 1, RT_END, -1), 2);   /* start_one_desc's zone and h */
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_nested_refused_begin_handles(const char *out)
{
    ps3dk_prof_zone zs[256], r[3];
    int i;
    CHECK_OK(start_one_desc(out));
    for (i = 0; i < 256; i++)
        zs[i] = pz();
    for (i = 0; i < 3; i++) {
        r[i] = pz();   /* depth 256: refused, not pushed */
        CHECK_EQ(r[i].kind, 2);
    }
    CHECK_EQ(RT_READ(DEPTH), 256);
    for (i = 3; i-- > 0;)
        PS3DK_PROF_ZONE_END(r[i]);   /* consumed silently, pairing up */
    CHECK_EQ(RT_READ(REFUSED_TOP), 0);
    CHECK_EQ(RT_READ(REFUSED), 3);
    for (i = 256; i-- > 0;)
        PS3DK_PROF_ZONE_END(zs[i]);
    CHECK_EQ(RT_READ(DEPTH), 0);
    return finish(out, 3);
}

/* ---- guards (C cleanup) ---------------------------------------------------------------------------------------------- */

static int rec(int n)
{
    PS3DK_PROF_ZONE("rec");
    if (n == 0)
        return 0;
    return rec(n - 1) + 1;
}


static int early(int i)
{
    PS3DK_PROF_ZONE("early");
    if (i & 1)
        return 1;
    {
        PS3DK_PROF_ZONE("inner");
        if (i & 2)
            return 2;
    }
    return 0;
}

static int load_main(const char *out, rt_cap *cap)
{
    char path[512];
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, cap), 0);
    return 0;
}

int rt_recursion_handles(const char *out)
{
    ps3dk_prof_config c;
    rt_cap cap;
    size_t i;
    unsigned depths = 0;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    CHECK_EQ(rec(9), 9);   /* ten nested zones, one automatic handle each */
    CHECK_EQ(RT_READ(DEPTH), 0);
    CHECK_EQ(RT_READ(REFUSED), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_OK(load_main(out, &cap));
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 10);
    CHECK_EQ(rt_count(&cap, 1, RT_END, 0), 10);
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].kind == RT_BEGIN)
            depths |= 1u << cap.recs[i].depth;
    CHECK_EQ(depths, 0x3FFu);   /* depths 0..9, once each */
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

int rt_early_return_cleanup(const char *out)
{
    ps3dk_prof_config c;
    rt_cap cap;
    int i;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    for (i = 0; i < 8; i++)
        (void)early(i);
    CHECK_EQ(RT_READ(DEPTH), 0);
    CHECK_EQ(RT_READ(REFUSED), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_OK(load_main(out, &cap));
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 12);   /* 8 outer + 4 inner (even i) */
    CHECK_EQ(rt_count(&cap, 1, RT_END, 0), 12);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- stale and foreign handles --------------------------------------------------------------------------------------- */

static rt_event g_e1, g_e2, g_e3, g_e4;

static void *rebegin_worker(void *a)
{
    ps3dk_prof_zone h, hcopy, h2;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("old") == PS3DK_PROF_OK);
    h = PS3DK_PROF_ZONE_BEGIN("old");
    hcopy = h;
    ps3dk_prof_thread_end();
    rt_event_set(&g_e1);
    WCHECK(rt_event_wait(&g_e2, 5000) == 0);   /* the drainer freed the slot */
    WCHECK(ps3dk_prof_thread_begin("new") == PS3DK_PROF_OK);
    WCHECK(RT_READ(GENERATION) == 2);
    h2 = PS3DK_PROF_ZONE_BEGIN("new");
    PS3DK_PROF_ZONE_END(h);       /* the old guard: stale, no pop */
    WCHECK(RT_READ(DEPTH) == 1);
    WCHECK(RT_READ(REFUSED) == 0);
    PS3DK_PROF_ZONE_END(h2);
    WCHECK(RT_READ(DEPTH) == 0);
    PS3DK_PROF_ZONE_END(hcopy);   /* also stale */
    WCHECK(RT_READ(REFUSED) == 0);
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_stale_guard_after_rebegin_no_pop(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_config(&c);
    c.max_threads = 1;
    rt_event_init(&g_e1);
    rt_event_init(&g_e2);
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&t, rebegin_worker, NULL);
    CHECK_EQ(rt_event_wait(&g_e1, 5000), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    rt_event_set(&g_e2);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}

static ps3dk_prof_zone g_ha, g_hacopy;

static void *stale_a(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("A") == PS3DK_PROF_OK);   /* slot 0, incarnation 1 */
    g_ha = PS3DK_PROF_ZONE_BEGIN("A");
    g_hacopy = g_ha;
    ps3dk_prof_thread_end();
    rt_event_set(&g_e1);
    WCHECK(rt_event_wait(&g_e2, 5000) == 0);   /* B holds slot 0 */
    PS3DK_PROF_ZONE_END(g_ha);   /* retired TLS: touches nothing */
    rt_event_set(&g_e3);
    WCHECK(rt_event_wait(&g_e4, 5000) == 0);
    WCHECK(ps3dk_prof_thread_begin("C") == PS3DK_PROF_OK);   /* a NEW incarnation */
    WCHECK(RT_READ(INCARNATION) == 3);
    WCHECK(RT_READ(SLOT) == 2);
    PS3DK_PROF_ZONE_END(g_hacopy);   /* foreign to C: no pop, no counter */
    WCHECK(RT_READ(DEPTH) == 0);
    WCHECK(RT_READ(REFUSED) == 0);
    WCHECK(RT_READ(NEXT_SEQ) == 0);
    ps3dk_prof_thread_end();
    return NULL;
}

static void *stale_b(void *a)
{
    ps3dk_prof_zone hb;
    uint64_t s, r;
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("B") == PS3DK_PROF_OK);
    WCHECK(RT_READ(SLOT) == 1);
    WCHECK(RT_READ(GENERATION) == 2);
    hb = PS3DK_PROF_ZONE_BEGIN("B");
    s = RT_READ(NEXT_SEQ);
    r = RT_READ(REFUSED);
    rt_event_set(&g_e2);
    WCHECK(rt_event_wait(&g_e3, 5000) == 0);
    WCHECK(RT_READ(NEXT_SEQ) == s && RT_READ(REFUSED) == r && RT_READ(DEPTH) == 1);   /* A changed nothing */
    rt_event_set(&g_e4);
    PS3DK_PROF_ZONE_END(hb);
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_stale_guard_after_slot_reuse(const char *out)
{
    ps3dk_prof_config c;
    rt_thread a, b;
    rt_config(&c);
    c.max_threads = 2;
    rt_event_init(&g_e1);
    rt_event_init(&g_e2);
    rt_event_init(&g_e3);
    rt_event_init(&g_e4);
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&a, stale_a, NULL);
    CHECK_EQ(rt_event_wait(&g_e1, 5000), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);   /* A's RETIRED entry, then slot 0 is free */
    rt_spawn(&b, stale_b, NULL);
    rt_join(&b);
    rt_join(&a);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}

static void *reuse_gen_worker(void *a)
{
    (void)a;
    WCHECK(ps3dk_prof_thread_begin("g") == PS3DK_PROF_OK);
    WCHECK(RT_READ(GENERATION) == 1 && RT_READ(INCARNATION) == 1);
    ps3dk_prof_thread_end();
    rt_event_set(&g_e1);
    WCHECK(rt_event_wait(&g_e2, 5000) == 0);
    WCHECK(ps3dk_prof_thread_begin("g") == PS3DK_PROF_OK);
    WCHECK(RT_READ(GENERATION) == 2 && RT_READ(INCARNATION) == 2 && RT_READ(SLOT) == 1);
    ps3dk_prof_thread_end();
    return NULL;
}

int rt_retired_drain_then_reuse_generation(const char *out)
{
    ps3dk_prof_config c;
    rt_thread t;
    rt_cap cap;
    rt_config(&c);
    c.max_threads = 1;
    rt_event_init(&g_e1);
    rt_event_init(&g_e2);
    CHECK_OK(rt_start(out, &c, 0));
    rt_spawn(&t, reuse_gen_worker, NULL);
    CHECK_EQ(rt_event_wait(&g_e1, 5000), 0);
    CHECK_EQ(RT_STAT(SLOT_STATE_BASE + 0), RT_SLOT_RETIRING);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(RT_STAT(SLOT_STATE_BASE + 0), RT_SLOT_FREE);
    rt_event_set(&g_e2);
    rt_join(&t);
    CHECK_EQ(rt_worker_failed(), 0);
    CHECK_EQ(ps3dk_prof_drain(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_OK(load_main(out, &cap));
    CHECK_EQ(cap.nthreads, 2);
    CHECK_EQ(cap.threads_slot[0], 0);
    CHECK_EQ(cap.threads_slot[1], 0);   /* reused only after the RETIRED entry */
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}

/* ---- ID_COLLISION: SYNTHETIC branch coverage (codex 00:59 / 01:13 / 02:34) -----------------------------------------
 * No natural FNV-1a-64 collision is searched for or claimed.  The candidate_desc_id test hook replaces ONLY the
 * candidate id that the production collision comparison uses; the published descriptor, every stored id, the REGERR
 * record and the reader's id validation are untouched, so the capture is checked by the normal reader. */

static ps3dk_prof_desc g_ca, g_cb, g_ca2;
static uint64_t g_nat_a, g_nat_b, g_nat_a2;
static int g_seen_a, g_seen_b, g_seen_a2, g_force;

/* Records each natural id; with g_force, coll_b (and the coll_a twin g_ca2) get coll_a's id as the candidate. */
static uint64_t cand_hook(const char *name, uint32_t line, uint64_t natural)
{
    if (line == g_ca.line && strcmp(name, g_ca.name) == 0 && g_seen_a == 0) {
        g_seen_a = 1;
        g_nat_a = natural;
        return natural;
    }
    if (line == g_cb.line && strcmp(name, g_cb.name) == 0) {
        g_seen_b++;
        g_nat_b = natural;
        return g_force ? g_nat_a : natural;
    }
    if (line == g_ca.line && strcmp(name, g_ca.name) == 0) {   /* the second static of the same identity */
        g_seen_a2++;
        g_nat_a2 = natural;
        return g_force ? g_nat_a : natural;
    }
    return natural;
}

static uint64_t fnv_key(const char *name, const char *tag)   /* FNV-1a-64(name, 0x00, tag): the REGERR b field */
{
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char *p;
    for (p = (const unsigned char *)name; *p; p++)
        h = (h ^ *p) * 0x100000001b3ull;
    h = (h ^ 0u) * 0x100000001b3ull;
    for (p = (const unsigned char *)tag; *p; p++)
        h = (h ^ *p) * 0x100000001b3ull;
    return h;
}

static ps3dk_prof_zone zb(ps3dk_prof_desc *d) { return ps3dk_prof_zone_begin(d, PS3DK_PROF_TOKEN_NONE); }

static int coll_start(const char *out)
{
    ps3dk_prof_config c;
    rt_config(&c);
    c.ring_records = 2048;
    CHECK_OK(rt_start(out, &c, 1));
    rt_desc(&g_ca, "coll_a", 401);
    rt_desc(&g_cb, "coll_b", 402);   /* a different name AND line: a different (file, line, name) identity */
    rt_desc(&g_ca2, "coll_a", 401);  /* a second static with coll_a's identity */
    return 0;
}

static int coll_load(const char *out, rt_cap *cap)
{
    char path[512];
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, cap), 0);
    return 0;
}

int rt_id_collision_detected(const char *out)
{
    ps3dk_prof_zone a, b, b2, a2;
    rt_cap cap;
    const rt_final *f;
    size_t i, nregerr = 0;
    char path[512];
    CHECK_OK(coll_start(out));
    ps3dk_prof_test_hooks.candidate_desc_id = cand_hook;
    a = zb(&g_ca);                    /* registered normally: the hook returns the natural id */
    CHECK_EQ(a.kind, 1);
    PS3DK_PROF_ZONE_END(a);
    CHECK_EQ(g_seen_a, 1);
    CHECK_EQ(RT_READ(REGERR), 0);
    g_force = 1;
    b = zb(&g_cb);                    /* only its candidate id is forced to coll_a's */
    CHECK_EQ(g_seen_b, 1);
    CHECK(g_nat_b != g_nat_a);        /* distinct NATURAL ids: the collision is synthetic */
    CHECK_EQ(b.kind, 2);              /* REFUSED: no BEGIN */
    PS3DK_PROF_ZONE_END(b);           /* consumed silently */
    CHECK_EQ(RT_READ(REGERR), 1);
    CHECK_EQ(RT_READ(REFUSED), 1);
    b2 = zb(&g_cb);                   /* the cached failure: refused again, no second registration or REGERR */
    CHECK_EQ(g_seen_b, 1);
    CHECK_EQ(b2.kind, 2);
    PS3DK_PROF_ZONE_END(b2);
    CHECK_EQ(RT_READ(REGERR), 1);
    CHECK_EQ(RT_READ(REFUSED), 2);
    ps3dk_prof_test_hooks.candidate_desc_id = NULL;
    a2 = zb(&g_ca);                   /* the existing entry is unchanged and still in use */
    CHECK_EQ(a2.kind, 1);
    PS3DK_PROF_ZONE_END(a2);
    CHECK_OK(coll_load(out, &cap));
    CHECK_EQ(cap.ndesc, 1);           /* descriptor count unchanged by the refusal */
    CHECK_EQ(cap.desc_count, 1);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->refused, 2);
    CHECK_EQ(f->regerr, 1);
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 2);   /* coll_a twice; coll_b never */
    CHECK_EQ(rt_count(&cap, 1, RT_END, -1), 2);
    for (i = 0; i < cap.nrecs; i++) {
        const rt_rec *r = &cap.recs[i];
        if (r->kind == RT_BEGIN || r->kind == RT_END)
            CHECK_EQ(r->a, 0);        /* every BEGIN/END names coll_a's index */
        if (r->kind != RT_REGERR)
            continue;
        nregerr++;
        CHECK_EQ(r->a, 2);            /* ID_COLLISION */
        CHECK_EQ(r->b, fnv_key("coll_b", ""));
        /* c is "the descriptor id it would have had" (format section 4, record 6): coll_b's natural id.  It is not a
           reference to a published descriptor, and none carries it. */
        CHECK_EQ(r->c, g_nat_b);
    }
    CHECK_EQ(nregerr, 1);
    rt_cap_free(&cap);
    /* Through the normal `ps3dk-prof report` (section 9): the capture is TRANSPORT COMPLETE, yet the lane is NOT a
       valid measurement (refused and regerr reasons), so statistics use no lane and no zone row is computed from it.
       The retained descriptor is coll_a exactly as first published (natural id, file, line, name); coll_b never was. */
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    rt_report_line(path, "HAS", "verdict: COMPLETE (TRANSPORT COMPLETE)");
    rt_report_line(path, "MATCH", "1 main: INVALID [(]refused 2, regerr 1[)]; [0-9]+ records, [0-9]+ frames");
    rt_report_line(path, "HAS", "scope: statistics, frame elapsed and W use only lanes [none]");
    rt_report_line(path, "HAS", "index 0 id 0x%016" PRIx64 " scope 1 rt_runtime.c:401 coll_a", g_nat_a);
    rt_report_line(path, "NOMATCH", "index [1-9].*");
    rt_report_line(path, "NOMATCH", "coll_[ab]: .*");
    rt_report_line(path, "NOMATCH", ".* coll_b( .*)?");
    return rt_expect(out, 0);         /* and the normal check verdict */
}

int rt_id_noncollision_control(const char *out)
{
    ps3dk_prof_zone a, b;
    rt_cap cap;
    const rt_final *f;
    char path[512];
    CHECK_OK(coll_start(out));        /* the hook stays OFF */
    a = zb(&g_ca);
    b = zb(&g_cb);
    CHECK_EQ(a.kind, 1);
    CHECK_EQ(b.kind, 1);
    PS3DK_PROF_ZONE_END(b);
    PS3DK_PROF_ZONE_END(a);
    CHECK_EQ(RT_READ(REGERR), 0);
    CHECK_EQ(RT_READ(REFUSED), 0);
    CHECK_OK(coll_load(out, &cap));
    CHECK_EQ(cap.ndesc, 2);
    CHECK_EQ(cap.desc_count, 2);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->refused, 0);
    CHECK_EQ(f->regerr, 0);
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 2);
    CHECK_EQ(rt_count(&cap, 1, RT_REGERR, -1), 0);
    rt_cap_free(&cap);
    /* The same report checks, hook off: a valid lane in scope, both descriptors published, both zones counted. */
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    rt_report_line(path, "HAS", "verdict: COMPLETE (TRANSPORT COMPLETE)");
    rt_report_line(path, "MATCH", "1 main: MEASUREMENT VALID; [0-9]+ records, [0-9]+ frames");
    rt_report_line(path, "HAS", "scope: statistics, frame elapsed and W use only lanes [1]");
    rt_report_line(path, "MATCH", "index 0 id 0x[0-9a-f]{16} scope 1 rt_runtime[.]c:401 coll_a");
    rt_report_line(path, "MATCH", "index 1 id 0x[0-9a-f]{16} scope 1 rt_runtime[.]c:402 coll_b");
    rt_report_line(path, "HAS", "coll_a: 1 instances");
    rt_report_line(path, "HAS", "coll_b: 1 instances");
    return rt_expect(out, 0);
}

int rt_id_same_name_not_collision(const char *out)
{
    ps3dk_prof_zone a, a2;
    rt_cap cap;
    const rt_final *f;
    size_t i;
    char path[512];
    CHECK_OK(coll_start(out));
    ps3dk_prof_test_hooks.candidate_desc_id = cand_hook;
    a = zb(&g_ca);
    CHECK_EQ(a.kind, 1);
    PS3DK_PROF_ZONE_END(a);
    g_force = 1;
    a2 = zb(&g_ca2);                  /* same (file, line, name), candidate = coll_a's id: REUSE, not a collision */
    CHECK_EQ(g_seen_a2, 1);
    CHECK_EQ(g_nat_a2, g_nat_a);      /* the same identity has the same natural id */
    CHECK_EQ(a2.kind, 1);
    PS3DK_PROF_ZONE_END(a2);
    CHECK_EQ(RT_READ(REGERR), 0);
    CHECK_EQ(RT_READ(REFUSED), 0);
    CHECK_OK(coll_load(out, &cap));
    CHECK_EQ(cap.ndesc, 1);
    CHECK_EQ(cap.desc_count, 1);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->regerr, 0);
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 2);
    CHECK_EQ(rt_count(&cap, 1, RT_REGERR, -1), 0);
    for (i = 0; i < cap.nrecs; i++)
        if (cap.recs[i].kind == RT_BEGIN)
            CHECK_EQ(cap.recs[i].a, 0);   /* both statics share descriptor 0 */
    rt_cap_free(&cap);
    /* Reuse, not a collision: a valid lane, one descriptor with coll_a's natural id, both instances counted on it. */
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    rt_report_line(path, "MATCH", "1 main: MEASUREMENT VALID; [0-9]+ records, [0-9]+ frames");
    rt_report_line(path, "HAS", "scope: statistics, frame elapsed and W use only lanes [1]");
    rt_report_line(path, "HAS", "index 0 id 0x%016" PRIx64 " scope 1 rt_runtime.c:401 coll_a", g_nat_a);
    rt_report_line(path, "NOMATCH", "index [1-9].*");
    rt_report_line(path, "HAS", "coll_a: 2 instances");
    return rt_expect(out, 0);
}
