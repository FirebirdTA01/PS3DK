// llong2 - 2-lane signed-long-long SIMD type, backed by vec_llong2.

#ifndef PS3TC_SPU_SIMD_LLONG2_H
#define PS3TC_SPU_SIMD_LLONG2_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool2;
class ullong2;
class double2;
class llong2_idx;

class llong2
{
private:
    vec_llong2 data;

public:
    inline llong2() {}
    inline llong2(bool2);
    inline llong2(ullong2);
    inline llong2(double2);
    inline llong2(llong r0, llong r1);

    explicit inline llong2(llong);

    inline llong2(vec_llong2 rhs);
    inline operator vector signed long long() const;

    inline llong2_idx operator [] (int i);
    inline llong       operator [] (int i) const;

    inline const llong2 operator ++ (int);
    inline const llong2 operator -- (int);
    inline llong2& operator ++ ();
    inline llong2& operator -- ();

    inline const llong2 operator ~ () const;
    inline const bool2  operator ! () const;
    inline const llong2 operator - () const;

    inline llong2& operator =  (llong2 rhs);
    inline llong2& operator *= (llong2 rhs);
    inline llong2& operator /= (llong2 rhs);
    inline llong2& operator %= (llong2 rhs);
    inline llong2& operator += (llong2 rhs);
    inline llong2& operator -= (llong2 rhs);
    inline llong2& operator <<= (ullong2 rhs);
    inline llong2& operator >>= (ullong2 rhs);
    inline llong2& operator &= (llong2 rhs);
    inline llong2& operator ^= (llong2 rhs);
    inline llong2& operator |= (llong2 rhs);
};

inline const llong2 operator * (llong2 lhs, llong2 rhs);
inline const llong2 operator / (llong2 lhs, llong2 rhs);
inline const llong2 operator % (llong2 lhs, llong2 rhs);
inline const llong2 operator + (llong2 lhs, llong2 rhs);
inline const llong2 operator - (llong2 lhs, llong2 rhs);

inline const llong2 operator << (llong2 lhs, ullong2 rhs);
inline const llong2 operator >> (llong2 lhs, ullong2 rhs);

inline const bool2 operator < (llong2 lhs, llong2 rhs);
inline const bool2 operator <= (llong2 lhs, llong2 rhs);
inline const bool2 operator > (llong2 lhs, llong2 rhs);
inline const bool2 operator >= (llong2 lhs, llong2 rhs);
inline const bool2 operator == (llong2 lhs, llong2 rhs);
inline const bool2 operator != (llong2 lhs, llong2 rhs);

inline const llong2 select(llong2 lhs, llong2 rhs, bool2 rhs_slots);

inline const llong2 operator & (llong2 lhs, llong2 rhs);
inline const llong2 operator ^ (llong2 lhs, llong2 rhs);
inline const llong2 operator | (llong2 lhs, llong2 rhs);

class llong2_idx
{
private:
    llong2 &ref __attribute__((aligned(16)));
    int     i   __attribute__((aligned(16)));
public:
    inline llong2_idx(llong2& vec, int idx) : ref(vec) { i = idx; }
    inline operator llong() const;
    inline llong operator = (llong rhs);
    inline llong operator = (const llong2_idx& rhs);
    inline llong operator ++ (int);
    inline llong operator -- (int);
    inline llong operator ++ ();
    inline llong operator -- ();
    inline llong operator *= (llong rhs);
    inline llong operator /= (llong rhs);
    inline llong operator %= (llong rhs);
    inline llong operator += (llong rhs);
    inline llong operator -= (llong rhs);
    inline llong operator <<= (ullong rhs);
    inline llong operator >>= (ullong rhs);
    inline llong operator &= (llong rhs);
    inline llong operator ^= (llong rhs);
    inline llong operator |= (llong rhs);
};

}  // namespace simd

#include "simd_bool2.h"
#include "simd_ullong2.h"
#include "simd_double2.h"

namespace simd {

inline llong2::llong2(llong rhs)        { data = spu_splats((llong)rhs); }
inline llong2::llong2(vec_llong2 rhs)   { data = rhs; }

inline llong2::llong2(bool2 rhs)
{
    data = (vec_llong2)spu_and((vec_ullong2)rhs, spu_splats((ullong)1));
}

inline llong2::llong2(ullong2 rhs)
{
    data = (vec_llong2)(vec_ullong2)rhs;
}

inline llong2::llong2(double2 rhs)
{
    data = (vec_llong2){
        (llong)spu_extract((vec_double2)rhs, 0),
        (llong)spu_extract((vec_double2)rhs, 1)
    };
}

inline llong2::llong2(llong r0, llong r1)
{
    data = (vec_llong2){ r0, r1 };
}

inline llong2::operator vector signed long long() const { return data; }

inline llong2_idx llong2::operator [] (int i) { return llong2_idx(*this, i); }
inline llong      llong2::operator [] (int i) const
{
    return (llong)spu_extract(data, i);
}

inline const llong2 llong2::operator ++ (int) { vec_llong2 p = data; operator ++(); return llong2(p); }
inline const llong2 llong2::operator -- (int) { vec_llong2 p = data; operator --(); return llong2(p); }
inline llong2& llong2::operator ++ () { *this += llong2(1); return *this; }
inline llong2& llong2::operator -- () { *this -= llong2(1); return *this; }

inline const llong2 llong2::operator ~ () const { return llong2(spu_nor(data, data)); }
inline const bool2  llong2::operator ! () const { return *this == llong2(0); }
inline const llong2 llong2::operator - () const { return llong2(0) - *this; }

inline llong2& llong2::operator =  (llong2 rhs) { data = rhs.data; return *this; }
inline llong2& llong2::operator *= (llong2 rhs) { *this = *this *  rhs; return *this; }
inline llong2& llong2::operator /= (llong2 rhs) { *this = *this /  rhs; return *this; }
inline llong2& llong2::operator %= (llong2 rhs) { *this = *this %  rhs; return *this; }
inline llong2& llong2::operator += (llong2 rhs) { *this = *this +  rhs; return *this; }
inline llong2& llong2::operator -= (llong2 rhs) { *this = *this -  rhs; return *this; }
inline llong2& llong2::operator <<= (ullong2 rhs) { *this = *this << rhs; return *this; }
inline llong2& llong2::operator >>= (ullong2 rhs) { *this = *this >> rhs; return *this; }
inline llong2& llong2::operator &= (llong2 rhs) { *this = *this & rhs; return *this; }
inline llong2& llong2::operator ^= (llong2 rhs) { *this = *this ^ rhs; return *this; }
inline llong2& llong2::operator |= (llong2 rhs) { *this = *this | rhs; return *this; }

// SPU has no native 64-bit ALU; per-lane scalar fallback for arithmetic.

inline const llong2 operator * (llong2 lhs, llong2 rhs)
{
    return llong2(spu_extract((vec_llong2)lhs, 0) * spu_extract((vec_llong2)rhs, 0),
                  spu_extract((vec_llong2)lhs, 1) * spu_extract((vec_llong2)rhs, 1));
}

inline const llong2 operator / (llong2 num, llong2 den)
{
    return llong2(spu_extract((vec_llong2)num, 0) / spu_extract((vec_llong2)den, 0),
                  spu_extract((vec_llong2)num, 1) / spu_extract((vec_llong2)den, 1));
}

inline const llong2 operator % (llong2 num, llong2 den)
{
    return llong2(spu_extract((vec_llong2)num, 0) % spu_extract((vec_llong2)den, 0),
                  spu_extract((vec_llong2)num, 1) % spu_extract((vec_llong2)den, 1));
}

inline const llong2 operator + (llong2 lhs, llong2 rhs)
{
    // 64-bit add via two 32-bit halves with carry propagation.
    const vec_uchar16 carry_pos = (vec_uchar16)(vec_uint4){
        0x04050607u, 0x80808080u, 0x0c0d0e0fu, 0x80808080u };
    vec_uint4 a = (vec_uint4)(vec_llong2)lhs;
    vec_uint4 b = (vec_uint4)(vec_llong2)rhs;
    vec_uint4 c = spu_genc(a, b);
    c = spu_shuffle(c, c, carry_pos);
    return llong2((vec_llong2)spu_addx(a, b, c));
}

inline const llong2 operator - (llong2 lhs, llong2 rhs)
{
    const vec_uchar16 borrow_pos = (vec_uchar16)(vec_uint4){
        0x04050607u, 0xc0c0c0c0u, 0x0c0d0e0fu, 0xc0c0c0c0u };
    vec_uint4 a = (vec_uint4)(vec_llong2)lhs;
    vec_uint4 b = (vec_uint4)(vec_llong2)rhs;
    vec_uint4 borrow = spu_genb(a, b);
    borrow = spu_shuffle(borrow, borrow, borrow_pos);
    return llong2((vec_llong2)spu_subx(a, b, borrow));
}

inline const llong2 operator << (llong2 lhs, ullong2 rhs)
{
    return llong2(spu_extract((vec_llong2)lhs, 0) << spu_extract((vec_ullong2)rhs, 0),
                  spu_extract((vec_llong2)lhs, 1) << spu_extract((vec_ullong2)rhs, 1));
}

inline const llong2 operator >> (llong2 lhs, ullong2 rhs)
{
    return llong2(spu_extract((vec_llong2)lhs, 0) >> spu_extract((vec_ullong2)rhs, 0),
                  spu_extract((vec_llong2)lhs, 1) >> spu_extract((vec_ullong2)rhs, 1));
}

namespace ll_detail {
    // Build a 2x ullong "all-ones if true, zero if false" mask given the
    // 2x int4 results of comparing the high 32-bit halves and the
    // matching equality + low-half-cmp masks.
    inline vec_ullong2 fold_pair_gt(vec_uint4 hi_gt, vec_uint4 hi_eq, vec_uint4 lo_gt)
    {
        const vec_uchar16 hi32 = (vec_uchar16)(vec_uint4){
            0x00010203u, 0x00010203u, 0x08090a0bu, 0x08090a0bu };
        const vec_uchar16 lo32 = (vec_uchar16)(vec_uint4){
            0x04050607u, 0x04050607u, 0x0c0d0e0fu, 0x0c0d0e0fu };
        return (vec_ullong2)spu_or(
            spu_shuffle(hi_gt, hi_gt, hi32),
            spu_and(spu_shuffle(hi_eq, hi_eq, hi32),
                    spu_shuffle(lo_gt, lo_gt, lo32)));
    }
}

inline const bool2 operator > (llong2 lhs, llong2 rhs)
{
    bool2 r;
    vec_int4  a_i = (vec_int4)(vec_llong2)lhs;
    vec_int4  b_i = (vec_int4)(vec_llong2)rhs;
    vec_uint4 a_u = (vec_uint4)(vec_llong2)lhs;
    vec_uint4 b_u = (vec_uint4)(vec_llong2)rhs;
    vec_uint4 hi_gt_signed   = spu_cmpgt(a_i, b_i);
    vec_uint4 hi_eq          = spu_cmpeq(a_i, b_i);
    vec_uint4 lo_gt_unsigned = spu_cmpgt(a_u, b_u);
    r.data = ll_detail::fold_pair_gt(hi_gt_signed, hi_eq, lo_gt_unsigned);
    return r;
}

inline const bool2 operator < (llong2 lhs, llong2 rhs)  { return rhs > lhs; }
inline const bool2 operator <= (llong2 lhs, llong2 rhs) { return !(lhs > rhs); }
inline const bool2 operator >= (llong2 lhs, llong2 rhs) { return !(rhs > lhs); }

inline const bool2 operator == (llong2 lhs, llong2 rhs)
{
    bool2 r;
    const vec_uchar16 hi32 = (vec_uchar16)(vec_uint4){
        0x00010203u, 0x00010203u, 0x08090a0bu, 0x08090a0bu };
    const vec_uchar16 lo32 = (vec_uchar16)(vec_uint4){
        0x04050607u, 0x04050607u, 0x0c0d0e0fu, 0x0c0d0e0fu };
    vec_uint4 eq = spu_cmpeq((vec_int4)(vec_llong2)lhs,
                             (vec_int4)(vec_llong2)rhs);
    r.data = (vec_ullong2)spu_and(spu_shuffle(eq, eq, hi32),
                                  spu_shuffle(eq, eq, lo32));
    return r;
}

inline const bool2 operator != (llong2 lhs, llong2 rhs) { return !(lhs == rhs); }

inline const llong2 select(llong2 lhs, llong2 rhs, bool2 rhs_slots)
{
    return llong2(spu_sel((vec_llong2)lhs, (vec_llong2)rhs, (vec_ullong2)rhs_slots));
}

inline const llong2 operator & (llong2 lhs, llong2 rhs) { return llong2(spu_and((vec_llong2)lhs, (vec_llong2)rhs)); }
inline const llong2 operator | (llong2 lhs, llong2 rhs) { return llong2(spu_or((vec_llong2)lhs, (vec_llong2)rhs)); }
inline const llong2 operator ^ (llong2 lhs, llong2 rhs) { return llong2(spu_xor((vec_llong2)lhs, (vec_llong2)rhs)); }

inline llong2_idx::operator llong() const
{
    return (llong)spu_extract((vec_llong2)ref, i);
}

inline llong llong2_idx::operator = (llong rhs)
{
    ref = spu_insert(rhs, (vec_llong2)ref, i);
    return rhs;
}

inline llong llong2_idx::operator = (const llong2_idx& rhs)
{
    return *this = (llong)rhs;
}

inline llong llong2_idx::operator ++ (int)
{
    llong t = spu_extract((vec_llong2)ref, i);
    ref = spu_insert(t + 1, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator -- (int)
{
    llong t = spu_extract((vec_llong2)ref, i);
    ref = spu_insert(t - 1, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator ++ ()
{
    llong t = spu_extract((vec_llong2)ref, i) + 1;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator -- ()
{
    llong t = spu_extract((vec_llong2)ref, i) - 1;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator *= (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) * rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator /= (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) / rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator %= (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) % rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator += (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) + rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator -= (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) - rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator <<= (ullong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) << rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator >>= (ullong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) >> rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator &= (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) & rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator ^= (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) ^ rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

inline llong llong2_idx::operator |= (llong rhs)
{
    llong t = spu_extract((vec_llong2)ref, i) | rhs;
    ref = spu_insert(t, (vec_llong2)ref, i);
    return t;
}

}  // namespace simd

#endif
