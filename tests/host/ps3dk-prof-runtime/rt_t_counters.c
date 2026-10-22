/* The counter ceiling, fail-closed (format 5.1 "Counter ceiling", the codex 23:55 boundary correction).  Every case
 * presets a counter near 2^63, then checks: the counter is exact and never past 2^63, SEQ_LIMIT is set, the
 * shutdown result is ESEQLIMIT and the capture is INCOMPLETE (end_flags bit1), never clean. */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

static ps3dk_prof_zone pz(void) { return PS3DK_PROF_ZONE_BEGIN("p"); }
static ps3dk_prof_zone refz(void) { return PS3DK_PROF_ZONE_BEGIN("full"); }

static int start(const char *out, uint32_t max_desc)
{
    ps3dk_prof_config c;
    ps3dk_prof_zone z;
    rt_config(&c);
    c.max_descriptors = max_desc;
    CHECK_OK(rt_start(out, &c, 1));
    z = pz();   /* "p" registered (with max_desc 1 the table is now full) */
    PS3DK_PROF_ZONE_END(z);
    return 0;
}

/* Shutdown and the final entry of incarnation 1. */
static int finish(const char *out, uint64_t refused, uint64_t regerr, size_t regerr_records)
{
    char path[512];
    rt_cap cap;
    const rt_final *f;
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(cap.has_trailer, 1);
    CHECK_EQ(cap.end_flags & 2u, 2u);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->state, RT_ACKED);
    CHECK_EQ(f->limit, 1);
    CHECK_EQ(f->refused, refused);
    CHECK_EQ(f->regerr, regerr);
    CHECK(f->refused <= RT_CEIL && f->regerr <= RT_CEIL);
    CHECK_EQ(rt_count(&cap, 1, RT_REGERR, -1), regerr_records);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}

int rt_counter_refused_misuse_at_ceiling(const char *out)
{
    ps3dk_prof_zone p, pcopy, q;
    CHECK_OK(start(out, 64));
    RT_PRESET(REFUSED, RT_CEIL);
    p = pz();
    pcopy = p;
    q = pz();
    PS3DK_PROF_ZONE_END(p);   /* a misuse that cannot be counted: not applied, fail closed */
    CHECK_EQ(RT_READ(REFUSED), RT_CEIL);
    CHECK_EQ(RT_READ(LIMIT), 1);
    CHECK_EQ(RT_READ(EXHAUSTED), 1);
    CHECK_EQ(RT_READ(DEPTH), 2);   /* the misuse changed nothing else */
    PS3DK_PROF_ZONE_END(q);       /* valid LIFO; its END is refused (the limit), still not counted past 2^63 */
    PS3DK_PROF_ZONE_END(pcopy);
    CHECK_EQ(RT_READ(DEPTH), 0);
    CHECK_EQ(RT_READ(REFUSED), RT_CEIL);
    return finish(out, RT_CEIL, 0, 0);
}

int rt_counter_refused_at_ceiling_id_below(const char *out)
{
    ps3dk_prof_zone r1, r2;
    CHECK_OK(start(out, 1));
    RT_PRESET(REFUSED, RT_CEIL - 1);
    r1 = refz();                  /* refused_count reaches exactly 2^63: applied */
    CHECK_EQ(RT_READ(REFUSED), RT_CEIL);
    CHECK_EQ(RT_READ(EXHAUSTED), 0);
    CHECK_EQ(r1.kind, 2);
    r2 = refz();                  /* refused_count at its ceiling, the refused-begin id still below its own */
    CHECK_EQ(r2.kind, 2);         /* a real refused handle: LIFO keeps working */
    CHECK(r2.id < RT_CEIL);
    CHECK_EQ(RT_READ(REFUSED), RT_CEIL);
    CHECK_EQ(RT_READ(LIMIT), 1);
    CHECK_EQ(RT_READ(EXHAUSTED), 1);
    PS3DK_PROF_ZONE_END(r2);
    PS3DK_PROF_ZONE_END(r1);
    CHECK_EQ(RT_READ(REFUSED_TOP), 0);
    CHECK_EQ(RT_READ(REFUSED), RT_CEIL);
    /* regerr: "full" failed once (1), its REGERR record written before the limit */
    return finish(out, RT_CEIL, 1, 1);
}

int rt_counter_regerr_at_ceiling(const char *out)
{
    ps3dk_prof_zone a, b;
    CHECK_OK(start(out, 1));
    RT_PRESET(REGERR, RT_CEIL - 1);
    a = PS3DK_PROF_ZONE_BEGIN("a");   /* TABLE_FULL: regerr reaches exactly 2^63, its REGERR is attempted */
    CHECK_EQ(RT_READ(REGERR), RT_CEIL);
    CHECK_EQ(RT_READ(LIMIT), 0);
    b = PS3DK_PROF_ZONE_BEGIN("b");   /* TABLE_FULL at the ceiling: not applied, no REGERR, fail closed */
    CHECK_EQ(RT_READ(REGERR), RT_CEIL);
    CHECK_EQ(RT_READ(LIMIT), 1);
    CHECK_EQ(RT_READ(EXHAUSTED), 1);
    PS3DK_PROF_ZONE_END(b);
    PS3DK_PROF_ZONE_END(a);
    /* refused: both zones (their refusals are exact: refused_count is far below its ceiling) */
    return finish(out, 2, RT_CEIL, 1);
}

int rt_counter_refused_id_exhausted(const char *out)
{
    ps3dk_prof_zone r;
    CHECK_OK(start(out, 1));
    RT_PRESET(NEXT_REFUSED_ID, RT_CEIL);
    r = refz();                   /* no refused-begin id is issued past 2^63 - 1 */
    CHECK_EQ(r.kind, 0);          /* INERT */
    CHECK_EQ(RT_READ(LIMIT), 1);
    CHECK_EQ(RT_READ(EXHAUSTED), 1);
    CHECK_EQ(RT_READ(REFUSED), 1);   /* the refusal itself still counted exactly */
    CHECK_EQ(RT_READ(REFUSED_TOP), 0);
    PS3DK_PROF_ZONE_END(r);       /* no-op */
    CHECK_EQ(RT_READ(REFUSED), 1);
    return finish(out, 1, 1, 1);
}

int rt_counter_span_id_exhausted(const char *out)
{
    ps3dk_prof_zone z;
    CHECK_OK(start(out, 64));
    RT_PRESET(NEXT_SPAN_ID, RT_CEIL);
    z = pz();                     /* no span id is issued past 2^63 - 1: refused, fail closed */
    CHECK_EQ(z.kind, 2);
    CHECK_EQ(RT_READ(LIMIT), 1);
    CHECK_EQ(RT_READ(EXHAUSTED), 1);
    CHECK_EQ(RT_READ(NEXT_SPAN_ID), RT_CEIL);
    PS3DK_PROF_ZONE_END(z);
    return finish(out, 1, 0, 0);
}

int rt_counter_stop_batch_refused_at_ceiling(const char *out)
{
    ps3dk_prof_zone z[3];
    char path[512];
    rt_cap cap;
    const rt_final *f;
    int i;
    CHECK_OK(start(out, 64));
    for (i = 0; i < 3; i++)
        z[i] = pz();
    RT_PRESET(NEXT_SEQ, RT_CEIL - 1);       /* k = 3 does not fit: the batch is not admitted */
    RT_PRESET(REFUSED, RT_CEIL - 1);        /* and refused_count + 3 would pass 2^63 */
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_ESEQLIMIT);
    for (i = 3; i-- > 0;)
        PS3DK_PROF_ZONE_END(z[i]);
    CHECK_EQ(RT_READ(REFUSED), RT_CEIL - 1);   /* left unchanged, never saturated */
    CHECK_EQ(RT_READ(EXHAUSTED), 1);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    f = rt_final_of(&cap, 1);
    CHECK(f != NULL);
    CHECK_EQ(f->refused, RT_CEIL - 1);
    CHECK_EQ(f->open_spans, 3);
    CHECK_EQ(f->limit, 1);
    CHECK_EQ(f->produced, RT_CEIL - 1);
    CHECK_EQ(cap.end_flags & 2u, 2u);
    CHECK_EQ(rt_count(&cap, 1, RT_END, 1), 0);
    rt_cap_free(&cap);
    return rt_expect(out, 1);
}
