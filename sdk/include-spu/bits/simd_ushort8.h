// ushort8 - 8-lane unsigned-short SIMD type, backed by vec_ushort8.

#ifndef PS3TC_SPU_SIMD_USHORT8_H
#define PS3TC_SPU_SIMD_USHORT8_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool8;
class short8;
class ushort8_idx;

class ushort8
{
private:
    vec_ushort8 data;

public:
    inline ushort8() {}
    inline ushort8(bool8);
    inline ushort8(short8);
    inline ushort8(ushort r0, ushort r1, ushort r2, ushort r3,
                   ushort r4, ushort r5, ushort r6, ushort r7);

    explicit inline ushort8(ushort);

    inline ushort8(vec_ushort8 rhs);
    inline operator vector unsigned short() const;

    inline ushort8_idx operator [] (int i);
    inline ushort       operator [] (int i) const;

    inline const ushort8 operator ++ (int);
    inline const ushort8 operator -- (int);
    inline ushort8& operator ++ ();
    inline ushort8& operator -- ();

    inline const ushort8 operator ~ () const;
    inline const bool8   operator ! () const;
    inline const short8  operator - () const;

    inline ushort8& operator =  (ushort8 rhs);
    inline ushort8& operator *= (ushort8 rhs);
    inline ushort8& operator /= (ushort8 rhs);
    inline ushort8& operator %= (ushort8 rhs);
    inline ushort8& operator += (ushort8 rhs);
    inline ushort8& operator -= (ushort8 rhs);
    inline ushort8& operator <<= (ushort8 rhs);
    inline ushort8& operator >>= (ushort8 rhs);
    inline ushort8& operator &= (ushort8 rhs);
    inline ushort8& operator ^= (ushort8 rhs);
    inline ushort8& operator |= (ushort8 rhs);
};

inline const ushort8 operator * (ushort8 lhs, ushort8 rhs);
inline const ushort8 operator / (ushort8 lhs, ushort8 rhs);
inline const ushort8 operator % (ushort8 lhs, ushort8 rhs);
inline const ushort8 operator + (ushort8 lhs, ushort8 rhs);
inline const ushort8 operator - (ushort8 lhs, ushort8 rhs);

inline const ushort8 operator << (ushort8 lhs, ushort8 rhs);
inline const ushort8 operator >> (ushort8 lhs, ushort8 rhs);

inline const bool8 operator < (ushort8 lhs, ushort8 rhs);
inline const bool8 operator <= (ushort8 lhs, ushort8 rhs);
inline const bool8 operator > (ushort8 lhs, ushort8 rhs);
inline const bool8 operator >= (ushort8 lhs, ushort8 rhs);
inline const bool8 operator == (ushort8 lhs, ushort8 rhs);
inline const bool8 operator != (ushort8 lhs, ushort8 rhs);

inline const ushort8 select(ushort8 lhs, ushort8 rhs, bool8 rhs_slots);

inline const ushort8 operator & (ushort8 lhs, ushort8 rhs);
inline const ushort8 operator ^ (ushort8 lhs, ushort8 rhs);
inline const ushort8 operator | (ushort8 lhs, ushort8 rhs);

class ushort8_idx
{
private:
    ushort8 &ref __attribute__((aligned(16)));
    int      i   __attribute__((aligned(16)));
public:
    inline ushort8_idx(ushort8& vec, int idx) : ref(vec) { i = idx; }
    inline operator ushort() const;
    inline ushort operator = (ushort rhs);
    inline ushort operator = (const ushort8_idx& rhs);
    inline ushort operator ++ (int);
    inline ushort operator -- (int);
    inline ushort operator ++ ();
    inline ushort operator -- ();
    inline ushort operator *= (ushort rhs);
    inline ushort operator /= (ushort rhs);
    inline ushort operator %= (ushort rhs);
    inline ushort operator += (ushort rhs);
    inline ushort operator -= (ushort rhs);
    inline ushort operator <<= (ushort rhs);
    inline ushort operator >>= (ushort rhs);
    inline ushort operator &= (ushort rhs);
    inline ushort operator ^= (ushort rhs);
    inline ushort operator |= (ushort rhs);
};

}  // namespace simd

#include "simd_bool8.h"
#include "simd_short8.h"

namespace simd {

inline ushort8::ushort8(ushort rhs)        { data = spu_splats((ushort)rhs); }
inline ushort8::ushort8(vec_ushort8 rhs)   { data = rhs; }

inline ushort8::ushort8(bool8 rhs)
{
    data = spu_and((vec_ushort8)rhs, (ushort)1);
}

inline ushort8::ushort8(short8 rhs)
{
    data = (vec_ushort8)(vec_short8)rhs;
}

inline ushort8::ushort8(ushort r0, ushort r1, ushort r2, ushort r3,
                        ushort r4, ushort r5, ushort r6, ushort r7)
{
    data = (vec_ushort8){ r0, r1, r2, r3, r4, r5, r6, r7 };
}

inline ushort8::operator vector unsigned short() const { return data; }

inline ushort8_idx ushort8::operator [] (int i) { return ushort8_idx(*this, i); }
inline ushort      ushort8::operator [] (int i) const
{
    return (ushort)spu_extract(data, i);
}

inline const ushort8 ushort8::operator ++ (int) { vec_ushort8 p = data; operator ++(); return ushort8(p); }
inline const ushort8 ushort8::operator -- (int) { vec_ushort8 p = data; operator --(); return ushort8(p); }
inline ushort8& ushort8::operator ++ () { *this += ushort8(1); return *this; }
inline ushort8& ushort8::operator -- () { *this -= ushort8(1); return *this; }

inline const ushort8 ushort8::operator ~ () const { return ushort8(spu_nor(data, data)); }
inline const bool8   ushort8::operator ! () const { return *this == ushort8(0); }
inline const short8  ushort8::operator - () const { return short8((vec_short8)spu_sub((ushort)0, data)); }

inline ushort8& ushort8::operator =  (ushort8 rhs) { data = rhs.data; return *this; }
inline ushort8& ushort8::operator *= (ushort8 rhs) { *this = *this *  rhs; return *this; }
inline ushort8& ushort8::operator /= (ushort8 rhs) { *this = *this /  rhs; return *this; }
inline ushort8& ushort8::operator %= (ushort8 rhs) { *this = *this %  rhs; return *this; }
inline ushort8& ushort8::operator += (ushort8 rhs) { *this = *this +  rhs; return *this; }
inline ushort8& ushort8::operator -= (ushort8 rhs) { *this = *this -  rhs; return *this; }
inline ushort8& ushort8::operator <<= (ushort8 rhs) { *this = *this << rhs; return *this; }
inline ushort8& ushort8::operator >>= (ushort8 rhs) { *this = *this >> rhs; return *this; }
inline ushort8& ushort8::operator &= (ushort8 rhs) { *this = *this & rhs; return *this; }
inline ushort8& ushort8::operator ^= (ushort8 rhs) { *this = *this ^ rhs; return *this; }
inline ushort8& ushort8::operator |= (ushort8 rhs) { *this = *this | rhs; return *this; }

inline const ushort8 operator * (ushort8 lhs, ushort8 rhs)
{
    const vec_uchar16 pack_low16 = (vec_uchar16){
        0x02, 0x03, 0x12, 0x13, 0x06, 0x07, 0x16, 0x17,
        0x0a, 0x0b, 0x1a, 0x1b, 0x0e, 0x0f, 0x1e, 0x1f
    };
    vec_uint4 e = spu_mule((vec_ushort8)lhs, (vec_ushort8)rhs);
    vec_uint4 o = spu_mulo((vec_ushort8)lhs, (vec_ushort8)rhs);
    return ushort8((vec_ushort8)spu_shuffle(e, o, pack_low16));
}

inline const ushort8 operator / (ushort8 num, ushort8 den)
{
    ushort q[8];
    for (int i = 0; i < 8; ++i)
        q[i] = (ushort)(spu_extract((vec_ushort8)num, i) /
                        spu_extract((vec_ushort8)den, i));
    return ushort8(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
}

inline const ushort8 operator % (ushort8 num, ushort8 den)
{
    ushort r[8];
    for (int i = 0; i < 8; ++i)
        r[i] = (ushort)(spu_extract((vec_ushort8)num, i) %
                        spu_extract((vec_ushort8)den, i));
    return ushort8(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

inline const ushort8 operator + (ushort8 lhs, ushort8 rhs)
{
    return ushort8(spu_add((vec_ushort8)lhs, (vec_ushort8)rhs));
}

inline const ushort8 operator - (ushort8 lhs, ushort8 rhs)
{
    return ushort8(spu_sub((vec_ushort8)lhs, (vec_ushort8)rhs));
}

inline const ushort8 operator << (ushort8 lhs, ushort8 rhs)
{
    return ushort8(spu_sl((vec_ushort8)lhs, (vec_ushort8)rhs));
}

inline const ushort8 operator >> (ushort8 lhs, ushort8 rhs)
{
    return ushort8(spu_rlmask((vec_ushort8)lhs,
                              (vec_short8)spu_sub((short)0, (vec_short8)(vec_ushort8)rhs)));
}

inline const bool8 operator < (ushort8 lhs, ushort8 rhs)
{
    bool8 r;
    r.data = spu_cmpgt((vec_ushort8)rhs, (vec_ushort8)lhs);
    return r;
}

inline const bool8 operator <= (ushort8 lhs, ushort8 rhs) { return !(lhs > rhs); }

inline const bool8 operator > (ushort8 lhs, ushort8 rhs)
{
    bool8 r;
    r.data = spu_cmpgt((vec_ushort8)lhs, (vec_ushort8)rhs);
    return r;
}

inline const bool8 operator >= (ushort8 lhs, ushort8 rhs) { return !(lhs < rhs); }

inline const bool8 operator == (ushort8 lhs, ushort8 rhs)
{
    bool8 r;
    r.data = spu_cmpeq((vec_ushort8)lhs, (vec_ushort8)rhs);
    return r;
}

inline const bool8 operator != (ushort8 lhs, ushort8 rhs) { return !(lhs == rhs); }

inline const ushort8 select(ushort8 lhs, ushort8 rhs, bool8 rhs_slots)
{
    return ushort8(spu_sel((vec_ushort8)lhs, (vec_ushort8)rhs, (vec_ushort8)rhs_slots));
}

inline const ushort8 operator & (ushort8 lhs, ushort8 rhs) { return ushort8(spu_and((vec_ushort8)lhs, (vec_ushort8)rhs)); }
inline const ushort8 operator | (ushort8 lhs, ushort8 rhs) { return ushort8(spu_or((vec_ushort8)lhs, (vec_ushort8)rhs)); }
inline const ushort8 operator ^ (ushort8 lhs, ushort8 rhs) { return ushort8(spu_xor((vec_ushort8)lhs, (vec_ushort8)rhs)); }

inline ushort8_idx::operator ushort() const
{
    return (ushort)spu_extract((vec_ushort8)ref, i);
}

inline ushort ushort8_idx::operator = (ushort rhs)
{
    ref = spu_insert(rhs, (vec_ushort8)ref, i);
    return rhs;
}

inline ushort ushort8_idx::operator = (const ushort8_idx& rhs)
{
    return *this = (ushort)rhs;
}

inline ushort ushort8_idx::operator ++ (int)
{
    ushort t = spu_extract((vec_ushort8)ref, i);
    ref = spu_insert((ushort)(t + 1), (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator -- (int)
{
    ushort t = spu_extract((vec_ushort8)ref, i);
    ref = spu_insert((ushort)(t - 1), (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator ++ ()
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) + 1);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator -- ()
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) - 1);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator *= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) * rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator /= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) / rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator %= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) % rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator += (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) + rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator -= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) - rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator <<= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) << rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator >>= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) >> rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator &= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) & rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator ^= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) ^ rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

inline ushort ushort8_idx::operator |= (ushort rhs)
{
    ushort t = (ushort)(spu_extract((vec_ushort8)ref, i) | rhs);
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return t;
}

}  // namespace simd

#endif
