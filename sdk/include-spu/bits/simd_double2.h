// double2 - 2-lane double SIMD type, backed by vec_double2.

#ifndef PS3TC_SPU_SIMD_DOUBLE2_H
#define PS3TC_SPU_SIMD_DOUBLE2_H

#include <spu_intrinsics.h>
#include <simdmath/simdmath.h>     // divd2
#include "simd_scalars.h"

namespace simd {

class llong2;
class ullong2;
class bool2;
class double2_idx;

class double2
{
private:
    vec_double2 data;

public:
    inline double2() {}
    inline double2(llong2);
    inline double2(ullong2);
    inline double2(bool2);
    inline double2(double r0, double r1);

    explicit inline double2(double);

    inline double2(vec_double2 rhs);
    inline operator vector double() const;

    inline double2_idx operator [] (int i);
    inline double       operator [] (int i) const;

    inline const double2 operator ++ (int);
    inline const double2 operator -- (int);
    inline double2& operator ++ ();
    inline double2& operator -- ();

    inline const double2 operator - () const;

    inline double2& operator =  (double2 rhs);
    inline double2& operator *= (double2 rhs);
    inline double2& operator /= (double2 rhs);
    inline double2& operator += (double2 rhs);
    inline double2& operator -= (double2 rhs);
};

inline const double2 operator * (double2 lhs, double2 rhs);
inline const double2 operator / (double2 lhs, double2 rhs);
inline const double2 operator + (double2 lhs, double2 rhs);
inline const double2 operator - (double2 lhs, double2 rhs);

inline const bool2 operator < (double2 lhs, double2 rhs);
inline const bool2 operator <= (double2 lhs, double2 rhs);
inline const bool2 operator > (double2 lhs, double2 rhs);
inline const bool2 operator >= (double2 lhs, double2 rhs);
inline const bool2 operator == (double2 lhs, double2 rhs);
inline const bool2 operator != (double2 lhs, double2 rhs);

inline const double2 select(double2 lhs, double2 rhs, bool2 rhs_slots);

class double2_idx
{
private:
    double2 &ref __attribute__((aligned(16)));
    int      i   __attribute__((aligned(16)));
public:
    inline double2_idx(double2& vec, int idx) : ref(vec) { i = idx; }
    inline operator double() const;
    inline double operator = (double rhs);
    inline double operator = (const double2_idx& rhs);
    inline double operator ++ (int);
    inline double operator -- (int);
    inline double operator ++ ();
    inline double operator -- ();
    inline double operator *= (double rhs);
    inline double operator /= (double rhs);
    inline double operator += (double rhs);
    inline double operator -= (double rhs);
};

}  // namespace simd

#include "simd_llong2.h"
#include "simd_ullong2.h"
#include "simd_bool2.h"

namespace simd {

inline double2::double2(double rhs)        { data = spu_splats((double)rhs); }
inline double2::double2(vec_double2 rhs)   { data = rhs; }

inline double2::double2(llong2 rhs)
{
    data = (vec_double2){
        (double)spu_extract((vec_llong2)rhs, 0),
        (double)spu_extract((vec_llong2)rhs, 1)
    };
}

inline double2::double2(ullong2 rhs)
{
    data = (vec_double2){
        (double)spu_extract((vec_ullong2)rhs, 0),
        (double)spu_extract((vec_ullong2)rhs, 1)
    };
}

inline double2::double2(bool2 rhs)
{
    *this = double2(llong2(rhs));
}

inline double2::double2(double r0, double r1)
{
    data = (vec_double2){ r0, r1 };
}

inline double2::operator vector double() const { return data; }

inline double2_idx double2::operator [] (int i) { return double2_idx(*this, i); }
inline double      double2::operator [] (int i) const
{
    return spu_extract(data, i);
}

inline const double2 double2::operator ++ (int) { vec_double2 p = data; operator ++(); return double2(p); }
inline const double2 double2::operator -- (int) { vec_double2 p = data; operator --(); return double2(p); }
inline double2& double2::operator ++ () { *this += double2(1.0); return *this; }
inline double2& double2::operator -- () { *this -= double2(1.0); return *this; }

inline const double2 double2::operator - () const
{
    return double2((vec_double2)spu_xor((vec_ullong2)data,
                                        spu_splats((ullong)0x8000000000000000ull)));
}

inline double2& double2::operator =  (double2 rhs) { data = rhs.data; return *this; }
inline double2& double2::operator *= (double2 rhs) { *this = *this * rhs; return *this; }
inline double2& double2::operator /= (double2 rhs) { *this = *this / rhs; return *this; }
inline double2& double2::operator += (double2 rhs) { *this = *this + rhs; return *this; }
inline double2& double2::operator -= (double2 rhs) { *this = *this - rhs; return *this; }

inline const double2 operator * (double2 lhs, double2 rhs)
{
    return double2(spu_mul((vec_double2)lhs, (vec_double2)rhs));
}

inline const double2 operator / (double2 num, double2 den)
{
    return double2(divd2((vec_double2)num, (vec_double2)den));
}

inline const double2 operator + (double2 lhs, double2 rhs)
{
    return double2(spu_add((vec_double2)lhs, (vec_double2)rhs));
}

inline const double2 operator - (double2 lhs, double2 rhs)
{
    return double2(spu_sub((vec_double2)lhs, (vec_double2)rhs));
}

inline const bool2 operator > (double2 lhs, double2 rhs)
{
    bool2 r;
    r.data = (vec_ullong2)spu_cmpgt((vec_double2)lhs, (vec_double2)rhs);
    return r;
}

inline const bool2 operator < (double2 lhs, double2 rhs)
{
    bool2 r;
    r.data = (vec_ullong2)spu_cmpgt((vec_double2)rhs, (vec_double2)lhs);
    return r;
}

inline const bool2 operator >= (double2 lhs, double2 rhs)
{
    // a >= b == !(b > a) — but unordered (NaN) handling differs: SPU
    // dfcgt returns 0 when either operand is NaN.  cmpged2 in libsimdmath
    // handles those edge cases; this fallback matches the SPU hardware.
    return !(rhs > lhs);
}

inline const bool2 operator <= (double2 lhs, double2 rhs)
{
    return !(lhs > rhs);
}

inline const bool2 operator == (double2 lhs, double2 rhs)
{
    bool2 r;
    r.data = (vec_ullong2)spu_cmpeq((vec_double2)lhs, (vec_double2)rhs);
    return r;
}

inline const bool2 operator != (double2 lhs, double2 rhs) { return !(lhs == rhs); }

inline const double2 select(double2 lhs, double2 rhs, bool2 rhs_slots)
{
    return double2(spu_sel((vec_double2)lhs, (vec_double2)rhs, (vec_ullong2)rhs_slots));
}

inline double2_idx::operator double() const
{
    return spu_extract((vec_double2)ref, i);
}

inline double double2_idx::operator = (double rhs)
{
    ref = spu_insert(rhs, (vec_double2)ref, i);
    return rhs;
}

inline double double2_idx::operator = (const double2_idx& rhs)
{
    return *this = (double)rhs;
}

inline double double2_idx::operator ++ (int)
{
    double t = spu_extract((vec_double2)ref, i);
    ref = spu_insert(t + 1.0, (vec_double2)ref, i);
    return t;
}

inline double double2_idx::operator -- (int)
{
    double t = spu_extract((vec_double2)ref, i);
    ref = spu_insert(t - 1.0, (vec_double2)ref, i);
    return t;
}

inline double double2_idx::operator ++ ()
{
    double t = spu_extract((vec_double2)ref, i) + 1.0;
    ref = spu_insert(t, (vec_double2)ref, i);
    return t;
}

inline double double2_idx::operator -- ()
{
    double t = spu_extract((vec_double2)ref, i) - 1.0;
    ref = spu_insert(t, (vec_double2)ref, i);
    return t;
}

inline double double2_idx::operator *= (double rhs)
{
    double t = spu_extract((vec_double2)ref, i) * rhs;
    ref = spu_insert(t, (vec_double2)ref, i);
    return t;
}

inline double double2_idx::operator /= (double rhs)
{
    double t = spu_extract((vec_double2)ref, i) / rhs;
    ref = spu_insert(t, (vec_double2)ref, i);
    return t;
}

inline double double2_idx::operator += (double rhs)
{
    double t = spu_extract((vec_double2)ref, i) + rhs;
    ref = spu_insert(t, (vec_double2)ref, i);
    return t;
}

inline double double2_idx::operator -= (double rhs)
{
    double t = spu_extract((vec_double2)ref, i) - rhs;
    ref = spu_insert(t, (vec_double2)ref, i);
    return t;
}

}  // namespace simd

#endif
