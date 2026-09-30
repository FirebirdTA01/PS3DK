/* spurs-suite SPU vector literals, C++ half: the same parenthesised forms
 * through the SPU C++ front end, plus the forms where a vector (or other
 * non-arithmetic) operand keeps its ordinary cast meaning with postfix
 * operators: (V)(arr)[i], (V)(s).m, (V)(f)(x) and (V)(p)->m.  Bit i of the
 * returned mask is set when check i fails. */
#include <spu_intrinsics.h>

typedef vector unsigned int V;
typedef vector float F;

struct S { V m; };

static V s_arr[2];

static V make(unsigned x) { return spu_splats(x); }

static bool eq4(V v, unsigned a, unsigned b, unsigned c, unsigned d)
{
    return spu_extract(v, 0) == a && spu_extract(v, 1) == b
        && spu_extract(v, 2) == c && spu_extract(v, 3) == d;
}

extern "C" unsigned vector_literals_cpp(int xi)
{
    const unsigned x = static_cast<unsigned>(xi);
    unsigned fail = 0;

    /* 0: splat of a run-time scalar */
    if (!eq4((V)(x), x, x, x, x)) fail |= 1u << 0;
    /* 1: run-time list */
    if (!eq4((V)(x, x + 1, x + 2, x + 3), x, x + 1, x + 2, x + 3)) fail |= 1u << 1;
    /* 2: a nested scalar cast is splatted after conversion */
    {
        volatile float f = static_cast<float>(x) + 0.75f;
        if (!eq4((V)((unsigned)f), x, x, x, x)) fail |= 1u << 2;
    }
    /* 3: vector to vector is a reinterpret cast */
    {
        F one = spu_splats(1.0f);
        if (!eq4((V)(one), 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u)) fail |= 1u << 3;
    }
    /* 4: (V)(arr)[i] indexes the array, then casts */
    s_arr[0] = (V)(0u);
    s_arr[1] = (V)(x, 2, 3, 4);
    {
        volatile int i = 1;
        if (!eq4((V)(s_arr)[i], x, 2, 3, 4)) fail |= 1u << 4;
    }
    /* 5: (V)(s).m selects the member, then casts */
    {
        S s;
        s.m = (V)(7, 8, 9, x);
        if (!eq4((V)(s).m, 7, 8, 9, x)) fail |= 1u << 5;
        /* 6: (V)(p)->m through a pointer */
        S *p = &s;
        if (!eq4((V)(p)->m, 7, 8, 9, x)) fail |= 1u << 6;
    }
    /* 7: (V)(f)(x) calls, then casts */
    if (!eq4((V)(make)(x + 5), x + 5, x + 5, x + 5, x + 5)) fail |= 1u << 7;
    /* 8: a short list is zero-filled (the compiler warns) */
    if (!eq4((V)(5u, 6u, 7u), 5, 6, 7, 0)) fail |= 1u << 8;
    return fail;
}
