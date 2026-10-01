// Reduced shape of the ILP32 address fault: a row loop calling an
// inlined per-element kernel that reads several float arrays at a row offset,
// with enough live values that the optimizer's biased IV is spilled.
#include <math.h>
#include <stdint.h>
#include <stddef.h>

struct C { float re, im; };

static inline C pairOf(C a, C b) { C r = { a.re - b.im, a.im + b.re }; return r; }

static inline int rowKernel(const C *h0, const C *h1, const float *omega, const float *invK,
                            const float *kxs, float kz, float t, uint32_t width,
                            C *o0, C *o1, C *o2, C *o3)
{
    for (uint32_t x = 0; x < width; ++x) {
        const float ph = omega[x] * t;
        if (!isfinite(ph))
            return 1;
        const float c = cosf(ph), s = sinf(ph), kx = kxs[x];
        C e;
        e.re = (h0[x].re * c - h0[x].im * s) + (h1[x].re * c + h1[x].im * s);
        e.im = (h0[x].re * s + h0[x].im * c) + (h1[x].im * c - h1[x].re * s);
        const float fx = kx * invK[x], fz = kz * invK[x];
        C dx = { fx * e.im, -fx * e.re }, dz = { fz * e.im, -fz * e.re };
        C xx = { kx * fx * e.re, kx * fx * e.im }, xz = { kx * fz * e.re, kx * fz * e.im };
        C zz = { kz * fz * e.re, kz * fz * e.im };
        o0[x] = pairOf(e, dx);
        o1[x] = pairOf(dz, xx);
        o2[x] = pairOf(xz, zz);
        if (o3) {
            C sx = { -kx * e.im, kx * e.re }, sz = { -kz * e.im, kz * e.re };
            o3[x] = pairOf(sx, sz);
        }
    }
    return 0;
}

struct Grid {
    uint32_t width, height;
    C *h0, *h1, *p0, *p1, *p2, *p3;
    float *omega, *invK, *kxs, *kzs;
    bool slopes;
};

int evolve(Grid &g, float t)
{
    for (uint32_t y = 0; y < g.height; ++y) {
        const size_t row = size_t(y) * size_t(g.width);
        if (rowKernel(&g.h0[row], &g.h1[row], &g.omega[row], &g.invK[row], g.kxs, g.kzs[y], t,
                      g.width, &g.p0[row], &g.p1[row], &g.p2[row], g.slopes ? &g.p3[row] : 0))
            return 1;
    }
    return 0;
}
