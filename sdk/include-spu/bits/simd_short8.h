// short8 - 8-lane signed-short SIMD type, backed by vec_short8.

#ifndef PS3TC_SPU_SIMD_SHORT8_H
#define PS3TC_SPU_SIMD_SHORT8_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool8;
class ushort8;
class short8_idx;

class short8
{
private:
    vec_short8 data;

public:
    inline short8() {}
    inline short8(bool8);
    inline short8(ushort8);
    inline short8(short r0, short r1, short r2, short r3,
                  short r4, short r5, short r6, short r7);

    explicit inline short8(short);

    inline short8(vec_short8 rhs);
    inline operator vector signed short() const;

    inline short8_idx operator [] (int i);
    inline short       operator [] (int i) const;

    inline const short8 operator ++ (int);
    inline const short8 operator -- (int);
    inline short8& operator ++ ();
    inline short8& operator -- ();

    inline const short8 operator ~ () const;
    inline const bool8  operator ! () const;
    inline const short8 operator - () const;

    inline short8& operator =  (short8 rhs);
    inline short8& operator *= (short8 rhs);
    inline short8& operator /= (short8 rhs);
    inline short8& operator %= (short8 rhs);
    inline short8& operator += (short8 rhs);
    inline short8& operator -= (short8 rhs);
    inline short8& operator <<= (ushort8 rhs);
    inline short8& operator >>= (ushort8 rhs);
    inline short8& operator &= (short8 rhs);
    inline short8& operator ^= (short8 rhs);
    inline short8& operator |= (short8 rhs);
};

inline const short8 operator * (short8 lhs, short8 rhs);
inline const short8 operator / (short8 lhs, short8 rhs);
inline const short8 operator % (short8 lhs, short8 rhs);
inline const short8 operator + (short8 lhs, short8 rhs);
inline const short8 operator - (short8 lhs, short8 rhs);

inline const short8 operator << (short8 lhs, ushort8 rhs);
inline const short8 operator >> (short8 lhs, ushort8 rhs);

inline const bool8 operator < (short8 lhs, short8 rhs);
inline const bool8 operator <= (short8 lhs, short8 rhs);
inline const bool8 operator > (short8 lhs, short8 rhs);
inline const bool8 operator >= (short8 lhs, short8 rhs);
inline const bool8 operator == (short8 lhs, short8 rhs);
inline const bool8 operator != (short8 lhs, short8 rhs);

inline const short8 select(short8 lhs, short8 rhs, bool8 rhs_slots);

inline const short8 operator & (short8 lhs, short8 rhs);
inline const short8 operator ^ (short8 lhs, short8 rhs);
inline const short8 operator | (short8 lhs, short8 rhs);

class short8_idx
{
private:
    short8 &ref __attribute__((aligned(16)));
    int     i   __attribute__((aligned(16)));
public:
    inline short8_idx(short8& vec, int idx) : ref(vec) { i = idx; }
    inline operator short() const;
    inline short operator = (short rhs);
    inline short operator = (const short8_idx& rhs);
    inline short operator ++ (int);
    inline short operator -- (int);
    inline short operator ++ ();
    inline short operator -- ();
    inline short operator *= (short rhs);
    inline short operator /= (short rhs);
    inline short operator %= (short rhs);
    inline short operator += (short rhs);
    inline short operator -= (short rhs);
    inline short operator <<= (ushort rhs);
    inline short operator >>= (ushort rhs);
    inline short operator &= (short rhs);
    inline short operator ^= (short rhs);
    inline short operator |= (short rhs);
};

}  // namespace simd

#include "simd_bool8.h"
#include "simd_ushort8.h"

namespace simd {

inline short8::short8(short rhs)       { data = spu_splats((short)rhs); }
inline short8::short8(vec_short8 rhs)  { data = rhs; }

inline short8::short8(bool8 rhs)
{
    data = (vec_short8)spu_and((vec_ushort8)rhs, (ushort)1);
}

inline short8::short8(ushort8 rhs)
{
    data = (vec_short8)(vec_ushort8)rhs;
}

inline short8::short8(short r0, short r1, short r2, short r3,
                      short r4, short r5, short r6, short r7)
{
    data = (vec_short8){ r0, r1, r2, r3, r4, r5, r6, r7 };
}

inline short8::operator vector signed short() const { return data; }

inline short8_idx short8::operator [] (int i) { return short8_idx(*this, i); }
inline short      short8::operator [] (int i) const
{
    return (short)spu_extract(data, i);
}

inline const short8 short8::operator ++ (int) { vec_short8 p = data; operator ++(); return short8(p); }
inline const short8 short8::operator -- (int) { vec_short8 p = data; operator --(); return short8(p); }
inline short8& short8::operator ++ () { *this += short8(1); return *this; }
inline short8& short8::operator -- () { *this -= short8(1); return *this; }

inline const short8 short8::operator ~ () const { return short8(spu_nor(data, data)); }
inline const bool8  short8::operator ! () const { return *this == short8(0); }
inline const short8 short8::operator - () const { return short8((vec_short8)spu_sub((short)0, data)); }

inline short8& short8::operator =  (short8 rhs) { data = rhs.data; return *this; }
inline short8& short8::operator *= (short8 rhs) { *this = *this *  rhs; return *this; }
inline short8& short8::operator /= (short8 rhs) { *this = *this /  rhs; return *this; }
inline short8& short8::operator %= (short8 rhs) { *this = *this %  rhs; return *this; }
inline short8& short8::operator += (short8 rhs) { *this = *this +  rhs; return *this; }
inline short8& short8::operator -= (short8 rhs) { *this = *this -  rhs; return *this; }
inline short8& short8::operator <<= (ushort8 rhs) { *this = *this << rhs; return *this; }
inline short8& short8::operator >>= (ushort8 rhs) { *this = *this >> rhs; return *this; }
inline short8& short8::operator &= (short8 rhs) { *this = *this & rhs; return *this; }
inline short8& short8::operator ^= (short8 rhs) { *this = *this ^ rhs; return *this; }
inline short8& short8::operator |= (short8 rhs) { *this = *this | rhs; return *this; }

inline const short8 operator * (short8 lhs, short8 rhs)
{
    // Build full 16x16->16 mul from mule (even lanes) + mulo (odd lanes),
    // then pack low halves of each 32-bit result back into 16 lanes.
    const vec_uchar16 pack_low16 = (vec_uchar16){
        0x02, 0x03, 0x12, 0x13, 0x06, 0x07, 0x16, 0x17,
        0x0a, 0x0b, 0x1a, 0x1b, 0x0e, 0x0f, 0x1e, 0x1f
    };
    vec_int4 e = spu_mule((vec_short8)lhs, (vec_short8)rhs);
    vec_int4 o = spu_mulo((vec_short8)lhs, (vec_short8)rhs);
    return short8((vec_short8)spu_shuffle(e, o, pack_low16));
}

inline const short8 operator / (short8 num, short8 den)
{
    short q[8];
    for (int i = 0; i < 8; ++i)
        q[i] = (short)(spu_extract((vec_short8)num, i) /
                       spu_extract((vec_short8)den, i));
    return short8(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
}

inline const short8 operator % (short8 num, short8 den)
{
    short r[8];
    for (int i = 0; i < 8; ++i)
        r[i] = (short)(spu_extract((vec_short8)num, i) %
                       spu_extract((vec_short8)den, i));
    return short8(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

inline const short8 operator + (short8 lhs, short8 rhs)
{
    return short8(spu_add((vec_short8)lhs, (vec_short8)rhs));
}

inline const short8 operator - (short8 lhs, short8 rhs)
{
    return short8(spu_sub((vec_short8)lhs, (vec_short8)rhs));
}

inline const short8 operator << (short8 lhs, ushort8 rhs)
{
    return short8(spu_sl((vec_short8)lhs, (vec_ushort8)rhs));
}

inline const short8 operator >> (short8 lhs, ushort8 rhs)
{
    return short8(spu_rlmaska((vec_short8)lhs,
                              (vec_short8)spu_sub((short)0, (vec_short8)(vec_ushort8)rhs)));
}

inline const bool8 operator < (short8 lhs, short8 rhs)
{
    bool8 r;
    r.data = spu_cmpgt((vec_short8)rhs, (vec_short8)lhs);
    return r;
}

inline const bool8 operator <= (short8 lhs, short8 rhs) { return !(lhs > rhs); }

inline const bool8 operator > (short8 lhs, short8 rhs)
{
    bool8 r;
    r.data = spu_cmpgt((vec_short8)lhs, (vec_short8)rhs);
    return r;
}

inline const bool8 operator >= (short8 lhs, short8 rhs) { return !(lhs < rhs); }

inline const bool8 operator == (short8 lhs, short8 rhs)
{
    bool8 r;
    r.data = spu_cmpeq((vec_short8)lhs, (vec_short8)rhs);
    return r;
}

inline const bool8 operator != (short8 lhs, short8 rhs) { return !(lhs == rhs); }

inline const short8 select(short8 lhs, short8 rhs, bool8 rhs_slots)
{
    return short8(spu_sel((vec_short8)lhs, (vec_short8)rhs, (vec_ushort8)rhs_slots));
}

inline const short8 operator & (short8 lhs, short8 rhs) { return short8(spu_and((vec_short8)lhs, (vec_short8)rhs)); }
inline const short8 operator | (short8 lhs, short8 rhs) { return short8(spu_or((vec_short8)lhs, (vec_short8)rhs)); }
inline const short8 operator ^ (short8 lhs, short8 rhs) { return short8(spu_xor((vec_short8)lhs, (vec_short8)rhs)); }

inline short8_idx::operator short() const
{
    return (short)spu_extract((vec_short8)ref, i);
}

inline short short8_idx::operator = (short rhs)
{
    ref = spu_insert(rhs, (vec_short8)ref, i);
    return rhs;
}

inline short short8_idx::operator = (const short8_idx& rhs)
{
    return *this = (short)rhs;
}

inline short short8_idx::operator ++ (int)
{
    short t = spu_extract((vec_short8)ref, i);
    ref = spu_insert((short)(t + 1), (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator -- (int)
{
    short t = spu_extract((vec_short8)ref, i);
    ref = spu_insert((short)(t - 1), (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator ++ ()
{
    short t = (short)(spu_extract((vec_short8)ref, i) + 1);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator -- ()
{
    short t = (short)(spu_extract((vec_short8)ref, i) - 1);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator *= (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) * rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator /= (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) / rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator %= (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) % rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator += (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) + rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator -= (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) - rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator <<= (ushort rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) << rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator >>= (ushort rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) >> rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator &= (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) & rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator ^= (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) ^ rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

inline short short8_idx::operator |= (short rhs)
{
    short t = (short)(spu_extract((vec_short8)ref, i) | rhs);
    ref = spu_insert(t, (vec_short8)ref, i);
    return t;
}

}  // namespace simd

#endif
