// uint4 - 4-lane unsigned-int SIMD type, backed by vec_uint4.

#ifndef PS3TC_SPU_SIMD_UINT4_H
#define PS3TC_SPU_SIMD_UINT4_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool4;
class int4;
class float4;
class uint4_idx;

class uint4
{
private:
    vec_uint4 data;

public:
    inline uint4() {}
    inline uint4(bool4);
    inline uint4(int4);
    inline uint4(float4);
    inline uint4(uint r0, uint r1, uint r2, uint r3);

    explicit inline uint4(uint);

    inline uint4(vec_uint4 rhs);
    inline operator vector unsigned int() const;

    inline uint4_idx operator [] (int i);
    inline uint       operator [] (int i) const;

    inline const uint4 operator ++ (int);
    inline const uint4 operator -- (int);
    inline uint4& operator ++ ();
    inline uint4& operator -- ();

    inline const uint4 operator ~ () const;
    inline const bool4 operator ! () const;
    inline const int4  operator - () const;

    inline uint4& operator =  (uint4 rhs);
    inline uint4& operator *= (uint4 rhs);
    inline uint4& operator /= (uint4 rhs);
    inline uint4& operator %= (uint4 rhs);
    inline uint4& operator += (uint4 rhs);
    inline uint4& operator -= (uint4 rhs);
    inline uint4& operator <<= (uint4 rhs);
    inline uint4& operator >>= (uint4 rhs);
    inline uint4& operator &= (uint4 rhs);
    inline uint4& operator ^= (uint4 rhs);
    inline uint4& operator |= (uint4 rhs);
};

inline const uint4 operator * (uint4 lhs, uint4 rhs);
inline const uint4 operator / (uint4 lhs, uint4 rhs);
inline const uint4 operator % (uint4 lhs, uint4 rhs);
inline const uint4 operator + (uint4 lhs, uint4 rhs);
inline const uint4 operator - (uint4 lhs, uint4 rhs);

inline const uint4 operator << (uint4 lhs, uint4 rhs);
inline const uint4 operator >> (uint4 lhs, uint4 rhs);

inline const bool4 operator < (uint4 lhs, uint4 rhs);
inline const bool4 operator <= (uint4 lhs, uint4 rhs);
inline const bool4 operator > (uint4 lhs, uint4 rhs);
inline const bool4 operator >= (uint4 lhs, uint4 rhs);
inline const bool4 operator == (uint4 lhs, uint4 rhs);
inline const bool4 operator != (uint4 lhs, uint4 rhs);

inline const uint4 select(uint4 lhs, uint4 rhs, bool4 rhs_slots);

inline const uint4 operator & (uint4 lhs, uint4 rhs);
inline const uint4 operator ^ (uint4 lhs, uint4 rhs);
inline const uint4 operator | (uint4 lhs, uint4 rhs);

class uint4_idx
{
private:
    uint4 &ref __attribute__((aligned(16)));
    int    i   __attribute__((aligned(16)));
public:
    inline uint4_idx(uint4& vec, int idx) : ref(vec) { i = idx; }
    inline operator uint() const;
    inline uint operator = (uint rhs);
    inline uint operator = (const uint4_idx& rhs);
    inline uint operator ++ (int);
    inline uint operator -- (int);
    inline uint operator ++ ();
    inline uint operator -- ();
    inline uint operator *= (uint rhs);
    inline uint operator /= (uint rhs);
    inline uint operator %= (uint rhs);
    inline uint operator += (uint rhs);
    inline uint operator -= (uint rhs);
    inline uint operator <<= (uint rhs);
    inline uint operator >>= (uint rhs);
    inline uint operator &= (uint rhs);
    inline uint operator ^= (uint rhs);
    inline uint operator |= (uint rhs);
};

}  // namespace simd

#include "simd_bool4.h"
#include "simd_int4.h"
#include "simd_float4.h"

namespace simd {

inline uint4::uint4(uint rhs)        { data = spu_splats((uint)rhs); }
inline uint4::uint4(vec_uint4 rhs)   { data = rhs; }

inline uint4::uint4(bool4 rhs)
{
    data = spu_and((vec_uint4)rhs, 1u);
}

inline uint4::uint4(int4 rhs)
{
    data = (vec_uint4)(vec_int4)rhs;
}

inline uint4::uint4(float4 rhs)
{
    data = spu_convtu((vec_float4)rhs, 0);
}

inline uint4::uint4(uint r0, uint r1, uint r2, uint r3)
{
    data = (vec_uint4){ r0, r1, r2, r3 };
}

inline uint4::operator vector unsigned int() const { return data; }

inline uint4_idx uint4::operator [] (int i) { return uint4_idx(*this, i); }
inline uint      uint4::operator [] (int i) const
{
    return (uint)spu_extract(data, i);
}

inline const uint4 uint4::operator ++ (int) { vec_uint4 p = data; operator ++(); return uint4(p); }
inline const uint4 uint4::operator -- (int) { vec_uint4 p = data; operator --(); return uint4(p); }
inline uint4& uint4::operator ++ () { *this += uint4(1); return *this; }
inline uint4& uint4::operator -- () { *this -= uint4(1); return *this; }

inline const uint4 uint4::operator ~ () const { return uint4(spu_nor(data, data)); }
inline const bool4 uint4::operator ! () const { return *this == uint4(0); }
inline const int4  uint4::operator - () const { return int4((vec_int4)spu_sub(0u, data)); }

inline uint4& uint4::operator =  (uint4 rhs) { data = rhs.data; return *this; }
inline uint4& uint4::operator *= (uint4 rhs) { *this = *this *  rhs; return *this; }
inline uint4& uint4::operator /= (uint4 rhs) { *this = *this /  rhs; return *this; }
inline uint4& uint4::operator %= (uint4 rhs) { *this = *this %  rhs; return *this; }
inline uint4& uint4::operator += (uint4 rhs) { *this = *this +  rhs; return *this; }
inline uint4& uint4::operator -= (uint4 rhs) { *this = *this -  rhs; return *this; }
inline uint4& uint4::operator <<= (uint4 rhs) { *this = *this << rhs; return *this; }
inline uint4& uint4::operator >>= (uint4 rhs) { *this = *this >> rhs; return *this; }
inline uint4& uint4::operator &= (uint4 rhs) { *this = *this & rhs; return *this; }
inline uint4& uint4::operator ^= (uint4 rhs) { *this = *this ^ rhs; return *this; }
inline uint4& uint4::operator |= (uint4 rhs) { *this = *this | rhs; return *this; }

inline const uint4 operator * (uint4 lhs, uint4 rhs)
{
    vec_short8 a = (vec_short8)(vec_uint4)lhs;
    vec_short8 b = (vec_short8)(vec_uint4)rhs;
    vec_int4 hl = spu_mulh(a, b);
    vec_int4 lh = spu_mulh(b, a);
    vec_int4 ll = (vec_int4)spu_mulo((vec_ushort8)a, (vec_ushort8)b);
    return uint4((vec_uint4)spu_add(spu_add(hl, lh), ll));
}

inline const uint4 operator / (uint4 num, uint4 den)
{
    uint q0 = spu_extract((vec_uint4)num, 0) / spu_extract((vec_uint4)den, 0);
    uint q1 = spu_extract((vec_uint4)num, 1) / spu_extract((vec_uint4)den, 1);
    uint q2 = spu_extract((vec_uint4)num, 2) / spu_extract((vec_uint4)den, 2);
    uint q3 = spu_extract((vec_uint4)num, 3) / spu_extract((vec_uint4)den, 3);
    return uint4(q0, q1, q2, q3);
}

inline const uint4 operator % (uint4 num, uint4 den)
{
    uint r0 = spu_extract((vec_uint4)num, 0) % spu_extract((vec_uint4)den, 0);
    uint r1 = spu_extract((vec_uint4)num, 1) % spu_extract((vec_uint4)den, 1);
    uint r2 = spu_extract((vec_uint4)num, 2) % spu_extract((vec_uint4)den, 2);
    uint r3 = spu_extract((vec_uint4)num, 3) % spu_extract((vec_uint4)den, 3);
    return uint4(r0, r1, r2, r3);
}

inline const uint4 operator + (uint4 lhs, uint4 rhs)
{
    return uint4(spu_add((vec_uint4)lhs, (vec_uint4)rhs));
}

inline const uint4 operator - (uint4 lhs, uint4 rhs)
{
    return uint4(spu_sub((vec_uint4)lhs, (vec_uint4)rhs));
}

inline const uint4 operator << (uint4 lhs, uint4 rhs)
{
    return uint4(spu_sl((vec_uint4)lhs, (vec_uint4)rhs));
}

inline const uint4 operator >> (uint4 lhs, uint4 rhs)
{
    return uint4(spu_rlmask((vec_uint4)lhs, (vec_int4)spu_sub(0u, (vec_uint4)rhs)));
}

inline const bool4 operator < (uint4 lhs, uint4 rhs)
{
    bool4 r;
    r.data = spu_cmpgt((vec_uint4)rhs, (vec_uint4)lhs);
    return r;
}

inline const bool4 operator <= (uint4 lhs, uint4 rhs) { return !(lhs > rhs); }

inline const bool4 operator > (uint4 lhs, uint4 rhs)
{
    bool4 r;
    r.data = spu_cmpgt((vec_uint4)lhs, (vec_uint4)rhs);
    return r;
}

inline const bool4 operator >= (uint4 lhs, uint4 rhs) { return !(lhs < rhs); }

inline const bool4 operator == (uint4 lhs, uint4 rhs)
{
    bool4 r;
    r.data = spu_cmpeq((vec_uint4)lhs, (vec_uint4)rhs);
    return r;
}

inline const bool4 operator != (uint4 lhs, uint4 rhs) { return !(lhs == rhs); }

inline const uint4 select(uint4 lhs, uint4 rhs, bool4 rhs_slots)
{
    return uint4(spu_sel((vec_uint4)lhs, (vec_uint4)rhs, (vec_uint4)rhs_slots));
}

inline const uint4 operator & (uint4 lhs, uint4 rhs) { return uint4(spu_and((vec_uint4)lhs, (vec_uint4)rhs)); }
inline const uint4 operator | (uint4 lhs, uint4 rhs) { return uint4(spu_or((vec_uint4)lhs, (vec_uint4)rhs)); }
inline const uint4 operator ^ (uint4 lhs, uint4 rhs) { return uint4(spu_xor((vec_uint4)lhs, (vec_uint4)rhs)); }

inline uint4_idx::operator uint() const
{
    return (uint)spu_extract((vec_uint4)ref, i);
}

inline uint uint4_idx::operator = (uint rhs)
{
    ref = spu_insert(rhs, (vec_uint4)ref, i);
    return rhs;
}

inline uint uint4_idx::operator = (const uint4_idx& rhs)
{
    return *this = (uint)rhs;
}

inline uint uint4_idx::operator ++ (int)
{
    uint t = spu_extract((vec_uint4)ref, i);
    ref = spu_insert(t + 1, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator -- (int)
{
    uint t = spu_extract((vec_uint4)ref, i);
    ref = spu_insert(t - 1, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator ++ ()
{
    uint t = spu_extract((vec_uint4)ref, i) + 1;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator -- ()
{
    uint t = spu_extract((vec_uint4)ref, i) - 1;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator *= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) * rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator /= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) / rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator %= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) % rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator += (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) + rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator -= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) - rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator <<= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) << rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator >>= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) >> rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator &= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) & rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator ^= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) ^ rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

inline uint uint4_idx::operator |= (uint rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) | rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return t;
}

}  // namespace simd

#endif
