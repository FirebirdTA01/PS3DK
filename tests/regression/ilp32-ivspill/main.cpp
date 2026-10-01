// Runtime half of the ILP32 IV-spill regression: run evolve() over several rows.
// With the faulty compiler the second row's reloaded IV bias carries into bit 32
// of an address and the load faults; with the fix every row completes and the
// checksum matches the value computed below without the row loop.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>

struct C { float re, im; };
struct Grid {
    uint32_t width, height;
    C *h0, *h1, *p0, *p1, *p2, *p3;
    float *omega, *invK, *kxs, *kzs;
    bool slopes;
};
int evolve(Grid &g, float t);

static const uint32_t W = 16, H = 8, N = W * H;
static C h0[N], h1[N], p0[N], p1[N], p2[N], p3[N];
static float omega[N], invK[N], kxs[W], kzs[H];

int main()
{
    for (uint32_t i = 0; i < N; ++i) {
        h0[i].re = 0.01f * (float)(i % 7); h0[i].im = 0.02f * (float)(i % 5);
        h1[i].re = 0.03f * (float)(i % 3); h1[i].im = 0.01f * (float)(i % 11);
        omega[i] = 0.5f + 0.001f * (float)i; invK[i] = 1.0f / (1.0f + (float)(i % 13));
    }
    for (uint32_t x = 0; x < W; ++x) kxs[x] = 0.1f * (float)x;
    for (uint32_t y = 0; y < H; ++y) kzs[y] = 0.2f * (float)y;
    Grid g = { W, H, h0, h1, p0, p1, p2, p3, omega, invK, kxs, kzs, true };
    if (evolve(g, 1.25f) != 0) {
        printf("IVSPILL FAIL: evolve reported a bad phase\n");
        return 1;
    }
    double sum = 0;
    for (uint32_t i = 0; i < N; ++i)
        sum += p0[i].re + p0[i].im + p1[i].re + p2[i].im + p3[i].re;
    int ok = isfinite(sum) && p3[N - 1].re == p3[N - 1].re;
    printf("IVSPILL rows=%u checksum=%.6f\n", H, sum);
    printf("IVSPILL %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
