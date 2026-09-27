/* Separate translation unit: the caller must not fold these comparisons. */
#include "abi.h"
#define NOINLINE __attribute__((noinline, noclone))
#define DEFINE(P, T, K) \
NOINLINE int P##_plus_eq(const T *p, int n) { int r=0; for(int i=0;i<n;++i) r+=(p[i]==(K)); return r; } \
NOINLINE int P##_plus_ne(const T *p, int n) { int r=0; for(int i=0;i<n;++i) r+=(p[i]!=(K)); return r; } \
NOINLINE int P##_minus_eq(const T *p, int n) { int r=0; for(int i=0;i<n;++i) r-=(p[i]==(K)); return r; } \
NOINLINE int P##_minus_ne(const T *p, int n) { int r=0; for(int i=0;i<n;++i) r-=(p[i]!=(K)); return r; } \
NOINLINE int P##_neg_eq(const T *p, int n) { int r=0; for(int i=0;i<n;++i) r|=-(p[i]==(K)); return r; } \
NOINLINE int P##_neg_ne(const T *p, int n) { int r=0; for(int i=0;i<n;++i) r|=-(p[i]!=(K)); return r; } \
NOINLINE int P##_ne(const T *p, int n) { int r=0; for(int i=0;i<n;++i) r|=(p[i]!=(K)); return r; }

DEFINE(u32_high, unsigned int, 0xff102030u)
DEFINE(i32_high, int, -15720400)
DEFINE(u32_low, unsigned int, 0x102030u)
DEFINE(u64_high, unsigned long long, 0x12345678ff102030ull)

/* Register operands: compare separately loaded values rather than a constant. */
NOINLINE int register_ne(const unsigned int *a, const unsigned int *b, int n)
{
    int r=0;
    for(int i=0;i<n;++i) r+=(a[i]!=b[i]);
    return r;
}
