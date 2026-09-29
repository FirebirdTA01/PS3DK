/* spurs-suite SPU vector literals (row spu-vector-literals): the PS3
 * parenthesised vector literal and scalar splat forms, checked lane by lane
 * on the SPU.  x comes from the PPU (arg2) so the variable forms cannot be
 * folded at compile time.  Bit i of the result mask is set when check i
 * fails; the mask is DMAed to the box with the magic, and is the exit code.
 * arg1 = box EA, arg2 = x. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include "../vector_literals.h"

static int eq4(vec_int4 v, int a, int b, int c, int d)
{
    return spu_extract(v, 0) == a && spu_extract(v, 1) == b
        && spu_extract(v, 2) == c && spu_extract(v, 3) == d;
}

static unsigned marker;

int main(uint64_t box_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg3; (void)arg4;
    const int x = (int)arg2;
    unsigned fail = 0;

    /* 0: constant list */
    if (!eq4((vec_int4)(1, 2, 3, 4), 1, 2, 3, 4)) fail |= 1u << 0;
    /* 1: splat of a run-time scalar */
    if (!eq4((vec_int4)(x), x, x, x, x)) fail |= 1u << 1;
    /* 2: run-time list */
    if (!eq4((vec_int4)(x, x + 1, x + 2, x + 3), x, x + 1, x + 2, x + 3)) fail |= 1u << 2;
    /* 3: a short list is zero-filled (the compiler warns) */
    if (!eq4((vec_int4)(5, 6, 7), 5, 6, 7, 0)) fail |= 1u << 3;
    /* 4: float splat */
    {
        vec_float4 f = (vec_float4)(1.5f);
        if (spu_extract(f, 0) != 1.5f || spu_extract(f, 3) != 1.5f) fail |= 1u << 4;
    }
    /* 5: a double splatted into int lanes is converted (1.5 -> 1) */
    if (!eq4((vec_int4)(1.5), 1, 1, 1, 1)) fail |= 1u << 5;
    /* 6: byte list, then a reinterpret cast of a vector operand */
    {
        vec_uchar16 b = (vector unsigned char)(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        vec_uint4 w = (vec_uint4)(b);
        if (spu_extract(w, 0) != 0x00010203u || spu_extract(w, 3) != 0x0c0d0e0fu) fail |= 1u << 6;
    }
    /* 7: uintptr_t splats (uintptr_t is unsigned int on SPU) */
    {
        vec_uint4 p = spu_splats((uintptr_t)&marker);
        if (spu_extract(p, 2) != (unsigned)(uintptr_t)&marker || sizeof(uintptr_t) != 4) fail |= 1u << 7;
    }
    /* 8: a splat of a run-time negative value */
    if (!eq4((vec_int4)(-x), -x, -x, -x, -x)) fail |= 1u << 8;

    __attribute__((aligned(16))) uint32_t buf[4] = { VL_MAGIC, fail, (uint32_t)x, 0 };
    mfc_put(buf, box_ea, sizeof buf, 0, 0, 0);
    mfc_write_tag_mask(1u);
    mfc_read_tag_status_all();
    sys_spu_thread_exit((int)fail);
}
