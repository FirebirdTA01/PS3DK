/* overhead-bench SPU cases: ONE loop source, included by bench_spu_on.c (the lane writer, PS3DK_PROF=1) and
 * bench_spu_off.c (no writer call at all: PS3DK_PROF=0 alone would not remove a direct writer call such as the flush,
 * so the OFF binding maps every OB_S* operation to nothing; the A1 audit proves the OFF object references no writer
 * symbol), and by the host ledger model (host-tests/test_ob_spu.c, the writer core under the transport model).
 * The includer defines OB_TU, OB_DEC() (the decrementer boundary, common/ob_timer.h on the SPU), OB_SB(name),
 * OB_SE(z), OB_SM(name) and OB_FLUSH().  Decrementer readings are ticks (Plan A r2 2.3); the caller computes the
 * modulo 2^32 difference and its validity. */
#include "../common/ob_timer.h" /* OB_ITER */

#ifndef OB_TU
#error "define OB_TU"
#endif
#define OB_SCAT2(a, b) a##b
#define OB_SCAT(a, b) OB_SCAT2(a, b)
#define OB_SFN(name) OB_SCAT(OB_SCAT(bench_spu_, OB_TU), OB_SCAT(_, name))

typedef struct ob_d2 {
    uint32_t d0, d1;
} ob_d2;

/* S-pair and S-stagefull: n pairs, no flush */
void OB_SFN(pairs)(uint32_t n, ob_d2 *t)
{
    t->d0 = OB_DEC();
    OB_NO_UNROLL
    for (uint32_t i = 0; i < n; ++i) {
        OB_ITER();
        uint32_t z = OB_SB("ob.s");
        OB_SE(z);
    }
    t->d1 = OB_DEC();
}

/* UNTIMED preconditions: k marks into the staging */
void OB_SFN(stage)(uint32_t k)
{
    OB_NO_UNROLL
    for (uint32_t i = 0; i < k; ++i) {
        OB_ITER();
        OB_SM("ob.k");
    }
}

/* S-flush-k and S-backpressure: one timed flush; its result is returned */
int OB_SFN(flush)(ob_d2 *t)
{
    t->d0 = OB_DEC();
    int rc = OB_FLUSH();
    t->d1 = OB_DEC();
    return rc;
}
