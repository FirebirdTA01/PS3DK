// tests/simdmath-inline/spu_source/main.c
//
// SPU-side acceptance for the libsimdmath inline-visibility card.
//
// Purpose: prove that calling sincosf4 / divf4 / rsqrtf4 / divf4fast from a
// plain `#include <simdmath.h>` links against the per-arch SPU static
// inlines installed by the sdk/libsimdmath build, NOT against
// libsimdmath.a.  The CMake rule for this SPU image intentionally omits
// `simdmath` from LIBS; if the inlines were not visible, the link fails,
// and there is no way to "get by" at runtime.
//
// Value checks (hard-coded expected values so this SPU image needs no libm):
//   sincosf4(0)              s == 0.0   c == 1.0         (exact)
//   sincosf4(1)              s ~ sin(1), c ~ cos(1)      (1e-5 abs)
//   divf4(2, 4)              ~ 0.5                       (1e-5 abs)
//   rsqrtf4(4)               ~ 1/sqrt(4) = 0.5           (1e-5 abs)
//   divf4fast(2, 4)          ~ 0.5                       (1e-5 abs)
//
// Tolerance is 1e-5 in absolute value (about 5 ULPs at these magnitudes),
// comfortably above the single-precision round-off of the polynomials and
// the 1-iteration Newton-Raphson estimates in the SPU inline set, but
// strict enough that a missing or wrong inline cannot pass.
//
// Exit: 0 if every check passes, 1 otherwise.  The PPU joins the SPU
// thread group and returns this code; the harness asserts that the join's
// result word is 0.

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include <simdmath.h>

/* Lane 0 of a SPU vector float, as a host scalar. */
static float lane0 (vector float v)
{
    return spu_extract (v, 0);
}

/* |a - b| <= tol  (plain float scalar compare, no intrinsics needed). */
static int approx (float a, float b, float tol)
{
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= tol;
}

/* Bit-exact scalar compare. */
static int exact (float a, float b)
{
    return a == b;
}

int main (uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg2; (void)arg3; (void)arg4;
    uint64_t done_ea = arg1;    /* EA of PPU's `spu_done` word */

    int fail = 0;
    vector float s, c;
    const float tol = 10e-6f;          /* comfortably above single-precision round-off */

    /* -- sincosf4(0) == (0, 1) -- */
    sincosf4 (spu_splats(0.0f), &s, &c);
    if (!exact (lane0(s), 0.0f))      fail = 1;
    if (!exact (lane0(c), 1.0f))      fail = 1;

    /* -- sincosf4(1) ~= (sin(1), cos(1)) -- */
    const float sin1 = 0.8414709848078965f;
    const float cos1 = 0.5403023058681398f;
    sincosf4 (spu_splats(1.0f), &s, &c);
    if (!approx (lane0(s), sin1, tol))  fail = 1;
    if (!approx (lane0(c), cos1, tol))  fail = 1;

    /* -- divf4(2, 4) ~= 0.5 -- */
    if (!approx (lane0(divf4(spu_splats(2.0f), spu_splats(4.0f))),
                0.5f, tol))  fail = 1;

    /* -- rsqrtf4(4) ~= 1 / sqrt(4) = 0.5 -- */
    if (!approx (lane0(rsqrtf4(spu_splats(4.0f))),
                0.5f, tol))  fail = 1;

    /* -- fast form: divf4fast(2, 4) ~= 0.5 -- */
    if (!approx (lane0(divf4fast(spu_splats(2.0f), spu_splats(4.0f))),
                0.5f, tol))  fail = 1;

    /* Signal the PPU via DMA so its busy-wait ends, then exit with our
     * status byte.  sys_spu_thread_exit(0) is required as the final
     * statement per the SPU crt contract (see samples/spu/hello-spu). */
    {
        __attribute__((aligned(16))) uint32_t buf[4] = { 0xc0ffee, 0, 0, 0 };
        mfc_put(buf, done_ea, 16, 0, 0, 0);
        mfc_write_tag_mask(1u << 0);
        mfc_read_tag_status_all();
    }
    sys_spu_thread_exit(fail);
}
