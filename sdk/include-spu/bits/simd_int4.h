// int4 - 4-lane signed-int SIMD type, backed by vec_int4.

#ifndef PS3TC_SPU_SIMD_INT4_H
#define PS3TC_SPU_SIMD_INT4_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool4;
class uint4;
class float4;
class int4_idx;

class int4
{
private:
    vec_int4 data;

public:
    inline int4() {}
    inline int4(bool4);
    inline int4(uint4);
    inline int4(float4);
    inline int4(int r0, int r1, int r2, int r3);

    explicit inline int4(int);

    inline int4(vec_int4 rhs);
    inline operator vector signed int() const;

    inline int4_idx operator [] (int i);
    inline int       operator [] (int i) const;

    inline const int4 operator ++ (int);
    inline const int4 operator -- (int);
    inline int4& operator ++ ();
    inline int4& operator -- ();

    inline const int4  operator ~ () const;
    inline const bool4 operator ! () const;
    inline const int4  operator - () const;

    inline int4& operator =  (int4 rhs);
    inline int4& operator *= (int4 rhs);
    inline int4& operator /= (int4 rhs);
    inline int4& operator %= (int4 rhs);
    inline int4& operator += (int4 rhs);
    inline int4& operator -= (int4 rhs);
    inline int4& operator <<= (uint4 rhs);
    inline int4& operator >>= (uint4 rhs);
    inline int4& operator &= (int4 rhs);
    inline int4& operator ^= (int4 rhs);
    inline int4& operator |= (int4 rhs);
};

inline const int4 operator * (int4 lhs, int4 rhs);
inline const int4 operator / (int4 lhs, int4 rhs);
inline const int4 operator % (int4 lhs, int4 rhs);
inline const int4 operator + (int4 lhs, int4 rhs);
inline const int4 operator - (int4 lhs, int4 rhs);

inline const int4 operator << (int4 lhs, uint4 rhs);
inline const int4 operator >> (int4 lhs, uint4 rhs);

inline const bool4 operator < (int4 lhs, int4 rhs);
inline const bool4 operator <= (int4 lhs, int4 rhs);
inline const bool4 operator > (int4 lhs, int4 rhs);
inline const bool4 operator >= (int4 lhs, int4 rhs);
inline const bool4 operator == (int4 lhs, int4 rhs);
inline const bool4 operator != (int4 lhs, int4 rhs);

inline const int4 select(int4 lhs, int4 rhs, bool4 rhs_slots);

inline const int4 operator & (int4 lhs, int4 rhs);
inline const int4 operator ^ (int4 lhs, int4 rhs);
inline const int4 operator | (int4 lhs, int4 rhs);

class int4_idx
{
private:
    int4 &ref __attribute__((aligned(16)));
    int   i   __attribute__((aligned(16)));
public:
    inline int4_idx(int4& vec, int idx) : ref(vec) { i = idx; }
    inline operator int() const;
    inline int operator = (int rhs);
    inline int operator = (const int4_idx& rhs);
    inline int operator ++ (int);
    inline int operator -- (int);
    inline int operator ++ ();
    inline int operator -- ();
    inline int operator *= (int rhs);
    inline int operator /= (int rhs);
    inline int operator %= (int rhs);
    inline int operator += (int rhs);
    inline int operator -= (int rhs);
    inline int operator <<= (uint rhs);
    inline int operator >>= (uint rhs);
    inline int operator &= (int rhs);
    inline int operator ^= (int rhs);
    inline int operator |= (int rhs);
};

}  // namespace simd

#include "simd_bool4.h"
#include "simd_uint4.h"
#include "simd_float4.h"

namespace simd {

inline int4::int4(int rhs)         { data = spu_splats((int)rhs); }
inline int4::int4(vec_int4 rhs)    { data = rhs; }

inline int4::int4(bool4 rhs)
{
    // bool4's mask is all-ones for true; a logical 1 is the natural
    // integer projection.
    data = (vec_int4)spu_and((vec_uint4)rhs, 1u);
}

inline int4::int4(uint4 rhs)
{
    data = (vec_int4)(vec_uint4)rhs;
}

inline int4::int4(float4 rhs)
{
    data = spu_convts((vec_float4)rhs, 0);
}

inline int4::int4(int r0, int r1, int r2, int r3)
{
    data = (vec_int4){ r0, r1, r2, r3 };
}

inline int4::operator vector signed int() const { return data; }

inline int4_idx int4::operator [] (int i) { return int4_idx(*this, i); }
inline int      int4::operator [] (int i) const
{
    return (int)spu_extract(data, i);
}

inline const int4 int4::operator ++ (int)
{
    vec_int4 prev = data;
    operator ++ ();
    return int4(prev);
}

inline const int4 int4::operator -- (int)
{
    vec_int4 prev = data;
    operator -- ();
    return int4(prev);
}

inline int4& int4::operator ++ () { *this += int4(1); return *this; }
inline int4& int4::operator -- () { *this -= int4(1); return *this; }

inline const int4  int4::operator ~ () const { return int4(spu_nor(data, data)); }
inline const bool4 int4::operator ! () const { return *this == int4(0); }
inline const int4  int4::operator - () const { return int4(spu_sub(0, data)); }

inline int4& int4::operator =  (int4 rhs) { data = rhs.data; return *this; }
inline int4& int4::operator *= (int4 rhs) { *this = *this *  rhs; return *this; }
inline int4& int4::operator /= (int4 rhs) { *this = *this /  rhs; return *this; }
inline int4& int4::operator %= (int4 rhs) { *this = *this %  rhs; return *this; }
inline int4& int4::operator += (int4 rhs) { *this = *this +  rhs; return *this; }
inline int4& int4::operator -= (int4 rhs) { *this = *this -  rhs; return *this; }
inline int4& int4::operator <<= (uint4 rhs) { *this = *this << rhs; return *this; }
inline int4& int4::operator >>= (uint4 rhs) { *this = *this >> rhs; return *this; }
inline int4& int4::operator &= (int4 rhs) { *this = *this & rhs; return *this; }
inline int4& int4::operator ^= (int4 rhs) { *this = *this ^ rhs; return *this; }
inline int4& int4::operator |= (int4 rhs) { *this = *this | rhs; return *this; }

inline const int4 operator * (int4 lhs, int4 rhs)
{
    // SPU lacks a native 32x32->32 multiply.  Build it from the three
    // 16x16 partial products: hi*lo + lo*hi (shifted into the high
    // half by mulh), plus lo*lo as the low half.
    vec_short8 a = (vec_short8)(vec_int4)lhs;
    vec_short8 b = (vec_short8)(vec_int4)rhs;
    vec_int4 hl = spu_mulh(a, b);
    vec_int4 lh = spu_mulh(b, a);
    vec_int4 ll = (vec_int4)spu_mulo((vec_ushort8)a, (vec_ushort8)b);
    return int4(spu_add(spu_add(hl, lh), ll));
}

inline const int4 operator / (int4 num, int4 den)
{
    // Scalar fallback: 4 lane-wise extracts.
    int q0 = spu_extract((vec_int4)num, 0) / spu_extract((vec_int4)den, 0);
    int q1 = spu_extract((vec_int4)num, 1) / spu_extract((vec_int4)den, 1);
    int q2 = spu_extract((vec_int4)num, 2) / spu_extract((vec_int4)den, 2);
    int q3 = spu_extract((vec_int4)num, 3) / spu_extract((vec_int4)den, 3);
    return int4(q0, q1, q2, q3);
}

inline const int4 operator % (int4 num, int4 den)
{
    int r0 = spu_extract((vec_int4)num, 0) % spu_extract((vec_int4)den, 0);
    int r1 = spu_extract((vec_int4)num, 1) % spu_extract((vec_int4)den, 1);
    int r2 = spu_extract((vec_int4)num, 2) % spu_extract((vec_int4)den, 2);
    int r3 = spu_extract((vec_int4)num, 3) % spu_extract((vec_int4)den, 3);
    return int4(r0, r1, r2, r3);
}

inline const int4 operator + (int4 lhs, int4 rhs)
{
    return int4(spu_add((vec_int4)lhs, (vec_int4)rhs));
}

inline const int4 operator - (int4 lhs, int4 rhs)
{
    return int4(spu_sub((vec_int4)lhs, (vec_int4)rhs));
}

inline const int4 operator << (int4 lhs, uint4 rhs)
{
    return int4(spu_sl((vec_int4)lhs, (vec_uint4)rhs));
}

inline const int4 operator >> (int4 lhs, uint4 rhs)
{
    // Arithmetic right-shift via rlmaska with negated count.
    return int4(spu_rlmaska((vec_int4)lhs, (vec_int4)spu_sub(0u, (vec_uint4)rhs)));
}

inline const bool4 operator < (int4 lhs, int4 rhs)
{
    bool4 r;
    r.data = spu_cmpgt((vec_int4)rhs, (vec_int4)lhs);
    return r;
}

inline const bool4 operator <= (int4 lhs, int4 rhs) { return !(lhs > rhs); }

inline const bool4 operator > (int4 lhs, int4 rhs)
{
    bool4 r;
    r.data = spu_cmpgt((vec_int4)lhs, (vec_int4)rhs);
    return r;
}

inline const bool4 operator >= (int4 lhs, int4 rhs) { return !(lhs < rhs); }

inline const bool4 operator == (int4 lhs, int4 rhs)
{
    bool4 r;
    r.data = spu_cmpeq((vec_int4)lhs, (vec_int4)rhs);
    return r;
}

inline const bool4 operator != (int4 lhs, int4 rhs) { return !(lhs == rhs); }

inline const int4 select(int4 lhs, int4 rhs, bool4 rhs_slots)
{
    return int4(spu_sel((vec_int4)lhs, (vec_int4)rhs, (vec_uint4)rhs_slots));
}

inline const int4 operator & (int4 lhs, int4 rhs)
{
    return int4(spu_and((vec_int4)lhs, (vec_int4)rhs));
}

inline const int4 operator | (int4 lhs, int4 rhs)
{
    return int4(spu_or((vec_int4)lhs, (vec_int4)rhs));
}

inline const int4 operator ^ (int4 lhs, int4 rhs)
{
    return int4(spu_xor((vec_int4)lhs, (vec_int4)rhs));
}

inline int4_idx::operator int() const
{
    return (int)spu_extract((vec_int4)ref, i);
}

inline int int4_idx::operator = (int rhs)
{
    ref = spu_insert(rhs, (vec_int4)ref, i);
    return rhs;
}

inline int int4_idx::operator = (const int4_idx& rhs)
{
    return *this = (int)rhs;
}

inline int int4_idx::operator ++ (int)
{
    int t = spu_extract((vec_int4)ref, i);
    ref = spu_insert(t + 1, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator -- (int)
{
    int t = spu_extract((vec_int4)ref, i);
    ref = spu_insert(t - 1, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator ++ ()
{
    int t = spu_extract((vec_int4)ref, i) + 1;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator -- ()
{
    int t = spu_extract((vec_int4)ref, i) - 1;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator *= (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) * rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator /= (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) / rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator %= (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) % rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator += (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) + rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator -= (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) - rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator <<= (uint rhs)
{
    int t = spu_extract((vec_int4)ref, i) << rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator >>= (uint rhs)
{
    int t = spu_extract((vec_int4)ref, i) >> rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator &= (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) & rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator ^= (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) ^ rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

inline int int4_idx::operator |= (int rhs)
{
    int t = spu_extract((vec_int4)ref, i) | rhs;
    ref = spu_insert(t, (vec_int4)ref, i);
    return t;
}

}  // namespace simd

#endif
