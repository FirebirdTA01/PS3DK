/* simdmath/fastf4.h - the reduced-accuracy f4fast forms of <simdmath.h>.
 *
 * Included by <simdmath.h>, which first defines the vector layer below for
 * the PPU (VMX) or the SPU; tests/sdk/simdmath-fast-host-test.sh defines it
 * over host vectors.  Each form trades accuracy or domain for speed, as
 * documented per function; outside its domain a result is unspecified and
 * no error is reported.  Accuracy in bits of mantissa, the worst case over
 * the domain as tests/sdk/simdmath-fast-host-test.sh enforces it (with
 * 12-bit estimates), and the SPU arithmetic per call against the accurate
 * form's (constants aside):
 *
 *   sinf4fast     [-pi/2, pi/2]      >= 20    6 vs 24
 *   cosf4fast     [0, pi]            >= 20    8 vs 26    sin(pi/2 - x)
 *   sincosf4fast  [-pi/4, pi/4]      >= 20   10 vs 35
 *   tanf4fast     [-pi/4, pi/4]      >= 20   13 vs 29
 *   asinf4fast    [-1, 1]            >= 9    24 vs 32
 *   acosf4fast    [-1, 1]            >= 16   21 vs 25
 *   atanf4fast    all x              >= 17   23 vs 28
 *   atan2f4fast   all y, x           >= 17   30 vs 38
 *   expm1f4fast   (log 1/2, log 3/2) >= 20    9 vs 57    no range handling
 *   log1pf4fast   (-1/2, 1/2)        >= 20   14 vs 48    2 atanh(x / (2 + x))
 *   exp2f4fast    float range        >= 20   23 vs 26    SPU; no range checks
 *   expf4fast     float range        >= 20   23 vs 40    SPU
 *   log2f4fast    positive normals   >= 20   20 vs 31    SPU
 *   logf4fast, log10f4fast           >= 20   21 vs 30-33 SPU
 *   powf4fast     x in [1/16, 16], |y| <= 8
 *                                    >= 16   42 vs 67    exp2(y log2 x)
 *   sqrtf4fast    x >= 0             ~ 12     5 vs 7     x * rsqrt estimate
 *   cbrtf4fast                       the accurate cbrtf4
 *
 * On the PPU the exp and log forms are the VMX estimates vexptefp and
 * vlogefp (scaled for e and 10).  The polynomial tables are minimax fits
 * from sdk/libsimdmath/gen_fast_coeffs.py.
 *
 * The vector layer:
 *   __fm_vf / __fm_vu / __fm_vi   float, unsigned and signed int vectors
 *   __fm_splat(f)                 every lane f
 *   __fm_madd(a, b, c)            a * b + c
 *   __fm_nmsub(a, b, c)           c - a * b
 *   __fm_mul, __fm_add, __fm_sub
 *   __fm_sel(a, b, m)             lanes of b where m is set, else a
 *   __fm_cmpgt(a, b), __fm_cmpeq(a, b)   lane masks as __fm_vu
 *   __fm_re(x), __fm_rsqrte(x)    reciprocal / reciprocal square root
 *                                 estimates (about 12 bits)
 *   __fm_cts(x), __fm_ctf(i)      float to int (toward zero), int to float
 *   __fm_isplat(n), __fm_iadd, __fm_isub, __fm_sl(i, n), __fm_sr(u, n)
 *   __FM_VEC_ESTIMATES            defined on the PPU: exp2 / log2 use the
 *                                 VMX estimate instructions __fm_expte /
 *                                 __fm_loge
 */
#ifndef PS3TC_SIMDMATH_FASTF4_H
#define PS3TC_SIMDMATH_FASTF4_H

#define __FM_U(v) ((__fm_vu)(v))
#define __FM_F(v) ((__fm_vf)(v))
#define __FM_I(v) ((__fm_vi)(v))

static inline __fm_vu __fm_usplat(unsigned int n) { return __FM_U(__fm_isplat((int)n)); }
static inline __fm_vf __fm_and(__fm_vf a, __fm_vu m) { return __FM_F(__FM_U(a) & m); }
static inline __fm_vf __fm_or(__fm_vf a, __fm_vf b) { return __FM_F(__FM_U(a) | __FM_U(b)); }
static inline __fm_vf __fm_abs(__fm_vf x) { return __fm_and(x, __fm_usplat(0x7fffffffu)); }
static inline __fm_vf __fm_sign(__fm_vf x) { return __fm_and(x, __fm_usplat(0x80000000u)); }
/* |r| with the sign of s */
static inline __fm_vf __fm_withsign(__fm_vf r, __fm_vf s) { return __fm_or(__fm_abs(r), __fm_sign(s)); }

/* 1 / d to about 23 bits: the estimate and one Newton step */
static inline __fm_vf __fm_recip(__fm_vf d)
{
	__fm_vf r = __fm_re(d);
	return __fm_madd(r, __fm_nmsub(d, r, __fm_splat(1.0f)), r);
}
static inline __fm_vf __fm_div(__fm_vf n, __fm_vf d) { return __fm_mul(n, __fm_recip(d)); }

/* sqrt(z) for z >= 0 to about 23 bits (0 for z == 0) */
static inline __fm_vf __fm_sqrt(__fm_vf z)
{
	__fm_vf r = __fm_rsqrte(z);
	__fm_vf h = __fm_mul(__fm_splat(0.5f), z);
	/* r * (1.5 - h * r * r) */
	r = __fm_mul(r, __fm_nmsub(h, __fm_mul(r, r), __fm_splat(1.5f)));
	return __fm_sel(__fm_mul(z, r), __fm_splat(0.0f), __fm_cmpeq(z, __fm_splat(0.0f)));
}

/* floor(x) as integers, for |x| < 2^31 */
static inline __fm_vi __fm_floori(__fm_vf x)
{
	__fm_vi t = __fm_cts(x);
	__fm_vu below = __fm_cmpgt(__fm_ctf(t), x);
	return __FM_I(__fm_iadd(t, __FM_I(below)));   /* below is all ones: t - 1 */
}

/* 2^k for integer lanes k in [-126, 127] */
static inline __fm_vf __fm_pow2i(__fm_vi k)
{
	return __FM_F(__fm_sl(__fm_iadd(k, __fm_isplat(127)), 23));
}

/* x times the reciprocal square root estimate; 0 for x == 0 */
static inline __fm_vf __fm_sqrtf4fast(__fm_vf x)
{
	return __fm_sel(__fm_mul(x, __fm_rsqrte(x)), __fm_splat(0.0f), __fm_cmpeq(x, __fm_splat(0.0f)));
}

/* ---- sin / cos ----
 * minimax tables from sdk/libsimdmath/gen_fast_coeffs.py */

/* sin(x) for |x| <= pi/2 */
static inline __fm_vf __fm_sin_half(__fm_vf x)
{
	__fm_vf z = __fm_mul(x, x);
	__fm_vf p = __fm_splat(2.6019031338364584e-06f);
	p = __fm_madd(p, z, __fm_splat(-0.00019807419448625296f));
	p = __fm_madd(p, z, __fm_splat(0.008333025500178337f));
	p = __fm_madd(p, z, __fm_splat(-0.16666656732559204f));
	return __fm_madd(__fm_mul(p, z), x, x);
}

/* sin(x) for |x| <= pi/4 */
static inline __fm_vf __fm_sin_quarter(__fm_vf x)
{
	__fm_vf z = __fm_mul(x, x);
	__fm_vf p = __fm_splat(-0.00019501821952871978f);
	p = __fm_madd(p, z, __fm_splat(0.008332016877830029f));
	p = __fm_madd(p, z, __fm_splat(-0.16666650772094727f));
	return __fm_madd(__fm_mul(p, z), x, x);
}

/* cos(x) for |x| <= pi/4 */
static inline __fm_vf __fm_cos_quarter(__fm_vf x)
{
	__fm_vf z = __fm_mul(x, x);
	__fm_vf p = __fm_splat(-0.0013579403748735785f);
	p = __fm_madd(p, z, __fm_splat(0.04165441915392876f));
	p = __fm_madd(p, z, __fm_splat(-0.49999842047691345f));
	return __fm_madd(p, z, __fm_splat(0.9999999403953552f));
}

static inline __fm_vf __fm_sinf4fast(__fm_vf x) { return __fm_sin_half(x); }

static inline __fm_vf __fm_cosf4fast(__fm_vf x)
{
	/* pi/2 in two parts keeps cos accurate near pi/2 */
	__fm_vf t = __fm_add(__fm_sub(__fm_splat(1.57079637050628662109375f), x),
	                     __fm_splat(-4.37113900018624283e-8f));
	return __fm_sin_half(t);
}

static inline void __fm_sincosf4fast(__fm_vf x, __fm_vf *s, __fm_vf *c)
{
	*s = __fm_sin_quarter(x);
	*c = __fm_cos_quarter(x);
}

static inline __fm_vf __fm_tanf4fast(__fm_vf x)
{
	return __fm_div(__fm_sin_quarter(x), __fm_cos_quarter(x));
}

/* ---- inverse trigonometric ---- */

/* asin(x) for 0 <= x <= 1/2 */
static inline __fm_vf __fm_asin_core(__fm_vf x)
{
	__fm_vf z = __fm_mul(x, x);
	return __fm_mul(__fm_madd(__fm_splat(0.18865181505680084f), z, __fm_splat(0.9992669224739075f)), x);
}

static inline __fm_vf __fm_asinf4fast(__fm_vf x)
{
	/* above 1/2: pi/2 - 2 asin(sqrt((1 - |x|) / 2)) */
	__fm_vf a = __fm_abs(x);
	__fm_vu big = __fm_cmpgt(a, __fm_splat(0.5f));
	__fm_vf s = __fm_sqrt(__fm_mul(__fm_splat(0.5f), __fm_sub(__fm_splat(1.0f), a)));
	__fm_vf r = __fm_asin_core(__fm_sel(a, s, big));
	__fm_vf far = __fm_nmsub(__fm_splat(2.0f), r, __fm_splat(1.57079637050628662109375f));
	return __fm_withsign(__fm_sel(r, far, big), x);
}

/* acos(|x|) = sqrt(1 - |x|) G(|x|); x < 0: pi - acos(|x|) */
static inline __fm_vf __fm_acosf4fast(__fm_vf x)
{
	__fm_vf a = __fm_abs(x);
	__fm_vf p = __fm_splat(0.008591808378696442f);
	p = __fm_madd(p, a, __fm_splat(-0.03564343973994255f));
	p = __fm_madd(p, a, __fm_splat(0.08459656685590744f));
	p = __fm_madd(p, a, __fm_splat(-0.2141108214855194f));
	p = __fm_madd(p, a, __fm_splat(1.5707874298095703f));
	__fm_vf r = __fm_mul(__fm_sqrt(__fm_sub(__fm_splat(1.0f), a)), p);
	return __fm_sel(r, __fm_sub(__fm_splat(3.1415927410125732421875f), r), __fm_cmpgt(__fm_splat(0.0f), x));
}

/* atan(t) for 0 <= t <= 1 */
static inline __fm_vf __fm_atan_unit(__fm_vf t)
{
	__fm_vf z = __fm_mul(t, t);
	__fm_vf p = __fm_splat(-0.013480469584465027f);
	p = __fm_madd(p, z, __fm_splat(0.05747731402516365f));
	p = __fm_madd(p, z, __fm_splat(-0.121239073574543f));
	p = __fm_madd(p, z, __fm_splat(0.19563592970371246f));
	p = __fm_madd(p, z, __fm_splat(-0.33299461007118225f));
	p = __fm_madd(p, z, __fm_splat(0.9999956488609314f));
	return __fm_mul(p, t);
}

static inline __fm_vf __fm_atanf4fast(__fm_vf x)
{
	/* |x| > 1: pi/2 - atan(1 / |x|) */
	__fm_vf a = __fm_abs(x);
	__fm_vf one = __fm_splat(1.0f);
	__fm_vu big = __fm_cmpgt(a, one);
	__fm_vf t = __fm_div(__fm_sel(a, one, big), __fm_sel(one, a, big));
#ifndef __SPU__
	/* 1 / +Inf: the refinement of the estimate gives NaN; the answer is 0 */
	t = __fm_sel(t, __fm_splat(0.0f), __fm_cmpgt(a, __fm_splat(3.4028234e38f)));
#endif
	__fm_vf r = __fm_atan_unit(t);
	r = __fm_sel(r, __fm_sub(__fm_splat(1.57079637050628662109375f), r), big);
	return __fm_withsign(r, x);
}

static inline __fm_vf __fm_atan2f4fast(__fm_vf y, __fm_vf x)
{
	__fm_vf ay = __fm_abs(y), ax = __fm_abs(x);
	__fm_vu yBig = __fm_cmpgt(ay, ax);
	__fm_vf mn = __fm_sel(ay, ax, yBig), mx = __fm_sel(ax, ay, yBig);
	__fm_vf t = __fm_div(mn, mx);
	/* 0 / 0 is 0 */
	t = __fm_sel(t, __fm_splat(0.0f), __fm_cmpeq(mx, __fm_splat(0.0f)));
#ifndef __SPU__
	/* finite / Inf is 0, Inf / Inf is 1 */
	__fm_vu mxInf = __fm_cmpgt(mx, __fm_splat(3.4028234e38f));
	__fm_vu mnInf = __fm_cmpgt(mn, __fm_splat(3.4028234e38f));
	t = __fm_sel(t, __fm_sel(__fm_splat(0.0f), __fm_splat(1.0f), mnInf), mxInf);
#endif
	__fm_vf r = __fm_atan_unit(t);
	r = __fm_sel(r, __fm_sub(__fm_splat(1.57079637050628662109375f), r), yBig);
	/* x < 0 (or -0): pi - r */
	__fm_vu xNeg = __FM_U(__fm_isub(__fm_isplat(0), __FM_I(__fm_sr(__FM_U(x), 31))));
	r = __fm_sel(r, __fm_sub(__fm_splat(3.1415927410125732421875f), r), xNeg);
	return __fm_withsign(r, y);
}

/* ---- exponential and logarithm ---- */

/* e^r - 1 for |r| <= 0.7: series through r^9 */
static inline __fm_vf __fm_expm1_core(__fm_vf r)
{
	__fm_vf p = __fm_splat(1.0f / 362880.0f);
	p = __fm_madd(p, r, __fm_splat(1.0f / 40320.0f));
	p = __fm_madd(p, r, __fm_splat(1.0f / 5040.0f));
	p = __fm_madd(p, r, __fm_splat(1.0f / 720.0f));
	p = __fm_madd(p, r, __fm_splat(1.0f / 120.0f));
	p = __fm_madd(p, r, __fm_splat(1.0f / 24.0f));
	p = __fm_madd(p, r, __fm_splat(1.0f / 6.0f));
	p = __fm_madd(p, r, __fm_splat(0.5f));
	return __fm_madd(__fm_mul(p, r), r, r);
}

static inline __fm_vf __fm_expm1f4fast(__fm_vf x) { return __fm_expm1_core(x); }

/* 2 atanh(s) for |s| <= 1/3: odd series through s^13 */
static inline __fm_vf __fm_atanh2_core(__fm_vf s)
{
	__fm_vf s2 = __fm_mul(s, s);
	__fm_vf p = __fm_splat(2.0f / 13.0f);
	p = __fm_madd(p, s2, __fm_splat(2.0f / 11.0f));
	p = __fm_madd(p, s2, __fm_splat(2.0f / 9.0f));
	p = __fm_madd(p, s2, __fm_splat(2.0f / 7.0f));
	p = __fm_madd(p, s2, __fm_splat(2.0f / 5.0f));
	p = __fm_madd(p, s2, __fm_splat(2.0f / 3.0f));
	p = __fm_madd(p, s2, __fm_splat(2.0f));
	return __fm_mul(p, s);
}

static inline __fm_vf __fm_log1pf4fast(__fm_vf x)
{
	return __fm_atanh2_core(__fm_div(x, __fm_add(__fm_splat(2.0f), x)));
}

#ifndef __FM_VEC_ESTIMATES
/* log2(x) = k + log2(m), m in [sqrt(1/2), sqrt(2)), log2(m) = t G(t) with
 * t = m - 1 */
static inline __fm_vf __fm_log2_core(__fm_vf x)
{
	__fm_vu bits = __FM_U(x);
	__fm_vi e = __fm_isub(__FM_I(__fm_sr(bits, 23)), __fm_isplat(127));
	__fm_vf m = __FM_F((bits & __fm_usplat(0x007fffffu)) | __fm_usplat(0x3f800000u));
	__fm_vu high = __fm_cmpgt(m, __fm_splat(1.41421356237f));
	m = __fm_sel(m, __fm_mul(m, __fm_splat(0.5f)), high);
	e = __fm_isub(e, __FM_I(high));                /* high is all ones: e + 1 */
	__fm_vf t = __fm_sub(m, __fm_splat(1.0f));
	__fm_vf p = __fm_splat(-0.14581169188022614f);
	p = __fm_madd(p, t, __fm_splat(0.23404237627983093f));
	p = __fm_madd(p, t, __fm_splat(-0.24887686967849731f));
	p = __fm_madd(p, t, __fm_splat(0.2870987057685852f));
	p = __fm_madd(p, t, __fm_splat(-0.3602396249771118f));
	p = __fm_madd(p, t, __fm_splat(0.48092323541641235f));
	p = __fm_madd(p, t, __fm_splat(-0.7213528156280518f));
	p = __fm_madd(p, t, __fm_splat(1.442694902420044f));
	return __fm_madd(p, t, __fm_ctf(e));
}

static inline __fm_vf __fm_log2f4fast(__fm_vf x) { return __fm_log2_core(x); }
static inline __fm_vf __fm_logf4fast(__fm_vf x) { return __fm_mul(__fm_log2_core(x), __fm_splat(0.693147180559945f)); }
static inline __fm_vf __fm_log10f4fast(__fm_vf x) { return __fm_mul(__fm_log2_core(x), __fm_splat(0.30102999566398f)); }

/* 2^x: x = k + f, k = floor(x + 1/2), 2^f = e^(f ln 2); no range checks */
static inline __fm_vf __fm_exp2f4fast(__fm_vf x)
{
	__fm_vi k = __fm_floori(__fm_add(x, __fm_splat(0.5f)));
	__fm_vf f = __fm_sub(x, __fm_ctf(k));
	__fm_vf r = __fm_mul(f, __fm_splat(0.693147180559945f));
	return __fm_mul(__fm_add(__fm_expm1_core(r), __fm_splat(1.0f)), __fm_pow2i(k));
}

/* e^x: k = round(x / ln 2), r = x - k ln 2 in two parts; no range checks */
static inline __fm_vf __fm_expf4fast(__fm_vf x)
{
	__fm_vi k = __fm_floori(__fm_madd(x, __fm_splat(1.44269504088896f), __fm_splat(0.5f)));
	__fm_vf kf = __fm_ctf(k);
	__fm_vf r = __fm_nmsub(kf, __fm_splat(0.693145751953125f), x);
	r = __fm_nmsub(kf, __fm_splat(1.428606765330187e-6f), r);
	return __fm_mul(__fm_add(__fm_expm1_core(r), __fm_splat(1.0f)), __fm_pow2i(k));
}
#else
/* PPU: the VMX estimate instructions, scaled */
static inline __fm_vf __fm_exp2f4fast(__fm_vf x) { return __fm_expte(x); }
static inline __fm_vf __fm_expf4fast(__fm_vf x) { return __fm_expte(__fm_mul(x, __fm_splat(1.44269504088896f))); }
static inline __fm_vf __fm_log2f4fast(__fm_vf x) { return __fm_loge(x); }
static inline __fm_vf __fm_logf4fast(__fm_vf x) { return __fm_mul(__fm_loge(x), __fm_splat(0.693147180559945f)); }
static inline __fm_vf __fm_log10f4fast(__fm_vf x) { return __fm_mul(__fm_loge(x), __fm_splat(0.30102999566398f)); }
#endif

static inline __fm_vf __fm_powf4fast(__fm_vf x, __fm_vf y)
{
	return __fm_exp2f4fast(__fm_mul(y, __fm_log2f4fast(x)));
}

#undef __FM_U
#undef __FM_F
#undef __FM_I

#endif /* PS3TC_SIMDMATH_FASTF4_H */
