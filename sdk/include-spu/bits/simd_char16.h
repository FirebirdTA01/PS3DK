// char16 - 16-lane signed-char SIMD type, backed by vec_char16.

#ifndef PS3TC_SPU_SIMD_CHAR16_H
#define PS3TC_SPU_SIMD_CHAR16_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool16;
class uchar16;
class char16_idx;

class char16
{
private:
    vec_char16 data;

public:
    inline char16() {}
    inline char16(bool16);
    inline char16(uchar16);
    inline char16(schar r0,  schar r1,  schar r2,  schar r3,
                  schar r4,  schar r5,  schar r6,  schar r7,
                  schar r8,  schar r9,  schar r10, schar r11,
                  schar r12, schar r13, schar r14, schar r15);

    explicit inline char16(schar);

    inline char16(vec_char16 rhs);
    inline operator vector signed char() const;

    inline char16_idx operator [] (int i);
    inline schar       operator [] (int i) const;

    inline const char16 operator ++ (int);
    inline const char16 operator -- (int);
    inline char16& operator ++ ();
    inline char16& operator -- ();

    inline const char16 operator ~ () const;
    inline const bool16 operator ! () const;
    inline const char16 operator - () const;

    inline char16& operator =  (char16 rhs);
    inline char16& operator *= (char16 rhs);
    inline char16& operator /= (char16 rhs);
    inline char16& operator %= (char16 rhs);
    inline char16& operator += (char16 rhs);
    inline char16& operator -= (char16 rhs);
    inline char16& operator <<= (uchar16 rhs);
    inline char16& operator >>= (uchar16 rhs);
    inline char16& operator &= (char16 rhs);
    inline char16& operator ^= (char16 rhs);
    inline char16& operator |= (char16 rhs);
};

inline const char16 operator * (char16 lhs, char16 rhs);
inline const char16 operator / (char16 lhs, char16 rhs);
inline const char16 operator % (char16 lhs, char16 rhs);
inline const char16 operator + (char16 lhs, char16 rhs);
inline const char16 operator - (char16 lhs, char16 rhs);

inline const char16 operator << (char16 lhs, uchar16 rhs);
inline const char16 operator >> (char16 lhs, uchar16 rhs);

inline const bool16 operator < (char16 lhs, char16 rhs);
inline const bool16 operator <= (char16 lhs, char16 rhs);
inline const bool16 operator > (char16 lhs, char16 rhs);
inline const bool16 operator >= (char16 lhs, char16 rhs);
inline const bool16 operator == (char16 lhs, char16 rhs);
inline const bool16 operator != (char16 lhs, char16 rhs);

inline const char16 select(char16 lhs, char16 rhs, bool16 rhs_slots);

inline const char16 operator & (char16 lhs, char16 rhs);
inline const char16 operator ^ (char16 lhs, char16 rhs);
inline const char16 operator | (char16 lhs, char16 rhs);

class char16_idx
{
private:
    char16 &ref __attribute__((aligned(16)));
    int     i   __attribute__((aligned(16)));
public:
    inline char16_idx(char16& vec, int idx) : ref(vec) { i = idx; }
    inline operator schar() const;
    inline schar operator = (schar rhs);
    inline schar operator = (const char16_idx& rhs);
    inline schar operator ++ (int);
    inline schar operator -- (int);
    inline schar operator ++ ();
    inline schar operator -- ();
    inline schar operator *= (schar rhs);
    inline schar operator /= (schar rhs);
    inline schar operator %= (schar rhs);
    inline schar operator += (schar rhs);
    inline schar operator -= (schar rhs);
    inline schar operator <<= (uchar rhs);
    inline schar operator >>= (uchar rhs);
    inline schar operator &= (schar rhs);
    inline schar operator ^= (schar rhs);
    inline schar operator |= (schar rhs);
};

}  // namespace simd

#include "simd_bool16.h"
#include "simd_uchar16.h"

namespace simd {

inline char16::char16(schar rhs)         { data = spu_splats((schar)rhs); }
inline char16::char16(vec_char16 rhs)    { data = rhs; }

inline char16::char16(bool16 rhs)
{
    data = (vec_char16)spu_and((vec_uchar16)rhs, (uchar)1);
}

inline char16::char16(uchar16 rhs)
{
    data = (vec_char16)(vec_uchar16)rhs;
}

inline char16::char16(schar r0,  schar r1,  schar r2,  schar r3,
                      schar r4,  schar r5,  schar r6,  schar r7,
                      schar r8,  schar r9,  schar r10, schar r11,
                      schar r12, schar r13, schar r14, schar r15)
{
    data = (vec_char16){
        r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15
    };
}

inline char16::operator vector signed char() const { return data; }

inline char16_idx char16::operator [] (int i) { return char16_idx(*this, i); }
inline schar      char16::operator [] (int i) const
{
    return (schar)spu_extract(data, i);
}

inline const char16 char16::operator ++ (int) { vec_char16 p = data; operator ++(); return char16(p); }
inline const char16 char16::operator -- (int) { vec_char16 p = data; operator --(); return char16(p); }
inline char16& char16::operator ++ () { *this += char16(1); return *this; }
inline char16& char16::operator -- () { *this -= char16(1); return *this; }

inline const char16 char16::operator ~ () const { return char16(spu_nor(data, data)); }
inline const bool16 char16::operator ! () const { return *this == char16(0); }
inline const char16 char16::operator - () const { return char16(0) - *this; }

inline char16& char16::operator =  (char16 rhs) { data = rhs.data; return *this; }
inline char16& char16::operator *= (char16 rhs) { *this = *this *  rhs; return *this; }
inline char16& char16::operator /= (char16 rhs) { *this = *this /  rhs; return *this; }
inline char16& char16::operator %= (char16 rhs) { *this = *this %  rhs; return *this; }
inline char16& char16::operator += (char16 rhs) { *this = *this +  rhs; return *this; }
inline char16& char16::operator -= (char16 rhs) { *this = *this -  rhs; return *this; }
inline char16& char16::operator <<= (uchar16 rhs) { *this = *this << rhs; return *this; }
inline char16& char16::operator >>= (uchar16 rhs) { *this = *this >> rhs; return *this; }
inline char16& char16::operator &= (char16 rhs) { *this = *this & rhs; return *this; }
inline char16& char16::operator ^= (char16 rhs) { *this = *this ^ rhs; return *this; }
inline char16& char16::operator |= (char16 rhs) { *this = *this | rhs; return *this; }

inline const char16 operator * (char16 lhs, char16 rhs)
{
    schar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (schar)((int)spu_extract((vec_char16)lhs, i) *
                       (int)spu_extract((vec_char16)rhs, i));
    return char16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                  r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const char16 operator / (char16 num, char16 den)
{
    schar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (schar)(spu_extract((vec_char16)num, i) /
                       spu_extract((vec_char16)den, i));
    return char16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                  r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const char16 operator % (char16 num, char16 den)
{
    schar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (schar)(spu_extract((vec_char16)num, i) %
                       spu_extract((vec_char16)den, i));
    return char16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                  r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const char16 operator + (char16 lhs, char16 rhs)
{
    // No native byte add on SPU; promote even/odd halves to short.
    vec_short8 le = spu_extend((vec_char16)lhs);                     // odd lanes (0,2,4,...)
    vec_short8 re = spu_extend((vec_char16)rhs);
    vec_short8 lo = spu_extend((vec_char16)spu_rlqwbyte((vec_char16)lhs, 1)); // even lanes shifted in
    vec_short8 ro = spu_extend((vec_char16)spu_rlqwbyte((vec_char16)rhs, 1));
    vec_short8 se = spu_add(le, re);
    vec_short8 so = spu_add(lo, ro);
    // Repack low byte of each lane back into 16-byte vector.
    const vec_uchar16 pack = (vec_uchar16){
        0x01, 0x11, 0x03, 0x13, 0x05, 0x15, 0x07, 0x17,
        0x09, 0x19, 0x0b, 0x1b, 0x0d, 0x1d, 0x0f, 0x1f
    };
    return char16((vec_char16)spu_shuffle((vec_char16)so, (vec_char16)se, pack));
}

inline const char16 operator - (char16 lhs, char16 rhs)
{
    vec_short8 le = spu_extend((vec_char16)lhs);
    vec_short8 re = spu_extend((vec_char16)rhs);
    vec_short8 lo = spu_extend((vec_char16)spu_rlqwbyte((vec_char16)lhs, 1));
    vec_short8 ro = spu_extend((vec_char16)spu_rlqwbyte((vec_char16)rhs, 1));
    vec_short8 se = spu_sub(le, re);
    vec_short8 so = spu_sub(lo, ro);
    const vec_uchar16 pack = (vec_uchar16){
        0x01, 0x11, 0x03, 0x13, 0x05, 0x15, 0x07, 0x17,
        0x09, 0x19, 0x0b, 0x1b, 0x0d, 0x1d, 0x0f, 0x1f
    };
    return char16((vec_char16)spu_shuffle((vec_char16)so, (vec_char16)se, pack));
}

inline const char16 operator << (char16 lhs, uchar16 rhs)
{
    schar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (schar)(spu_extract((vec_char16)lhs, i) <<
                       (spu_extract((vec_uchar16)rhs, i) & 7));
    return char16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                  r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const char16 operator >> (char16 lhs, uchar16 rhs)
{
    schar r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = (schar)(spu_extract((vec_char16)lhs, i) >>
                       (spu_extract((vec_uchar16)rhs, i) & 7));
    return char16(r[0],  r[1],  r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                  r[8],  r[9],  r[10], r[11], r[12], r[13], r[14], r[15]);
}

inline const bool16 operator < (char16 lhs, char16 rhs)
{
    bool16 r;
    r.data = spu_cmpgt((vec_char16)rhs, (vec_char16)lhs);
    return r;
}

inline const bool16 operator <= (char16 lhs, char16 rhs) { return !(lhs > rhs); }

inline const bool16 operator > (char16 lhs, char16 rhs)
{
    bool16 r;
    r.data = spu_cmpgt((vec_char16)lhs, (vec_char16)rhs);
    return r;
}

inline const bool16 operator >= (char16 lhs, char16 rhs) { return !(lhs < rhs); }

inline const bool16 operator == (char16 lhs, char16 rhs)
{
    bool16 r;
    r.data = spu_cmpeq((vec_char16)lhs, (vec_char16)rhs);
    return r;
}

inline const bool16 operator != (char16 lhs, char16 rhs) { return !(lhs == rhs); }

inline const char16 select(char16 lhs, char16 rhs, bool16 rhs_slots)
{
    return char16(spu_sel((vec_char16)lhs, (vec_char16)rhs, (vec_uchar16)rhs_slots));
}

inline const char16 operator & (char16 lhs, char16 rhs) { return char16(spu_and((vec_char16)lhs, (vec_char16)rhs)); }
inline const char16 operator | (char16 lhs, char16 rhs) { return char16(spu_or((vec_char16)lhs, (vec_char16)rhs)); }
inline const char16 operator ^ (char16 lhs, char16 rhs) { return char16(spu_xor((vec_char16)lhs, (vec_char16)rhs)); }

inline char16_idx::operator schar() const
{
    return (schar)spu_extract((vec_char16)ref, i);
}

inline schar char16_idx::operator = (schar rhs)
{
    ref = spu_insert(rhs, (vec_char16)ref, i);
    return rhs;
}

inline schar char16_idx::operator = (const char16_idx& rhs)
{
    return *this = (schar)rhs;
}

inline schar char16_idx::operator ++ (int)
{
    schar t = spu_extract((vec_char16)ref, i);
    ref = spu_insert((schar)(t + 1), (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator -- (int)
{
    schar t = spu_extract((vec_char16)ref, i);
    ref = spu_insert((schar)(t - 1), (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator ++ ()
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) + 1);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator -- ()
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) - 1);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator *= (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) * rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator /= (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) / rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator %= (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) % rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator += (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) + rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator -= (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) - rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator <<= (uchar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) << rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator >>= (uchar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) >> rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator &= (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) & rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator ^= (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) ^ rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

inline schar char16_idx::operator |= (schar rhs)
{
    schar t = (schar)(spu_extract((vec_char16)ref, i) | rhs);
    ref = spu_insert(t, (vec_char16)ref, i);
    return t;
}

}  // namespace simd

#endif
