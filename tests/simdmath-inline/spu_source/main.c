// tests/simdmath-inline/spu_source/main.c
//
// SPU-side acceptance for the libsimdmath inline-visibility card.
// Two parts, both value-checked against IEEE-754 so this SPU image
// needs no libm at link time and cannot "get by" at runtime:
//
// (A) Single-precision f4 family, splat input, lane 0 checked:
//   sincosf4(0)              s == 0.0    c == 1.0          (exact)
//   sincosf4(1)              s ~ sin(1)  c ~ cos(1)        (1e-5 abs)
//   divf4(2, 4)              ~ 0.5                            (1e-5 abs)
//   rsqrtf4(4)               ~ 0.5                            (1e-5 abs)
//   divf4fast(2, 4)          ~ 0.5                            (1e-5 abs)
//
// (B) Double-precision d2 comparison family, both lanes.  Vector double
//     is two 64-bit IEEE-754 doubles; every comparison returns a
//     vector unsigned long long (two 64-bit mask words) with each word
//     either all-0s or all-1s depending on whether the predicate holds
//     for that lane.  The vector operands are built from exact bit
//     patterns so NaN / +inf / -inf / -0.0 / +0.0 / subnormal edge
//     cases are tested with no rounding:
//
//   isnand2        (NaN, NaN)                -> (all-1, all-1)
//   isnand2        (1.0, ~2.0)               -> (all-0, all-0)
//   isinfd2        (+inf, -inf)              -> (all-1, all-1)
//   isinfd2        (1.0, ~-2.0)              -> (all-0, all-0)
//   is0denormd2    (+0.0, subnormal)         -> (all-1, all-1)
//   is0denormd2    (1.0, NaN)                -> (all-0, all-0)
//   isequald2      (x, x)  x finite           -> (all-1, all-1)
//   isequald2      (NaN, NaN)                 -> (all-0, all-0)   // NaN != NaN
//   isequald2      (+0.0, -0.0)               -> (all-1, all-1)   // +0.0 == -0.0
//   islessequald2  (0.25, 0.5) vs (0.25, 0.25)-> (all-1, all-0)
//   isgreaterd2    (0.5, 0.25) vs (0.25, 0.25)-> (all-1, all-0)
//   isgreaterd2    (NaN, NaN) vs (finite)     -> (all-0, all-0)
//   signbitd2      (-0.0, +0.0)               -> (all-1, all-0)
//
// Exit 0 if every check passes, 1 otherwise, propagated via the SPU's
// sys_spu_thread_exit code (the PPU reads it with
// sys_spu_thread_get_exit_status).  The SPU DMAs a 16-byte `done` slot
// back to the PPU to end its bounded busy-wait; the exit code comes
// from the LV2 thread-exit path, not the DMA word.

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include <simdmath.h>

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Lane 0 of a SPU vector float, as a host scalar. */
static float s_lane0f (vector float v)
{
    return spu_extract (v, 0);
}

/* |a - b| <= tol  (plain float scalar compare, no intrinsics needed). */
static int s_approxf (float a, float b, float tol)
{
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= tol;
}

/* Bit-exact float scalar compare. */
static int s_exactf (float a, float b)
{
    return a == b;
}

/* Build a vector double (two 64-bit IEEE-754 doubles, even lane first)
 * from a pair of bit patterns, through a union so the stores are not
 * type-punned away. */
static vector double s_d2 (uint64_t lo, uint64_t hi)
{
    union { uint64_t u[2]; vector double v; } pair;
    pair.u[0] = lo;
    pair.u[1] = hi;
    return pair.v;
}

/* Lane value: +1 if the 64-bit word is all-1s, -1 if all-0s,
 * 0 if partial or some other value. */
static int s_d2lane (vector unsigned long long r, int lane)
{
    vector unsigned int v = (vector unsigned int) r;
    uint32_t lo = spu_extract (v, lane * 2);
    uint32_t hi = spu_extract (v, lane * 2 + 1);
    if (lo == 0xFFFFFFFFu && hi == 0xFFFFFFFFu) return  1;
    if (lo == 0x00000000u && hi == 0x00000000u) return -1;
    return 0;
}

/* Check both lanes of a d2 predicate result against expected signs. */
static int s_chk (vector unsigned long long r, int exp0, int exp1)
{
    return s_d2lane (r, 0) == exp0 && s_d2lane (r, 1) == exp1;
}

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */

int main (uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg2; (void)arg3; (void)arg4;
    uint64_t done_ea = arg1;    /* EA of PPU's 16-byte `spu_done` slot */

    int fail = 0;
    vector float s, c;
    const float tol = 10e-6f;   /* comfortably above single-precision round-off */

    /* ========== (A) f4 family (unchanged from v1) ========== */

    sincosf4 (spu_splats(0.0f), &s, &c);
    if (!s_exactf  (s_lane0f(s), 0.0f))  fail = 1;
    if (!s_exactf  (s_lane0f(c), 1.0f))  fail = 1;

    const float sin1 = 0.8414709848078965f;
    const float cos1 = 0.5403023058681398f;
    sincosf4 (spu_splats(1.0f), &s, &c);
    if (!s_approxf (s_lane0f(s), sin1, tol))  fail = 1;
    if (!s_approxf (s_lane0f(c), cos1, tol))  fail = 1;

    if (!s_approxf (s_lane0f(divf4 (spu_splats(2.0f), spu_splats(4.0f))),
                    0.5f, tol))  fail = 1;
    if (!s_approxf (s_lane0f(rsqrtf4 (spu_splats(4.0f))),
                    0.5f, tol))  fail = 1;
    /* divf4fast is the reciprocal estimate: about 12 bits, not 24 */
    if (!s_approxf (s_lane0f(divf4fast(spu_splats(2.0f), spu_splats(4.0f))),
                    0.5f, 0.5f / 1024.0f))  fail = 1;

    /* ========== (B) d2 comparison family ========== */

    /* -- isnand2 --
     * 0x7ff8000000000000: exponent all-ones, non-zero fraction -> NaN.
     * Both lanes are NaN in this test. */
    if (!s_chk (isnand2 (s_d2 (0x7ff8000000000000ull,
                               0x7ff8000000000000ull)), +1, +1))
        fail = 1;
    /* Finite values: not NaN -> all-zero per lane */
    if (!s_chk (isnand2 (s_d2 (0x3ff0000000000000ull,  /* +1.0  */
                               0xc000000000000002ull)), /* ~-2.0 */
                -1, -1))
        fail = 1;

    /* -- isinfd2 -- */
    if (!s_chk (isinfd2 (s_d2 (0x7ff0000000000000ull,   /* +inf */
                               0xfff0000000000000ull)),  /* -inf */
                +1, +1))
        fail = 1;
    if (!s_chk (isinfd2 (s_d2 (0x3ff0000000000000ull,
                               0xc000000000000001ull)),
                -1, -1))
        fail = 1;

    /* -- is0denormd2: exponent field is zero -> +0.0 and subnormals -- */
    if (!s_chk (is0denormd2 (s_d2 (0x0000000000000000ull,  /* +0.0     */
                                  0x0000000000000001ull)), /* subnormal */
                +1, +1))
        fail = 1;
    /* Normal value or NaN: exponent not zero -> all-zero */
    if (!s_chk (is0denormd2 (s_d2 (0x3ff0000000000000ull,
                                   0x7ff8000000000000ull)),
                -1, -1))
        fail = 1;

    /* -- isequald2 -- */
    {
        vector double x = s_d2 (0x3ff0000000000000ull,  /* +1.0  even  */
                                0xc000000000000001ull); /* ~-2.0 odd   */
        if (!s_chk (isequald2 (x, x), +1, +1))
            fail = 1;
    }
    /* NaN is not equal to itself */
    {
        vector double x = s_d2 (0x7ff8000000000000ull,
                                0x7ff8000000000000ull);
        if (!s_chk (isequald2 (x, x), -1, -1))
            fail = 1;
    }
    /* +0.0 == -0.0 per IEEE-754 */
    {
        vector double p0 = s_d2 (0x0000000000000000ull,   /* +0.0 even */
                                 0x0000000000000000ull),   /* +0.0 odd  */
                       n0 = s_d2 (0x8000000000000000ull,   /* -0.0 even */
                                  0x8000000000000000ull);  /* -0.0 odd  */
        if (!s_chk (isequald2 (p0, n0), +1, +1))
            fail = 1;
    }

    /* -- islessequald2 -- */
    /* a=(0.25 even, 0.5 odd),  b=(0.25 even, 0.25 odd).
     * Even lane: 0.25 <= 0.25  true.  Odd lane: 0.5  <= 0.25  false. */
    {
        vector double a = s_d2 (0x3fc0000000000000ull,  /* +0.25 even  */
                                0x3fe0000000000000ull), /* +0.5  odd   */
                       b = s_d2 (0x3fc0000000000000ull,
                                 0x3fc0000000000000ull); /* +0.25 both  */
        if (!s_chk (islessequald2 (a, b), +1, -1))
            fail = 1;
    }

    /* -- isgreaterd2 -- */
    /* a=(0.5 even, 0.25 odd),  b=(0.25 even, 0.25 odd).
     * Even lane: 0.5  > 0.25  true.  Odd lane: 0.25 > 0.25  false. */
    {
        vector double a = s_d2 (0x3fe0000000000000ull,  /* +0.5  even  */
                                0x3fc0000000000000ull), /* +0.25 odd   */
                       b = s_d2 (0x3fc0000000000000ull,
                                 0x3fc0000000000000ull); /* +0.25 both  */
        if (!s_chk (isgreaterd2 (a, b), +1, -1))
            fail = 1;
    }
    /* Any comparison involving NaN -> false */
    {
        vector double a = s_d2 (0x7ff8000000000000ull,
                                0x7ff8000000000000ull);
        vector double b = s_d2 (0x3ff0000000000000ull,
                                0x3ff0000000000000ull);
        if (!s_chk (isgreaterd2 (a, b), -1, -1))
            fail = 1;
    }

    /* -- signbitd2: MSB of the 64-bit double -- */
    {
        vector double v = s_d2 (0x8000000000000000ull,  /* -0.0 even */
                                0x0000000000000000ull); /* +0.0 odd  */
        if (!s_chk (signbitd2 (v), +1, -1))
            fail = 1;
    }

    /* -- the cmp*d2 names, called directly -- */
    {
        vector double nan2 = s_d2 (0x7ff8000000000000ull, 0x7ff8000000000000ull),
                     inf2 = s_d2 (0x7ff0000000000000ull, 0xfff0000000000000ull),
                     one2 = s_d2 (0x3ff0000000000000ull, 0x3ff0000000000000ull),
                     a    = s_d2 (0x3fe0000000000000ull,  /* +0.5  even */
                                  0x3fc0000000000000ull), /* +0.25 odd  */
                     b    = s_d2 (0x3fc0000000000000ull,
                                  0x3fc0000000000000ull); /* +0.25 both */
        if (!s_chk (cmpnand2 (nan2), +1, +1)) fail = 1;
        if (!s_chk (cmpinfd2 (inf2), +1, +1)) fail = 1;
        if (!s_chk (cmpzerodenormd2 (s_d2 (0, 1)), +1, +1)) fail = 1;   /* +0.0, smallest subnormal */
        if (!s_chk (cmpeqd2 (one2, one2), +1, +1)) fail = 1;
        if (!s_chk (cmpgtd2 (a, b), +1, -1)) fail = 1;
        if (!s_chk (cmpged2 (a, b), +1, +1)) fail = 1;                  /* 0.5 >= 0.25, 0.25 >= 0.25 */
        if (!s_chk (cmpged2 (b, a), -1, +1)) fail = 1;                  /* 0.25 >= 0.5 is false */
        if (!s_chk (cmpged2 (nan2, one2), -1, -1)) fail = 1;            /* NaN compares false */
        if (!s_chk (cmpnegsignd2 (s_d2 (0x8000000000000000ull, 0)), +1, -1)) fail = 1;
    }

    /* -- remainderf4 rounds the quotient to nearest, ties to even -- */
    if (!s_exactf (s_lane0f (remainderf4 (spu_splats (7.0f), spu_splats (2.0f))), -1.0f)) fail = 1;
    if (!s_exactf (s_lane0f (remainderf4 (spu_splats (5.0f), spu_splats (2.0f))),  1.0f)) fail = 1;
    if (!s_exactf (s_lane0f (remainderf4 (spu_splats (7.5f), spu_splats (2.0f))), -0.5f)) fail = 1;

    /* -- divf4fast is the reciprocal estimate: close to, not exactly, 1/3 -- */
    if (!s_approxf (s_lane0f (divf4fast (spu_splats (1.0f), spu_splats (3.0f))),
                    1.0f / 3.0f, (1.0f / 3.0f) / 1024.0f)) fail = 1;

    /* ========== signal completion to PPU, then exit with our status ========== */
    {
        __attribute__((aligned(16))) uint32_t buf[4] = { 0x5d3db123, 0, 0, 0 };
        mfc_put (buf, done_ea, 16, 0, 0, 0);
        mfc_write_tag_mask (1u << 0);
        mfc_read_tag_status_all ();
    }
    sys_spu_thread_exit (fail);
}
