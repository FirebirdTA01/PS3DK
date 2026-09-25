/*
 * altivec-strict-std - AltiVec vector types under a strict -std=.
 *
 * Built as -std=c++17 with extensions off.  The reference compiler treats
 * vector, pixel and bool as context-sensitive AltiVec keywords in every
 * language mode, and the SDK's vector headers (simdmath, vectormath) are
 * written with them; upstream GCC drops the keywords under a strict -std=,
 * so this file did not compile at all before GCC patch 0042.
 *
 * The run checks that the vector code computes the right values and that
 * std::vector and bool keep their C++ meaning beside the AltiVec keywords.
 * Prints ALTIVEC_STRICT_OK, or ALTIVEC_STRICT_FAIL naming the first failed
 * check.
 */

#include <altivec.h>
#include <cstdio>
#include <vector>

#include <simdmath/simdmath.h>
#include <vectormath/cpp/vectormath_aos.h>

#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#ifndef __STRICT_ANSI__
#error "this probe must be built in a strict -std= mode"
#endif

static vector float madd4(vector float a, vector float b, vector float c)
{
    return vec_madd(a, b, c);
}

static bool fail(const char *what)
{
    std::printf("ALTIVEC_STRICT_FAIL %s\n", what);
    return false;
}

static bool run()
{
    union { vector float v; float f[4]; } r;
    const vector float a = (vector float){1.0f, 2.0f, 3.0f, 4.0f};
    const vector float b = (vector float){2.0f, 2.0f, 2.0f, 2.0f};
    const vector float c = (vector float){0.5f, 0.5f, 0.5f, 0.5f};
    r.v = madd4(a, b, c);
    const float want[4] = {2.5f, 4.5f, 6.5f, 8.5f};
    for (int i = 0; i < 4; ++i)
        if (r.f[i] != want[i])
            return fail("vec_madd lanes");

    union { vector signed int v; int i[4]; } s;
    s.v = vec_splat_s32(-3);
    for (int i = 0; i < 4; ++i)
        if (s.i[i] != -3)
            return fail("vec_splat_s32");

    /* A vector bool compare result: all ones where a > b. */
    union { vector bool int v; unsigned u[4]; } m;
    m.v = vec_cmpgt(a, b);
    if (m.u[0] != 0 || m.u[1] != 0 || m.u[2] != 0xffffffffu || m.u[3] != 0xffffffffu)
        return fail("vec_cmpgt mask");

    /* simdmath from the SDK: fabsf4 on a vector. */
    union { vector float v; float f[4]; } ab;
    ab.v = fabsf4((vector float){-1.0f, 2.0f, -3.5f, 0.0f});
    if (ab.f[0] != 1.0f || ab.f[1] != 2.0f || ab.f[2] != 3.5f || ab.f[3] != 0.0f)
        return fail("simdmath fabsf4");

    /* vectormath (C++ AoS) from the SDK. */
    const Vectormath::Aos::Vector3 x = Vectormath::Aos::Vector3::xAxis();
    const Vectormath::Aos::Vector3 y = Vectormath::Aos::Vector3::yAxis();
    const Vectormath::Aos::Vector3 z = Vectormath::Aos::cross(x, y);
    if (z.getX() != 0.0f || z.getY() != 0.0f || z.getZ() != 1.0f)
        return fail("vectormath cross");

    /* The context-sensitive keywords leave ordinary C++ alone. */
    std::vector<int> list(3, 7);
    bool flag = list.size() == 3;
    int vector_sum = 0;
    for (int x_ : list)
        vector_sum += x_;
    if (!flag || vector_sum != 21)
        return fail("std::vector and bool");

    return true;
}

int main()
{
    if (run())
        std::printf("ALTIVEC_STRICT_OK\n");
    return 0;
}
