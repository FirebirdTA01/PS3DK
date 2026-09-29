/*
   simdmath.h - PS3 SDK umbrella for the libsimdmath SIMD math library.

   Makes the per-arch libsimdmath functions available as static inlines
   so reference-SDK samples that call sincosf4() / divf4() / rsqrtf4()
   link WITHOUT -lsimdmath.

   Mechanism (per the card):
     1. Pull in the public declarations (simdmath/simdmath.h) so code that
        only needs prototypes still works.
     2. For each function this arch has, include its inline header
        (simdmath/_name.h) which defines a static inline  _name .
     3. Add a function-like macro mapping the public name to the inline:
                #define sincosf4(...) _sincosf4(__VA_ARGS__)

   The macro is function-like on purpose: a value call  sincosf4(a,b)
   rewrites to the inline, but  &sincosf4  does NOT rewrite and still
   resolves to the libsimdmath.a symbol.  That keeps the out-of-line
   archive meaningful for take-address / -lsimdmath users while giving
   everyone else inlines.

   The PPU and SPU function sets differ, so the SPU-only group is gated
   on __SPU__.  (libsimdmath.a itself is built from the .c wrappers in
   sdk/libsimdmath/, which include the per-arch _name.h directly - they
   never include this umbrella, so there is no self-rewrite to worry
   about.)
*/

#ifndef PS3TC_SIMDMATH_UMBRELLA_H
#define PS3TC_SIMDMATH_UMBRELLA_H

#include <simdmath/simdmath.h>

#ifdef __SPU__
#include <spu_intrinsics.h>
#else
#include <altivec.h>
#endif

/* ---- inline implementations shared by PPU + SPU ---- */
#include <simdmath/_absi4.h>
#include <simdmath/_acosf4.h>
#include <simdmath/_asinf4.h>
#include <simdmath/_atan2f4.h>
#include <simdmath/_atanf4.h>
#include <simdmath/_cbrtf4.h>
#include <simdmath/_ceilf4.h>
#include <simdmath/_copysignf4.h>
#include <simdmath/_cosf4.h>
#include <simdmath/_divf4.h>
#include <simdmath/_divi4.h>
#include <simdmath/_exp2f4.h>
#include <simdmath/_expf4.h>
#include <simdmath/_expm1f4.h>
#include <simdmath/_fabsf4.h>
#include <simdmath/_fdimf4.h>
#include <simdmath/_floorf4.h>
#include <simdmath/_fmaf4.h>
#include <simdmath/_fmaxf4.h>
#include <simdmath/_fminf4.h>
#include <simdmath/_fmodf4.h>
#include <simdmath/_frexpf4.h>
#include <simdmath/_hypotf4.h>
#include <simdmath/_ilogbf4.h>
#include <simdmath/_ldexpf4.h>
#include <simdmath/_log10f4.h>
#include <simdmath/_log1pf4.h>
#include <simdmath/_log2f4.h>
#include <simdmath/_logbf4.h>
#include <simdmath/_logf4.h>
#include <simdmath/_modff4.h>
#include <simdmath/_negatef4.h>
#include <simdmath/_negatei4.h>
#include <simdmath/_powf4.h>
#include <simdmath/_recipf4.h>
#include <simdmath/_rsqrtf4.h>
#include <simdmath/_sincosf4.h>
#include <simdmath/_sinf4.h>
#include <simdmath/_sqrtf4.h>
#include <simdmath/_tanf4.h>
#include <simdmath/_truncf4.h>

/* ---- SPU-only inline implementations ---- */
#ifdef __SPU__
#include <simdmath/_ceild2.h>
#include <simdmath/_copysignd2.h>
#include <simdmath/_cosd2.h>
#include <simdmath/_divd2.h>
#include <simdmath/_divu4.h>
#include <simdmath/_fabsd2.h>
#include <simdmath/_fdimd2.h>
#include <simdmath/_floord2.h>
#include <simdmath/_fmad2.h>
#include <simdmath/_fmaxd2.h>
#include <simdmath/_fmind2.h>
#include <simdmath/_fmodd2.h>
#include <simdmath/_fpclassifyd2.h>
#include <simdmath/_fpclassifyf4.h>
#include <simdmath/_frexpd2.h>
#include <simdmath/_hypotd2.h>
#include <simdmath/_ilogbd2.h>
#include <simdmath/_irintf4.h>
#include <simdmath/_iroundf4.h>
#include <simdmath/_is0denormd2.h>
#include <simdmath/_is0denormf4.h>
#include <simdmath/_isequald2.h>
#include <simdmath/_isequalf4.h>
#include <simdmath/_isfinited2.h>
#include <simdmath/_isfinitef4.h>
#include <simdmath/_isgreaterd2.h>
#include <simdmath/_isgreaterequald2.h>
#include <simdmath/_isgreaterequalf4.h>
#include <simdmath/_isgreaterf4.h>
#include <simdmath/_isinfd2.h>
#include <simdmath/_isinff4.h>
#include <simdmath/_islessd2.h>
#include <simdmath/_islessequald2.h>
#include <simdmath/_islessequalf4.h>
#include <simdmath/_islessf4.h>
#include <simdmath/_islessgreaterd2.h>
#include <simdmath/_islessgreaterf4.h>
#include <simdmath/_isnand2.h>
#include <simdmath/_isnanf4.h>
#include <simdmath/_isnormald2.h>
#include <simdmath/_isnormalf4.h>
#include <simdmath/_isunorderedd2.h>
#include <simdmath/_isunorderedf4.h>
#include <simdmath/_ldexpd2.h>
#include <simdmath/_llabsi2.h>
#include <simdmath/_lldivi2.h>
#include <simdmath/_lldivu2.h>
#include <simdmath/_llrintd2.h>
#include <simdmath/_llrintf4.h>
#include <simdmath/_llroundd2.h>
#include <simdmath/_llroundf4.h>
#include <simdmath/_logbd2.h>
#include <simdmath/_modfd2.h>
#include <simdmath/_nearbyintd2.h>
#include <simdmath/_nearbyintf4.h>
#include <simdmath/_negated2.h>
#include <simdmath/_negatell2.h>
#include <simdmath/_nextafterd2.h>
#include <simdmath/_nextafterf4.h>
#include <simdmath/_recipd2.h>
#include <simdmath/_remainderd2.h>
#include <simdmath/_remainderf4.h>
#include <simdmath/_remquod2.h>
#include <simdmath/_remquof4.h>
#include <simdmath/_rintd2.h>
#include <simdmath/_rintf4.h>
#include <simdmath/_roundd2.h>
#include <simdmath/_roundf4.h>
#include <simdmath/_rsqrtd2.h>
#include <simdmath/_scalbllnd2.h>
#include <simdmath/_scalbnf4.h>
#include <simdmath/_signbitd2.h>
#include <simdmath/_signbitf4.h>
#include <simdmath/_sincosd2.h>
#include <simdmath/_sind2.h>
#include <simdmath/_sqrtd2.h>
#include <simdmath/_tand2.h>
#include <simdmath/_truncd2.h>
#endif /* __SPU__ */

/* ---- shared public-name aliases (function-like: value call inlines, &name keeps the library symbol) ---- */
#define absi4(...) _absi4(__VA_ARGS__)
#define acosf4(...) _acosf4(__VA_ARGS__)
#define asinf4(...) _asinf4(__VA_ARGS__)
#define atan2f4(...) _atan2f4(__VA_ARGS__)
#define atanf4(...) _atanf4(__VA_ARGS__)
#define cbrtf4(...) _cbrtf4(__VA_ARGS__)
#define ceilf4(...) _ceilf4(__VA_ARGS__)
#define copysignf4(...) _copysignf4(__VA_ARGS__)
#define cosf4(...) _cosf4(__VA_ARGS__)
#define divf4(...) _divf4(__VA_ARGS__)
#define divi4(...) _divi4(__VA_ARGS__)
#define exp2f4(...) _exp2f4(__VA_ARGS__)
#define expf4(...) _expf4(__VA_ARGS__)
#define expm1f4(...) _expm1f4(__VA_ARGS__)
#define fabsf4(...) _fabsf4(__VA_ARGS__)
#define fdimf4(...) _fdimf4(__VA_ARGS__)
#define floorf4(...) _floorf4(__VA_ARGS__)
#define fmaf4(...) _fmaf4(__VA_ARGS__)
#define fmaxf4(...) _fmaxf4(__VA_ARGS__)
#define fminf4(...) _fminf4(__VA_ARGS__)
#define fmodf4(...) _fmodf4(__VA_ARGS__)
#define frexpf4(...) _frexpf4(__VA_ARGS__)
#define hypotf4(...) _hypotf4(__VA_ARGS__)
#define ilogbf4(...) _ilogbf4(__VA_ARGS__)
#define ldexpf4(...) _ldexpf4(__VA_ARGS__)
#define log10f4(...) _log10f4(__VA_ARGS__)
#define log1pf4(...) _log1pf4(__VA_ARGS__)
#define log2f4(...) _log2f4(__VA_ARGS__)
#define logbf4(...) _logbf4(__VA_ARGS__)
#define logf4(...) _logf4(__VA_ARGS__)
#define modff4(...) _modff4(__VA_ARGS__)
#define negatef4(...) _negatef4(__VA_ARGS__)
#define negatei4(...) _negatei4(__VA_ARGS__)
#define powf4(...) _powf4(__VA_ARGS__)
#define recipf4(...) _recipf4(__VA_ARGS__)
#define rsqrtf4(...) _rsqrtf4(__VA_ARGS__)
#define sincosf4(...) _sincosf4(__VA_ARGS__)
#define sinf4(...) _sinf4(__VA_ARGS__)
#define sqrtf4(...) _sqrtf4(__VA_ARGS__)
#define tanf4(...) _tanf4(__VA_ARGS__)
#define truncf4(...) _truncf4(__VA_ARGS__)

/* ---- SPU-only public-name aliases ---- */
#ifdef __SPU__
#define ceild2(...) _ceild2(__VA_ARGS__)
#define copysignd2(...) _copysignd2(__VA_ARGS__)
#define cosd2(...) _cosd2(__VA_ARGS__)
#define divd2(...) _divd2(__VA_ARGS__)
#define divu4(...) _divu4(__VA_ARGS__)
#define fabsd2(...) _fabsd2(__VA_ARGS__)
#define fdimd2(...) _fdimd2(__VA_ARGS__)
#define floord2(...) _floord2(__VA_ARGS__)
#define fmad2(...) _fmad2(__VA_ARGS__)
#define fmaxd2(...) _fmaxd2(__VA_ARGS__)
#define fmind2(...) _fmind2(__VA_ARGS__)
#define fmodd2(...) _fmodd2(__VA_ARGS__)
#define fpclassifyd2(...) _fpclassifyd2(__VA_ARGS__)
#define fpclassifyf4(...) _fpclassifyf4(__VA_ARGS__)
#define frexpd2(...) _frexpd2(__VA_ARGS__)
#define hypotd2(...) _hypotd2(__VA_ARGS__)
#define ilogbd2(...) _ilogbd2(__VA_ARGS__)
#define irintf4(...) _irintf4(__VA_ARGS__)
#define iroundf4(...) _iroundf4(__VA_ARGS__)
#define is0denormd2(...) _is0denormd2(__VA_ARGS__)
#define is0denormf4(...) _is0denormf4(__VA_ARGS__)
#define isequald2(...) _isequald2(__VA_ARGS__)
#define isequalf4(...) _isequalf4(__VA_ARGS__)
#define isfinited2(...) _isfinited2(__VA_ARGS__)
#define isfinitef4(...) _isfinitef4(__VA_ARGS__)
#define isgreaterd2(...) _isgreaterd2(__VA_ARGS__)
#define isgreaterequald2(...) _isgreaterequald2(__VA_ARGS__)
#define isgreaterequalf4(...) _isgreaterequalf4(__VA_ARGS__)
#define isgreaterf4(...) _isgreaterf4(__VA_ARGS__)
#define isinfd2(...) _isinfd2(__VA_ARGS__)
#define isinff4(...) _isinff4(__VA_ARGS__)
#define islessd2(...) _islessd2(__VA_ARGS__)
#define islessequald2(...) _islessequald2(__VA_ARGS__)
#define islessequalf4(...) _islessequalf4(__VA_ARGS__)
#define islessf4(...) _islessf4(__VA_ARGS__)
#define islessgreaterd2(...) _islessgreaterd2(__VA_ARGS__)
#define islessgreaterf4(...) _islessgreaterf4(__VA_ARGS__)
#define isnand2(...) _isnand2(__VA_ARGS__)
#define isnanf4(...) _isnanf4(__VA_ARGS__)
#define isnormald2(...) _isnormald2(__VA_ARGS__)
#define isnormalf4(...) _isnormalf4(__VA_ARGS__)
#define isunorderedd2(...) _isunorderedd2(__VA_ARGS__)
#define isunorderedf4(...) _isunorderedf4(__VA_ARGS__)
#define ldexpd2(...) _ldexpd2(__VA_ARGS__)
#define llabsi2(...) _llabsi2(__VA_ARGS__)
#define lldivi2(...) _lldivi2(__VA_ARGS__)
#define lldivu2(...) _lldivu2(__VA_ARGS__)
#define llrintd2(...) _llrintd2(__VA_ARGS__)
#define llrintf4(...) _llrintf4(__VA_ARGS__)
#define llroundd2(...) _llroundd2(__VA_ARGS__)
#define llroundf4(...) _llroundf4(__VA_ARGS__)
#define logbd2(...) _logbd2(__VA_ARGS__)
#define modfd2(...) _modfd2(__VA_ARGS__)
#define nearbyintd2(...) _nearbyintd2(__VA_ARGS__)
#define nearbyintf4(...) _nearbyintf4(__VA_ARGS__)
#define negated2(...) _negated2(__VA_ARGS__)
#define negatell2(...) _negatell2(__VA_ARGS__)
#define nextafterd2(...) _nextafterd2(__VA_ARGS__)
#define nextafterf4(...) _nextafterf4(__VA_ARGS__)
#define recipd2(...) _recipd2(__VA_ARGS__)
#define remainderd2(...) _remainderd2(__VA_ARGS__)
#define remainderf4(...) _remainderf4(__VA_ARGS__)
#define remquod2(...) _remquod2(__VA_ARGS__)
#define remquof4(...) _remquof4(__VA_ARGS__)
#define rintd2(...) _rintd2(__VA_ARGS__)
#define rintf4(...) _rintf4(__VA_ARGS__)
#define roundd2(...) _roundd2(__VA_ARGS__)
#define roundf4(...) _roundf4(__VA_ARGS__)
#define rsqrtd2(...) _rsqrtd2(__VA_ARGS__)
#define scalbllnd2(...) _scalbllnd2(__VA_ARGS__)
#define scalbnf4(...) _scalbnf4(__VA_ARGS__)
#define signbitd2(...) _signbitd2(__VA_ARGS__)
#define signbitf4(...) _signbitf4(__VA_ARGS__)
#define sincosd2(...) _sincosd2(__VA_ARGS__)
#define sind2(...) _sind2(__VA_ARGS__)
#define sqrtd2(...) _sqrtd2(__VA_ARGS__)
#define tand2(...) _tand2(__VA_ARGS__)
#define truncd2(...) _truncd2(__VA_ARGS__)
#endif /* __SPU__ */

/* ---- fast forms: reference-SDK 'f4fast' aliases, each mapped to the accurate inline ---- */
/* (no separate algorithm; the 'fast' path is the same polynomial/estimate) */
#define acosf4fast(...) _acosf4(__VA_ARGS__)
#define asinf4fast(...) _asinf4(__VA_ARGS__)
#define atan2f4fast(...) _atan2f4(__VA_ARGS__)
#define atanf4fast(...) _atanf4(__VA_ARGS__)
#define cbrtf4fast(...) _cbrtf4(__VA_ARGS__)
#define cosf4fast(...) _cosf4(__VA_ARGS__)
#define divf4fast(...) _divf4(__VA_ARGS__)
#define exp2f4fast(...) _exp2f4(__VA_ARGS__)
#define expf4fast(...) _expf4(__VA_ARGS__)
#define expm1f4fast(...) _expm1f4(__VA_ARGS__)
#define log10f4fast(...) _log10f4(__VA_ARGS__)
#define log1pf4fast(...) _log1pf4(__VA_ARGS__)
#define log2f4fast(...) _log2f4(__VA_ARGS__)
#define logf4fast(...) _logf4(__VA_ARGS__)
#define powf4fast(...) _powf4(__VA_ARGS__)
#define recipf4fast(...) _recipf4(__VA_ARGS__)
#define rsqrtf4fast(...) _rsqrtf4(__VA_ARGS__)
#define sincosf4fast(...) _sincosf4(__VA_ARGS__)
#define sinf4fast(...) _sinf4(__VA_ARGS__)
#define sqrtf4fast(...) _sqrtf4(__VA_ARGS__)
#define tanf4fast(...) _tanf4(__VA_ARGS__)

/* ---- SPU two-lane double comparisons (new; thin over the existing is* d2 primitives) ---- */
/*     cmpeqd2(x,y)       == isequal(x,y)   */
/*     cmpged2(x,y)       == !(x<y) == islessequal(y,x)   */
/*     cmpgtd2(x,y)       == isgreater(x,y)   */
/*     cmpinfd2(x)        == isinf(x)   */
/*     cmpnand2(x)        == isnan(x)   */
/*     cmpnegsignd2(x)    == signbit(x)   */
/*     cmpzerodenormd2(x) == zero_or_denorm(x)   */
/* SPU-only because the PPU side of libsimdmath has no double lane. */
#ifdef __SPU__
static inline vector unsigned long long
cmpeqd2      (vector double x, vector double y) { return _isequald2    (x, y); }
static inline vector unsigned long long
cmpged2      (vector double x, vector double y) { return _islessequald2(y, x); }
static inline vector unsigned long long
cmpgtd2      (vector double x, vector double y) { return _isgreaterd2  (x, y); }
static inline vector unsigned long long
cmpinfd2     (vector double x)                   { return _isinfd2      (x); }
static inline vector unsigned long long
cmpnand2     (vector double x)                   { return _isnand2      (x); }
static inline vector unsigned long long
cmpnegsignd2 (vector double x)                   { return _signbitd2    (x); }
static inline vector unsigned long long
cmpzerodenormd2(vector double x)                 { return _is0denormd2  (x); }
#endif /* __SPU__ */

#endif /* PS3TC_SIMDMATH_UMBRELLA_H */
