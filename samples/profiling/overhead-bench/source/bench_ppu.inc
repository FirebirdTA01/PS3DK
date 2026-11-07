/* overhead-bench PPU cases: ONE loop source, included by bench_ppu_on.c (PS3DK_PROF=1), bench_ppu_off.c and
 * bench_ppu_off_b.c (PS3DK_PROF=0), which differ only in OB_TU and in the PS3DK_PROF definition (identical flags
 * otherwise).  Every timed function reads the boundary (common/ob_timer.h) immediately before and after its loop
 * and stores both readings; validity (end >= start) is decided by the caller.  The descriptor of every zone is the
 * macro's own static descriptor at its call site (warm after the case's untimed warmup block); P-cold's 64 sites are
 * 64 distinct call sites, each executed exactly once (probe-design-r2 D7).  OB_DEPTH_PROBE() is empty on the target;
 * the host ledger model defines it to record the zone depth at the innermost point.  Every loop iteration carries
 * OB_ITER() (common/ob_timer.h), so the OFF TUs keep the loop. */
#include <ps3dk/prof.h>

#include "../common/ob_timer.h"

#ifndef OB_TU
#error "define OB_TU"
#endif
#ifndef OB_DEPTH_PROBE
#define OB_DEPTH_PROBE() ((void)0)
#endif
#define OB_CAT2(a, b) a##b
#define OB_CAT(a, b) OB_CAT2(a, b)
#define OB_FN(name) OB_CAT(OB_CAT(bench_, OB_TU), OB_CAT(_, name))

typedef struct ob_t2 {
    uint64_t t0, t1;
} ob_t2;

void OB_FN(hot)(uint32_t n, ob_t2 *t)
{
    t->t0 = ob_tb();
    OB_NO_UNROLL
    for (uint32_t i = 0; i < n; ++i) {
        OB_ITER();
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("ob.hot");
        OB_DEPTH_PROBE();
        PS3DK_PROF_ZONE_END(z);
    }
    t->t1 = ob_tb();
}

void OB_FN(nest8)(uint32_t n, ob_t2 *t)
{
    t->t0 = ob_tb();
    OB_NO_UNROLL
    for (uint32_t i = 0; i < n; ++i) {
        OB_ITER();
        ps3dk_prof_zone z1 = PS3DK_PROF_ZONE_BEGIN("ob.n1");
        ps3dk_prof_zone z2 = PS3DK_PROF_ZONE_BEGIN("ob.n2");
        ps3dk_prof_zone z3 = PS3DK_PROF_ZONE_BEGIN("ob.n3");
        ps3dk_prof_zone z4 = PS3DK_PROF_ZONE_BEGIN("ob.n4");
        ps3dk_prof_zone z5 = PS3DK_PROF_ZONE_BEGIN("ob.n5");
        ps3dk_prof_zone z6 = PS3DK_PROF_ZONE_BEGIN("ob.n6");
        ps3dk_prof_zone z7 = PS3DK_PROF_ZONE_BEGIN("ob.n7");
        ps3dk_prof_zone z8 = PS3DK_PROF_ZONE_BEGIN("ob.n8");
        OB_DEPTH_PROBE();
        PS3DK_PROF_ZONE_END(z8);
        PS3DK_PROF_ZONE_END(z7);
        PS3DK_PROF_ZONE_END(z6);
        PS3DK_PROF_ZONE_END(z5);
        PS3DK_PROF_ZONE_END(z4);
        PS3DK_PROF_ZONE_END(z3);
        PS3DK_PROF_ZONE_END(z2);
        PS3DK_PROF_ZONE_END(z1);
    }
    t->t1 = ob_tb();
}

/* P-drop: the hot loop at its own call site, run on the ring the prefill filled */
void OB_FN(drop)(uint32_t n, ob_t2 *t)
{
    t->t0 = ob_tb();
    OB_NO_UNROLL
    for (uint32_t i = 0; i < n; ++i) {
        OB_ITER();
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("ob.drop");
        OB_DEPTH_PROBE();
        PS3DK_PROF_ZONE_END(z);
    }
    t->t1 = ob_tb();
}

/* P-prefill (UNTIMED): fill `slots` ring slots exactly: slots / 3 pairs (3 slots each), then the remainder as
 * frame marks (1 slot each) */
void OB_FN(prefill)(uint32_t slots)
{
    OB_NO_UNROLL
    for (uint32_t i = 0; i < slots / 3u; ++i) {
        OB_ITER();
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("ob.fill");
        PS3DK_PROF_ZONE_END(z);
    }
    OB_NO_UNROLL
    for (uint32_t i = 0; i < slots % 3u; ++i) {
        OB_ITER();
        PS3DK_PROF_FRAME();
    }
}

void OB_FN(frame)(uint32_t n, ob_t2 *t)
{
    t->t0 = ob_tb();
    OB_NO_UNROLL
    for (uint32_t i = 0; i < n; ++i) {
        OB_ITER();
        PS3DK_PROF_FRAME();
    }
    t->t1 = ob_tb();
}

/* P-empty: the boundary pair with nothing between (the raw block baseline) */
void OB_FN(empty)(uint32_t n, ob_t2 *t)
{
    (void)n;
    t->t0 = ob_tb();
    t->t1 = ob_tb();
}

/* P-cold: 64 distinct call sites */
#define OB_COLD_SITE(k)                                                                                              \
    static void OB_FN(cold_##k)(ob_t2 *t)                                                                            \
    {                                                                                                                  \
        t->t0 = ob_tb();                                                                                               \
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("ob.cold." #k);                                                      \
        OB_DEPTH_PROBE();                                                                                              \
        PS3DK_PROF_ZONE_END(z);                                                                                        \
        t->t1 = ob_tb();                                                                                               \
    }
#define OB_COLD8(a) OB_COLD_SITE(a##0) OB_COLD_SITE(a##1) OB_COLD_SITE(a##2) OB_COLD_SITE(a##3) OB_COLD_SITE(a##4)  \
    OB_COLD_SITE(a##5) OB_COLD_SITE(a##6) OB_COLD_SITE(a##7)
OB_COLD8(0) OB_COLD8(1) OB_COLD8(2) OB_COLD8(3) OB_COLD8(4) OB_COLD8(5) OB_COLD8(6) OB_COLD8(7)
#define OB_COLD_REF8(a) OB_FN(cold_##a##0), OB_FN(cold_##a##1), OB_FN(cold_##a##2), OB_FN(cold_##a##3),             \
    OB_FN(cold_##a##4), OB_FN(cold_##a##5), OB_FN(cold_##a##6), OB_FN(cold_##a##7)
void (*const OB_FN(cold)[64])(ob_t2 *) = {OB_COLD_REF8(0), OB_COLD_REF8(1), OB_COLD_REF8(2), OB_COLD_REF8(3),
                                          OB_COLD_REF8(4), OB_COLD_REF8(5), OB_COLD_REF8(6), OB_COLD_REF8(7)};
