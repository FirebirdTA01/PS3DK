// ullong2 - 2-lane unsigned-long-long SIMD type, backed by vec_ullong2.

#ifndef PS3TC_SPU_SIMD_ULLONG2_H
#define PS3TC_SPU_SIMD_ULLONG2_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool2;
class llong2;
class double2;
class ullong2_idx;

class ullong2
{
private:
    vec_ullong2 data;

public:
    inline ullong2() {}
    inline ullong2(bool2);
    inline ullong2(llong2);
    inline ullong2(double2);
    inline ullong2(ullong r0, ullong r1);

    explicit inline ullong2(ullong);

    inline ullong2(vec_ullong2 rhs);
    inline operator vector unsigned long long() const;

    inline ullong2_idx operator [] (int i);
    inline ullong       operator [] (int i) const;

    inline const ullong2 operator ++ (int);
    inline const ullong2 operator -- (int);
    inline ullong2& operator ++ ();
    inline ullong2& operator -- ();

    inline const ullong2 operator ~ () const;
    inline const bool2   operator ! () const;
    inline const llong2  operator - () const;

    inline ullong2& operator =  (ullong2 rhs);
    inline ullong2& operator *= (ullong2 rhs);
    inline ullong2& operator /= (ullong2 rhs);
    inline ullong2& operator %= (ullong2 rhs);
    inline ullong2& operator += (ullong2 rhs);
    inline ullong2& operator -= (ullong2 rhs);
    inline ullong2& operator <<= (ullong2 rhs);
    inline ullong2& operator >>= (ullong2 rhs);
    inline ullong2& operator &= (ullong2 rhs);
    inline ullong2& operator ^= (ullong2 rhs);
    inline ullong2& operator |= (ullong2 rhs);
};

inline const ullong2 operator * (ullong2 lhs, ullong2 rhs);
inline const ullong2 operator / (ullong2 lhs, ullong2 rhs);
inline const ullong2 operator % (ullong2 lhs, ullong2 rhs);
inline const ullong2 operator + (ullong2 lhs, ullong2 rhs);
inline const ullong2 operator - (ullong2 lhs, ullong2 rhs);

inline const ullong2 operator << (ullong2 lhs, ullong2 rhs);
inline const ullong2 operator >> (ullong2 lhs, ullong2 rhs);

inline const bool2 operator < (ullong2 lhs, ullong2 rhs);
inline const bool2 operator <= (ullong2 lhs, ullong2 rhs);
inline const bool2 operator > (ullong2 lhs, ullong2 rhs);
inline const bool2 operator >= (ullong2 lhs, ullong2 rhs);
inline const bool2 operator == (ullong2 lhs, ullong2 rhs);
inline const bool2 operator != (ullong2 lhs, ullong2 rhs);

inline const ullong2 select(ullong2 lhs, ullong2 rhs, bool2 rhs_slots);

inline const ullong2 operator & (ullong2 lhs, ullong2 rhs);
inline const ullong2 operator ^ (ullong2 lhs, ullong2 rhs);
inline const ullong2 operator | (ullong2 lhs, ullong2 rhs);

class ullong2_idx
{
private:
    ullong2 &ref __attribute__((aligned(16)));
    int      i   __attribute__((aligned(16)));
public:
    inline ullong2_idx(ullong2& vec, int idx) : ref(vec) { i = idx; }
    inline operator ullong() const;
    inline ullong operator = (ullong rhs);
    inline ullong operator = (const ullong2_idx& rhs);
    inline ullong operator ++ (int);
    inline ullong operator -- (int);
    inline ullong operator ++ ();
    inline ullong operator -- ();
    inline ullong operator *= (ullong rhs);
    inline ullong operator /= (ullong rhs);
    inline ullong operator %= (ullong rhs);
    inline ullong operator += (ullong rhs);
    inline ullong operator -= (ullong rhs);
    inline ullong operator <<= (ullong rhs);
    inline ullong operator >>= (ullong rhs);
    inline ullong operator &= (ullong rhs);
    inline ullong operator ^= (ullong rhs);
    inline ullong operator |= (ullong rhs);
};

}  // namespace simd

#include "simd_bool2.h"
#include "simd_llong2.h"
#include "simd_double2.h"

namespace simd {

inline ullong2::ullong2(ullong rhs)        { data = spu_splats((ullong)rhs); }
inline ullong2::ullong2(vec_ullong2 rhs)   { data = rhs; }

inline ullong2::ullong2(bool2 rhs)
{
    data = spu_and((vec_ullong2)rhs, spu_splats((ullong)1));
}

inline ullong2::ullong2(llong2 rhs)
{
    data = (vec_ullong2)(vec_llong2)rhs;
}

inline ullong2::ullong2(double2 rhs)
{
    data = (vec_ullong2){
        (ullong)spu_extract((vec_double2)rhs, 0),
        (ullong)spu_extract((vec_double2)rhs, 1)
    };
}

inline ullong2::ullong2(ullong r0, ullong r1)
{
    data = (vec_ullong2){ r0, r1 };
}

inline ullong2::operator vector unsigned long long() const { return data; }

inline ullong2_idx ullong2::operator [] (int i) { return ullong2_idx(*this, i); }
inline ullong      ullong2::operator [] (int i) const
{
    return (ullong)spu_extract(data, i);
}

inline const ullong2 ullong2::operator ++ (int) { vec_ullong2 p = data; operator ++(); return ullong2(p); }
inline const ullong2 ullong2::operator -- (int) { vec_ullong2 p = data; operator --(); return ullong2(p); }
inline ullong2& ullong2::operator ++ () { *this += ullong2(1); return *this; }
inline ullong2& ullong2::operator -- () { *this -= ullong2(1); return *this; }

inline const ullong2 ullong2::operator ~ () const { return ullong2(spu_nor(data, data)); }
inline const bool2   ullong2::operator ! () const { return *this == ullong2(0); }
inline const llong2  ullong2::operator - () const { return llong2(0) - llong2(*this); }

inline ullong2& ullong2::operator =  (ullong2 rhs) { data = rhs.data; return *this; }
inline ullong2& ullong2::operator *= (ullong2 rhs) { *this = *this *  rhs; return *this; }
inline ullong2& ullong2::operator /= (ullong2 rhs) { *this = *this /  rhs; return *this; }
inline ullong2& ullong2::operator %= (ullong2 rhs) { *this = *this %  rhs; return *this; }
inline ullong2& ullong2::operator += (ullong2 rhs) { *this = *this +  rhs; return *this; }
inline ullong2& ullong2::operator -= (ullong2 rhs) { *this = *this -  rhs; return *this; }
inline ullong2& ullong2::operator <<= (ullong2 rhs) { *this = *this << rhs; return *this; }
inline ullong2& ullong2::operator >>= (ullong2 rhs) { *this = *this >> rhs; return *this; }
inline ullong2& ullong2::operator &= (ullong2 rhs) { *this = *this & rhs; return *this; }
inline ullong2& ullong2::operator ^= (ullong2 rhs) { *this = *this ^ rhs; return *this; }
inline ullong2& ullong2::operator |= (ullong2 rhs) { *this = *this | rhs; return *this; }

inline const ullong2 operator * (ullong2 lhs, ullong2 rhs)
{
    return ullong2(spu_extract((vec_ullong2)lhs, 0) * spu_extract((vec_ullong2)rhs, 0),
                   spu_extract((vec_ullong2)lhs, 1) * spu_extract((vec_ullong2)rhs, 1));
}

inline const ullong2 operator / (ullong2 num, ullong2 den)
{
    return ullong2(spu_extract((vec_ullong2)num, 0) / spu_extract((vec_ullong2)den, 0),
                   spu_extract((vec_ullong2)num, 1) / spu_extract((vec_ullong2)den, 1));
}

inline const ullong2 operator % (ullong2 num, ullong2 den)
{
    return ullong2(spu_extract((vec_ullong2)num, 0) % spu_extract((vec_ullong2)den, 0),
                   spu_extract((vec_ullong2)num, 1) % spu_extract((vec_ullong2)den, 1));
}

inline const ullong2 operator + (ullong2 lhs, ullong2 rhs)
{
    const vec_uchar16 carry_pos = (vec_uchar16)(vec_uint4){
        0x04050607u, 0x80808080u, 0x0c0d0e0fu, 0x80808080u };
    vec_uint4 a = (vec_uint4)(vec_ullong2)lhs;
    vec_uint4 b = (vec_uint4)(vec_ullong2)rhs;
    vec_uint4 c = spu_genc(a, b);
    c = spu_shuffle(c, c, carry_pos);
    return ullong2((vec_ullong2)spu_addx(a, b, c));
}

inline const ullong2 operator - (ullong2 lhs, ullong2 rhs)
{
    const vec_uchar16 borrow_pos = (vec_uchar16)(vec_uint4){
        0x04050607u, 0xc0c0c0c0u, 0x0c0d0e0fu, 0xc0c0c0c0u };
    vec_uint4 a = (vec_uint4)(vec_ullong2)lhs;
    vec_uint4 b = (vec_uint4)(vec_ullong2)rhs;
    vec_uint4 borrow = spu_genb(a, b);
    borrow = spu_shuffle(borrow, borrow, borrow_pos);
    return ullong2((vec_ullong2)spu_subx(a, b, borrow));
}

inline const ullong2 operator << (ullong2 lhs, ullong2 rhs)
{
    return ullong2(spu_extract((vec_ullong2)lhs, 0) << spu_extract((vec_ullong2)rhs, 0),
                   spu_extract((vec_ullong2)lhs, 1) << spu_extract((vec_ullong2)rhs, 1));
}

inline const ullong2 operator >> (ullong2 lhs, ullong2 rhs)
{
    return ullong2(spu_extract((vec_ullong2)lhs, 0) >> spu_extract((vec_ullong2)rhs, 0),
                   spu_extract((vec_ullong2)lhs, 1) >> spu_extract((vec_ullong2)rhs, 1));
}

inline const bool2 operator > (ullong2 lhs, ullong2 rhs)
{
    bool2 r;
    const vec_uchar16 hi32 = (vec_uchar16)(vec_uint4){
        0x00010203u, 0x00010203u, 0x08090a0bu, 0x08090a0bu };
    const vec_uchar16 lo32 = (vec_uchar16)(vec_uint4){
        0x04050607u, 0x04050607u, 0x0c0d0e0fu, 0x0c0d0e0fu };
    vec_uint4 a = (vec_uint4)(vec_ullong2)lhs;
    vec_uint4 b = (vec_uint4)(vec_ullong2)rhs;
    vec_uint4 gt = spu_cmpgt(a, b);
    vec_uint4 eq = spu_cmpeq(a, b);
    r.data = (vec_ullong2)spu_or(
        spu_shuffle(gt, gt, hi32),
        spu_and(spu_shuffle(eq, eq, hi32),
                spu_shuffle(gt, gt, lo32)));
    return r;
}

inline const bool2 operator < (ullong2 lhs, ullong2 rhs)  { return rhs > lhs; }
inline const bool2 operator <= (ullong2 lhs, ullong2 rhs) { return !(lhs > rhs); }
inline const bool2 operator >= (ullong2 lhs, ullong2 rhs) { return !(rhs > lhs); }

inline const bool2 operator == (ullong2 lhs, ullong2 rhs)
{
    bool2 r;
    const vec_uchar16 hi32 = (vec_uchar16)(vec_uint4){
        0x00010203u, 0x00010203u, 0x08090a0bu, 0x08090a0bu };
    const vec_uchar16 lo32 = (vec_uchar16)(vec_uint4){
        0x04050607u, 0x04050607u, 0x0c0d0e0fu, 0x0c0d0e0fu };
    vec_uint4 eq = spu_cmpeq((vec_uint4)(vec_ullong2)lhs,
                             (vec_uint4)(vec_ullong2)rhs);
    r.data = (vec_ullong2)spu_and(spu_shuffle(eq, eq, hi32),
                                  spu_shuffle(eq, eq, lo32));
    return r;
}

inline const bool2 operator != (ullong2 lhs, ullong2 rhs) { return !(lhs == rhs); }

inline const ullong2 select(ullong2 lhs, ullong2 rhs, bool2 rhs_slots)
{
    return ullong2(spu_sel((vec_ullong2)lhs, (vec_ullong2)rhs, (vec_ullong2)rhs_slots));
}

inline const ullong2 operator & (ullong2 lhs, ullong2 rhs) { return ullong2(spu_and((vec_ullong2)lhs, (vec_ullong2)rhs)); }
inline const ullong2 operator | (ullong2 lhs, ullong2 rhs) { return ullong2(spu_or((vec_ullong2)lhs, (vec_ullong2)rhs)); }
inline const ullong2 operator ^ (ullong2 lhs, ullong2 rhs) { return ullong2(spu_xor((vec_ullong2)lhs, (vec_ullong2)rhs)); }

inline ullong2_idx::operator ullong() const
{
    return (ullong)spu_extract((vec_ullong2)ref, i);
}

inline ullong ullong2_idx::operator = (ullong rhs)
{
    ref = spu_insert(rhs, (vec_ullong2)ref, i);
    return rhs;
}

inline ullong ullong2_idx::operator = (const ullong2_idx& rhs)
{
    return *this = (ullong)rhs;
}

inline ullong ullong2_idx::operator ++ (int)
{
    ullong t = spu_extract((vec_ullong2)ref, i);
    ref = spu_insert(t + 1, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator -- (int)
{
    ullong t = spu_extract((vec_ullong2)ref, i);
    ref = spu_insert(t - 1, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator ++ ()
{
    ullong t = spu_extract((vec_ullong2)ref, i) + 1;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator -- ()
{
    ullong t = spu_extract((vec_ullong2)ref, i) - 1;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator *= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) * rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator /= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) / rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator %= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) % rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator += (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) + rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator -= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) - rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator <<= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) << rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator >>= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) >> rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator &= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) & rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator ^= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) ^ rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

inline ullong ullong2_idx::operator |= (ullong rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) | rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return t;
}

}  // namespace simd

#endif
