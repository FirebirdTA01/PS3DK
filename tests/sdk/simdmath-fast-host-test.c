/* Host test for the f4fast forms in sdk/include/simdmath/fastf4.h.
 *
 * Defines the vector layer over host GCC vectors, with reciprocal and
 * reciprocal-square-root estimates that are deliberately only 12 bits good
 * (each result is off by 2^-12 relative, sign alternating by lane), and
 * sweeps every form over its documented domain against double-precision
 * libm.  Reports the worst accuracy in bits of mantissa per form and fails
 * when one is below the bound fastf4.h documents.  Built without the PPU
 * estimate path (__FM_VEC_ESTIMATES), i.e. the SPU algorithms.
 *
 * With -DPRINT_ONLY the bounds are not enforced (used to measure). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef float __fm_vf __attribute__((vector_size(16)));
typedef unsigned int __fm_vu __attribute__((vector_size(16)));
typedef int __fm_vi __attribute__((vector_size(16)));

static inline __fm_vf __fm_splat(float f) { return (__fm_vf){ f, f, f, f }; }
static inline __fm_vi __fm_isplat(int n) { return (__fm_vi){ n, n, n, n }; }
static inline __fm_vf __fm_madd(__fm_vf a, __fm_vf b, __fm_vf c)
{
	__fm_vf r;
	for (int i = 0; i < 4; i++)
		r[i] = fmaf(a[i], b[i], c[i]);
	return r;
}
static inline __fm_vf __fm_nmsub(__fm_vf a, __fm_vf b, __fm_vf c)
{
	__fm_vf r;
	for (int i = 0; i < 4; i++)
		r[i] = fmaf(-a[i], b[i], c[i]);
	return r;
}
static inline __fm_vf __fm_mul(__fm_vf a, __fm_vf b) { return a * b; }
static inline __fm_vf __fm_add(__fm_vf a, __fm_vf b) { return a + b; }
static inline __fm_vf __fm_sub(__fm_vf a, __fm_vf b) { return a - b; }
static inline __fm_vf __fm_sel(__fm_vf a, __fm_vf b, __fm_vu m)
{
	return (__fm_vf)(((__fm_vu)a & ~m) | ((__fm_vu)b & m));
}
static inline __fm_vu __fm_cmpgt(__fm_vf a, __fm_vf b) { return (__fm_vu)(a > b); }
static inline __fm_vu __fm_cmpeq(__fm_vf a, __fm_vf b) { return (__fm_vu)(a == b); }
/* 12-bit estimates: exact result times (1 +- 2^-12) */
static inline __fm_vf __fm_re(__fm_vf x)
{
	__fm_vf r;
	for (int i = 0; i < 4; i++)
		r[i] = (float)((1.0 / x[i]) * (1.0 + ((i & 1) ? 1.0 : -1.0) / 4096.0));
	return r;
}
static inline __fm_vf __fm_rsqrte(__fm_vf x)
{
	__fm_vf r;
	for (int i = 0; i < 4; i++)
		r[i] = (float)((1.0 / sqrt((double)x[i])) * (1.0 + ((i & 1) ? -1.0 : 1.0) / 4096.0));
	return r;
}
static inline __fm_vi __fm_cts(__fm_vf x)
{
	__fm_vi r;
	for (int i = 0; i < 4; i++)
		r[i] = (int)x[i];
	return r;
}
static inline __fm_vf __fm_ctf(__fm_vi v)
{
	__fm_vf r;
	for (int i = 0; i < 4; i++)
		r[i] = (float)v[i];
	return r;
}
static inline __fm_vi __fm_iadd(__fm_vi a, __fm_vi b) { return a + b; }
static inline __fm_vi __fm_isub(__fm_vi a, __fm_vi b) { return a - b; }
static inline __fm_vi __fm_sl(__fm_vi a, int n) { return (__fm_vi)((__fm_vu)a << n); }
static inline __fm_vu __fm_sr(__fm_vu a, int n) { return a >> n; }

#include <simdmath/fastf4.h>

static int fails;

/* worst accuracy in bits over a sweep; exact 0 is compared absolutely */
typedef struct { double worst; double at; } Acc;
static void acc_init(Acc *a) { a->worst = 99.0; a->at = 0; }
static void acc_add(Acc *a, double got, double want, double where)
{
	double err = fabs(got - want);
	double den = fabs(want);
	double bits;
	if (isnan(got) != isnan(want))
		bits = 0;
	else if (isnan(want) || err == 0)
		bits = 99.0;
	else
		bits = -log2(den > 0 ? err / den : err);
	if (bits < a->worst) {
		a->worst = bits;
		a->at = where;
	}
}
static void report(const char *name, const Acc *a, double bound)
{
	int bad = a->worst < bound;
#ifdef PRINT_ONLY
	bad = 0;
#endif
	printf("%-14s %6.2f bits (worst at %.9g), bound %.0f %s\n", name, a->worst, a->at, bound,
	       bad ? "FAIL" : "ok");
	fails += bad;
}

typedef __fm_vf (*Unary)(__fm_vf);
static void sweep1(const char *name, Unary f, double (*ref)(double), double lo, double hi, int n,
                   double bound)
{
	Acc a;
	acc_init(&a);
	for (int i = 0; i <= n; i += 4) {
		__fm_vf x;
		for (int l = 0; l < 4; l++)
			x[l] = (float)(lo + (hi - lo) * (double)(i + l) / n);
		__fm_vf r = f(x);
		for (int l = 0; l < 4; l++)
			if (x[l] >= lo && x[l] <= hi)
				acc_add(&a, r[l], ref(x[l]), x[l]);
	}
	report(name, &a, bound);
}

/* geometric sweep over [lo, hi], lo > 0, with both signs when sym */
static void sweepg(const char *name, Unary f, double (*ref)(double), double lo, double hi, int n,
                   int sym, double bound)
{
	Acc a;
	acc_init(&a);
	for (int i = 0; i <= n; i += 4) {
		__fm_vf x;
		for (int l = 0; l < 4; l++) {
			double v = lo * pow(hi / lo, (double)(i + l) / n);
			x[l] = (float)((sym && ((i + l) & 1)) ? -v : v);
		}
		__fm_vf r = f(x);
		for (int l = 0; l < 4; l++)
			acc_add(&a, r[l], ref(x[l]), x[l]);
	}
	report(name, &a, bound);
}

static __fm_vf sin_s(__fm_vf x) { __fm_vf s, c; __fm_sincosf4fast(x, &s, &c); return s; }
static __fm_vf cos_s(__fm_vf x) { __fm_vf s, c; __fm_sincosf4fast(x, &s, &c); return c; }
static double exp2d(double x) { return exp2(x); }
static double log2d(double x) { return log2(x); }

int main(void)
{
	const double pi = 3.14159265358979323846;
	const int N = 400000;

	sweepg("sqrtf4fast", __fm_sqrtf4fast, sqrt, 1.2e-38, 3e38, N, 0, 11);
	sweep1("sinf4fast", __fm_sinf4fast, sin, -pi / 2, pi / 2, N, 20);
	sweepg("sinf4fast~0", __fm_sinf4fast, sin, 1e-30, 1e-2, N, 1, 20);
	sweep1("cosf4fast", __fm_cosf4fast, cos, 0, pi, N, 20);
	sweep1("sincos.sin", sin_s, sin, -pi / 4, pi / 4, N, 20);
	sweep1("sincos.cos", cos_s, cos, -pi / 4, pi / 4, N, 20);
	sweep1("tanf4fast", __fm_tanf4fast, tan, -pi / 4, pi / 4, N, 20);
	sweep1("asinf4fast", __fm_asinf4fast, asin, -1, 1, N, 9);
	sweep1("acosf4fast", __fm_acosf4fast, acos, -1, 1, N, 16);
	sweep1("atanf4fast", __fm_atanf4fast, atan, -8, 8, N, 17);
	sweepg("atanf4fast|x|", __fm_atanf4fast, atan, 1e-30, 3e38, N, 1, 17);
	sweep1("expm1f4fast", __fm_expm1f4fast, expm1, log(0.5), log(1.5), N, 20);
	sweepg("expm1f4fast~0", __fm_expm1f4fast, expm1, 1e-30, 1e-2, N, 1, 20);
	sweep1("log1pf4fast", __fm_log1pf4fast, log1p, -0.5, 0.5, N, 20);
	sweepg("log1pf4fast~0", __fm_log1pf4fast, log1p, 1e-30, 1e-2, N, 1, 20);
	sweep1("exp2f4fast", __fm_exp2f4fast, exp2d, -126, 127, N, 20);
	sweep1("expf4fast", __fm_expf4fast, exp, -87, 88, N, 20);
	sweepg("logf4fast", __fm_logf4fast, log, 1.2e-38, 3.4e38, N, 0, 20);
	sweep1("logf4fast~1", __fm_logf4fast, log, 0.9, 1.1, N, 20);
	sweepg("log2f4fast", __fm_log2f4fast, log2d, 1.2e-38, 3.4e38, N, 0, 20);
	sweep1("log2f4fast~1", __fm_log2f4fast, log2d, 0.9, 1.1, N, 20);
	sweepg("log10f4fast", __fm_log10f4fast, log10, 1.2e-38, 3.4e38, N, 0, 20);
	sweep1("log10f4fast~1", __fm_log10f4fast, log10, 0.9, 1.1, N, 20);

	/* special values the contract names */
	{
		__fm_vf inf = __fm_splat(INFINITY);
		__fm_vf r = __fm_atanf4fast((__fm_vf){ INFINITY, -INFINITY, 0.0f, -0.0f });
		if (r[0] != (float)(pi / 2) || r[1] != -(float)(pi / 2) || r[2] != 0.0f || r[3] != 0.0f) {
			printf("atanf4fast(+-Inf, +-0) = %g %g %g %g FAIL\n", r[0], r[1], r[2], r[3]);
			fails++;
		}
		r = __fm_atan2f4fast((__fm_vf){ 0, 0, 1, -1 }, (__fm_vf){ 0, 1, 0, 0 });
		if (r[0] != 0.0f || r[1] != 0.0f || fabsf(r[2] - (float)(pi / 2)) > 1e-4f ||
		    fabsf(r[3] + (float)(pi / 2)) > 1e-4f) {
			printf("atan2f4fast axes = %g %g %g %g FAIL\n", r[0], r[1], r[2], r[3]);
			fails++;
		}
		r = __fm_atan2f4fast((__fm_vf){ 1, -1, 0, 1 }, (__fm_vf){ -1, -1, -1, inf[0] });
		if (fabsf(r[0] - (float)(3 * pi / 4)) > 1e-4f || fabsf(r[1] + (float)(3 * pi / 4)) > 1e-4f ||
		    fabsf(r[2] - (float)pi) > 1e-4f || r[3] != 0.0f) {
			printf("atan2f4fast quadrants = %g %g %g %g FAIL\n", r[0], r[1], r[2], r[3]);
			fails++;
		}
	}

	/* atan2 over angles and radii */
	{
		Acc a;
		acc_init(&a);
		for (int i = 0; i < N; i++) {
			double t = -pi + 2 * pi * (i + 0.5) / N;
			double rad = pow(10.0, (i % 61) - 30);
			__fm_vf y = __fm_splat((float)(rad * sin(t))), x = __fm_splat((float)(rad * cos(t)));
			__fm_vf r = __fm_atan2f4fast(y, x);
			acc_add(&a, r[i & 3], atan2(y[0], x[0]), t);
		}
		report("atan2f4fast", &a, 17);
	}

	/* pow over x in [1/16, 16], y in [-8, 8] */
	{
		Acc a;
		acc_init(&a);
		for (int i = 0; i < N; i++) {
			float x = (float)(0.0625 * pow(256.0, (double)(i % 997) / 996));
			float y = (float)(-8 + 16.0 * (double)(i / 997) / (N / 997));
			__fm_vf r = __fm_powf4fast(__fm_splat(x), __fm_splat(y));
			acc_add(&a, r[i & 3], pow(x, y), x);
		}
		report("powf4fast", &a, 16);
	}

	printf("simdmath-fast: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
