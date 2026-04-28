// uchar16 - 16-lane unsigned-char SIMD type, backed by vec_uchar16.

#ifndef PS3TC_SPU_SIMD_UCHAR16_H
#define PS3TC_SPU_SIMD_UCHAR16_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool16;
class char16;
class uchar16_idx;

class uchar16
{
private:
    vec_uchar16 data;

public:
    inline uchar16() {}
    inline uchar16(bool16);
    inline uchar16(char16);
    inline uchar16(uchar r0,  uchar r1,  uchar r2,  uchar r3,
                   uchar r4,  uchar r5,  uchar r6,  uchar r7,
                   uchar r8,  uchar r9,  uchar r10, uchar r11,
                   uchar r12, uchar r13, uchar r14, uchar r15);

    explicit inline uchar16(uchar);

    inline uchar16(vec_uchar16 rhs);
    inline operator vector unsigned char() const;

    inline uchar16_idx operator [] (int i);
    inline uchar        operator [] (int i) const;

    inline const uchar16 operator ++ (int);
    inline const uchar16 operator -- (int);
    inline uchar16& operator ++ ();
    inline uchar16& operator -- ();

    inline const uchar16 operator ~ () const;
    inline const bool16  operator ! () const;
    inline const char16  operator - () const;

    inline uchar16& operator =  (uchar16 rhs);
    inline uchar16& operator *= (uchar16 rhs);
    inline uchar16& operator /= (uchar16 rhs);
    inline uchar16& operator %= (uchar16 rhs);
    inline uchar16& operator += (uchar16 rhs);
    inline uchar16& operator -= (uchar16 rhs);
    inline uchar16& operator <<= (uchar16 rhs);
    inline uchar16& operator >>= (uchar16 rhs);
    inline uchar16& operator &= (uchar16 rhs);
    inline uchar16& operator ^= (uchar16 rhs);
    inline uchar16& operator |= (uchar16 rhs);
};

inline const uchar16 operator * (uchar16 lhs, uchar16 rhs);
inline const uchar16 operator / (uchar16 lhs, uchar16 rhs);
inline const uchar16 operator % (uchar16 lhs, uchar16 rhs);
inline const uchar16 operator + (uchar16 lhs, uchar16 rhs);
inline const uchar16 operator - (uchar16 lhs, uchar16 rhs);

inline const uchar16 operator << (uchar16 lhs, uchar16 rhs);
inline const uchar16 operator >> (uchar16 lhs, uchar16 rhs);

inline const bool16 operator < (uchar16 lhs, uchar16 rhs);
inline const bool16 operator <= (uchar16 lhs, uchar16 rhs);
inline const bool16 operator > (uchar16 lhs, uchar16 rhs);
inline const bool16 operator >= (uchar16 lhs, uchar16 rhs);
inline const bool16 operator == (uchar16 lhs, uchar16 rhs);
inline const bool16 operator != (uchar16 lhs, uchar16 rhs);

inline const uchar16 select(uchar16 lhs, uchar16 rhs, bool16 rhs_slots);

inline const uchar16 operator & (uchar16 lhs, uchar16 rhs);
inline const uchar16 operator ^ (uchar16 lhs, uchar16 rhs);
inline const uchar16 operator | (uchar16 lhs, uchar16 rhs);

class uchar16_idx
{
private:
    uchar16 &ref __attribute__((aligned(16)));
    int      i   __attribute__((aligned(16)));
public:
    inline uchar16_idx(uchar16& vec, int idx) : ref(vec) { i = idx; }
    inline operator uchar() const;
    inline uchar operator = (uchar rhs);
    inline uchar operator = (const uchar16_idx& rhs);
    inline uchar operator ++ (int);
    inline uchar operator -- (int);
    inline uchar operator ++ ();
    inline uchar operator -- ();
    inline uchar operator *= (uchar rhs);
    inline uchar operator /= (uchar rhs);
    inline uchar operator %= (uchar rhs);
    inline uchar operator += (uchar rhs);
    inline uchar operator -= (uchar rhs);
    inline uchar operator <<= (uchar rhs);
    inline uchar operator >>= (uchar rhs);
    inline uchar operator &= (uchar rhs);
    inline uchar operator ^= (uchar rhs);
    inline uchar operator |= (uchar rhs);
};

}  // namespace simd

#include "simd_bool16.h"
#include "simd_char16.h"

namespace simd {

inline uchar16::uchar16(uchar rhs)         { data = spu_splats((uchar)rhs); }
inline uchar16::uchar16(vec_uchar16 rhs)   { data = rhs; }

inline uchar16::uchar16(bool16 rhs)
{
    data = spu_and((vec_uchar16)rhs, (uchar)1);
}

inline uchar16::uchar16(char16 rhs)
{
    data = (vec_uchar16)(vec_char16)rhs;
}

inline uchar16::uchar16(uchar r0,  uchar r1,  uchar r2,  uchar r3,
                        uchar r4,  uchar r5,  uchar r6,  uchar r7,
                        uchar r8,  uchar r9,  uchar r10, uchar r11,
                        uchar r12, uchar r13, uchar r14, uchar r15)
{
    data = (vec_uchar16){
        r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15
    };
}

inline uchar16::operator vector unsigned char() const { return data; }

inline uchar16_idx uchar16::operator [] (int i) { return uchar16_idx(*this, i); }
inline uchar       uchar16::operator [] (int i) const
{
    return (uchar)spu_extract(data, i);
}

inline const uchar16 uchar16::operator ++ (int) { vec_uchar16 p = data; operator ++(); return uchar16(p); }
inline const uchar16 uchar16::operator -- (int) { vec_uchar16 p = data; operator --(); return uchar16(p); }
inline uchar16& uchar16::operator ++ () { *this += uchar16(1); return *this; }
inline uchar16& uchar16::operator -- () { *this -= uchar16(1); return *this; }

inline const uchar16 uchar16::operator ~ () const { return uchar16(spu_nor(data, data)); }
inline const bool16  uchar16::operator ! () const { return *this == uchar16(0); }
inline const char16  uchar16::operator - () const { return uchar16(0) - *this; }

inline uchar16& uchar16::operator =  (uchar16 rhs) { data = rhs.data; return *this; }
inline uchar16& uchar16::operator *= (uchar16 rhs) { *this = *this *  rhs; return *this; }
inline uchar16& uchar16::operator /= (uchar16 rhs) { *this = *this /  rhs; return *this; }
inline uchar16& uchar16::operator %= (uchar16 rhs) { *this = *this %  rhs; return *this; }
inline uchar16& uchar16::operator += (uchar16 rhs) { *this = *this +  rhs; return *this; }
inline uchar16& uchar16::operator -= (uchar16 rhs) { *this = *this -  rhs; return *this; }
inline uchar16& uchar16::operator <<= (uchar16 rhs) { *this = *this << rhs; return *this; }
inline uchar16& uchar16::operator >>= (uchar16 rhs) { *this = *this >> rhs; return *this; }
inline uchar16& uchar16::operator &= (uchar16 rhs) { *this = *this & rhs; return *this; }
inline uchar16& uchar16::operator ^= (uchar16 rhs) { *this = *this ^ rhs; return *this; }
inline uchar16& uchar16::operator |= (uchar16 rhs) { *this = *this | rhs; return *this; }

inline const uchar16 operator * (uchar16 lhs, uchar16 rhs)
{
    uchar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (uchar)((unsigned)spu_extract((vec_uchar16)lhs, i) *
                       (unsigned)spu_extract((vec_uchar16)rhs, i));
    return uchar16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                   r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const uchar16 operator / (uchar16 num, uchar16 den)
{
    uchar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (uchar)(spu_extract((vec_uchar16)num, i) /
                       spu_extract((vec_uchar16)den, i));
    return uchar16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                   r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const uchar16 operator % (uchar16 num, uchar16 den)
{
    uchar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (uchar)(spu_extract((vec_uchar16)num, i) %
                       spu_extract((vec_uchar16)den, i));
    return uchar16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                   r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const uchar16 operator + (uchar16 lhs, uchar16 rhs)
{
    // Same trick as char16: widen to short8 in two halves.
    vec_short8 le = (vec_short8)spu_and((vec_ushort8)(vec_uchar16)lhs, (ushort)0x00ff);
    vec_short8 re = (vec_short8)spu_and((vec_ushort8)(vec_uchar16)rhs, (ushort)0x00ff);
    vec_short8 lo = (vec_short8)spu_rlmask((vec_ushort8)(vec_uchar16)lhs, -8);
    vec_short8 ro = (vec_short8)spu_rlmask((vec_ushort8)(vec_uchar16)rhs, -8);
    vec_short8 se = spu_add(le, re);
    vec_short8 so = spu_add(lo, ro);
    const vec_uchar16 pack = (vec_uchar16){
        0x01, 0x11, 0x03, 0x13, 0x05, 0x15, 0x07, 0x17,
        0x09, 0x19, 0x0b, 0x1b, 0x0d, 0x1d, 0x0f, 0x1f
    };
    return uchar16((vec_uchar16)spu_shuffle((vec_uchar16)so, (vec_uchar16)se, pack));
}

inline const uchar16 operator - (uchar16 lhs, uchar16 rhs)
{
    vec_short8 le = (vec_short8)spu_and((vec_ushort8)(vec_uchar16)lhs, (ushort)0x00ff);
    vec_short8 re = (vec_short8)spu_and((vec_ushort8)(vec_uchar16)rhs, (ushort)0x00ff);
    vec_short8 lo = (vec_short8)spu_rlmask((vec_ushort8)(vec_uchar16)lhs, -8);
    vec_short8 ro = (vec_short8)spu_rlmask((vec_ushort8)(vec_uchar16)rhs, -8);
    vec_short8 se = spu_sub(le, re);
    vec_short8 so = spu_sub(lo, ro);
    const vec_uchar16 pack = (vec_uchar16){
        0x01, 0x11, 0x03, 0x13, 0x05, 0x15, 0x07, 0x17,
        0x09, 0x19, 0x0b, 0x1b, 0x0d, 0x1d, 0x0f, 0x1f
    };
    return uchar16((vec_uchar16)spu_shuffle((vec_uchar16)so, (vec_uchar16)se, pack));
}

inline const uchar16 operator << (uchar16 lhs, uchar16 rhs)
{
    uchar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (uchar)(spu_extract((vec_uchar16)lhs, i) <<
                       (spu_extract((vec_uchar16)rhs, i) & 7));
    return uchar16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                   r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const uchar16 operator >> (uchar16 lhs, uchar16 rhs)
{
    uchar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (uchar)(spu_extract((vec_uchar16)lhs, i) >>
                       (spu_extract((vec_uchar16)rhs, i) & 7));
    return uchar16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                   r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const bool16 operator < (uchar16 lhs, uchar16 rhs)
{
    bool16 r;
    r.data = spu_cmpgt((vec_uchar16)rhs, (vec_uchar16)lhs);
    return r;
}

inline const bool16 operator <= (uchar16 lhs, uchar16 rhs) { return !(lhs > rhs); }

inline const bool16 operator > (uchar16 lhs, uchar16 rhs)
{
    bool16 r;
    r.data = spu_cmpgt((vec_uchar16)lhs, (vec_uchar16)rhs);
    return r;
}

inline const bool16 operator >= (uchar16 lhs, uchar16 rhs) { return !(lhs < rhs); }

inline const bool16 operator == (uchar16 lhs, uchar16 rhs)
{
    bool16 r;
    r.data = spu_cmpeq((vec_uchar16)lhs, (vec_uchar16)rhs);
    return r;
}

inline const bool16 operator != (uchar16 lhs, uchar16 rhs) { return !(lhs == rhs); }

inline const uchar16 select(uchar16 lhs, uchar16 rhs, bool16 rhs_slots)
{
    return uchar16(spu_sel((vec_uchar16)lhs, (vec_uchar16)rhs, (vec_uchar16)rhs_slots));
}

inline const uchar16 operator & (uchar16 lhs, uchar16 rhs) { return uchar16(spu_and((vec_uchar16)lhs, (vec_uchar16)rhs)); }
inline const uchar16 operator | (uchar16 lhs, uchar16 rhs) { return uchar16(spu_or((vec_uchar16)lhs, (vec_uchar16)rhs)); }
inline const uchar16 operator ^ (uchar16 lhs, uchar16 rhs) { return uchar16(spu_xor((vec_uchar16)lhs, (vec_uchar16)rhs)); }

inline uchar16_idx::operator uchar() const
{
    return (uchar)spu_extract((vec_uchar16)ref, i);
}

inline uchar uchar16_idx::operator = (uchar rhs)
{
    ref = spu_insert(rhs, (vec_uchar16)ref, i);
    return rhs;
}

inline uchar uchar16_idx::operator = (const uchar16_idx& rhs)
{
    return *this = (uchar)rhs;
}

inline uchar uchar16_idx::operator ++ (int)
{
    uchar t = spu_extract((vec_uchar16)ref, i);
    ref = spu_insert((uchar)(t + 1), (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator -- (int)
{
    uchar t = spu_extract((vec_uchar16)ref, i);
    ref = spu_insert((uchar)(t - 1), (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator ++ ()
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) + 1);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator -- ()
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) - 1);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator *= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) * rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator /= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) / rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator %= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) % rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator += (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) + rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator -= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) - rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator <<= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) << rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator >>= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) >> rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator &= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) & rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator ^= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) ^ rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

inline uchar uchar16_idx::operator |= (uchar rhs)
{
    uchar t = (uchar)(spu_extract((vec_uchar16)ref, i) | rhs);
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return t;
}

}  // namespace simd

#endif
