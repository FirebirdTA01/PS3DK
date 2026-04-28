// bool16 - 16-lane boolean SIMD type, backed by vec_uchar16.

#ifndef PS3TC_SPU_SIMD_BOOL16_H
#define PS3TC_SPU_SIMD_BOOL16_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool16;
class char16;
class uchar16;
class bool16_idx;

class bool16
{
private:
    vec_uchar16 data;

public:
    inline bool16() {}
    inline bool16(char16);
    inline bool16(uchar16);
    inline bool16(bool r0,  bool r1,  bool r2,  bool r3,
                  bool r4,  bool r5,  bool r6,  bool r7,
                  bool r8,  bool r9,  bool r10, bool r11,
                  bool r12, bool r13, bool r14, bool r15);

    explicit inline bool16(bool);

    inline bool16(vec_uchar16 rhs);
    inline operator vector unsigned char() const;

    inline bool16_idx operator [] (int i);
    inline bool        operator [] (int i) const;

    inline const bool16 operator ! () const;

    inline bool16& operator = (bool16 rhs);
    inline bool16& operator &= (bool16 rhs);
    inline bool16& operator ^= (bool16 rhs);
    inline bool16& operator |= (bool16 rhs);

    friend inline const bool16 operator == (bool16 lhs, bool16 rhs);
    friend inline const bool16 operator != (bool16 lhs, bool16 rhs);
    friend inline const bool16 operator < (char16 lhs, char16 rhs);
    friend inline const bool16 operator <= (char16 lhs, char16 rhs);
    friend inline const bool16 operator > (char16 lhs, char16 rhs);
    friend inline const bool16 operator >= (char16 lhs, char16 rhs);
    friend inline const bool16 operator == (char16 lhs, char16 rhs);
    friend inline const bool16 operator != (char16 lhs, char16 rhs);
    friend inline const bool16 operator < (uchar16 lhs, uchar16 rhs);
    friend inline const bool16 operator <= (uchar16 lhs, uchar16 rhs);
    friend inline const bool16 operator > (uchar16 lhs, uchar16 rhs);
    friend inline const bool16 operator >= (uchar16 lhs, uchar16 rhs);
    friend inline const bool16 operator == (uchar16 lhs, uchar16 rhs);
    friend inline const bool16 operator != (uchar16 lhs, uchar16 rhs);
    friend class bool16_idx;
};

inline const bool16 operator == (bool16 lhs, bool16 rhs);
inline const bool16 operator != (bool16 lhs, bool16 rhs);

inline const bool16 select(bool16 lhs, bool16 rhs, bool16 rhs_slots);

inline const bool16 operator & (bool16 lhs, bool16 rhs);
inline const bool16 operator ^ (bool16 lhs, bool16 rhs);
inline const bool16 operator | (bool16 lhs, bool16 rhs);

inline uint gather(bool16 rhs);
inline bool any(bool16 rhs);
inline bool all(bool16 rhs);

class bool16_idx
{
private:
    bool16 &ref __attribute__((aligned(16)));
    int     i   __attribute__((aligned(16)));
public:
    inline bool16_idx(bool16& vec, int idx) : ref(vec) { i = idx; }
    inline operator bool() const;
    inline bool operator = (bool rhs);
    inline bool operator = (const bool16_idx& rhs);
    inline bool operator &= (bool rhs);
    inline bool operator ^= (bool rhs);
    inline bool operator |= (bool rhs);
};

}  // namespace simd

#include "simd_char16.h"
#include "simd_uchar16.h"

namespace simd {

inline bool16::bool16(bool rhs)
{
    data = spu_splats((uchar)-(int)rhs);
}

inline bool16::bool16(vec_uchar16 rhs)
{
    data = spu_cmpgt(rhs, (uchar)0);
}

inline bool16::bool16(char16 rhs)
{
    data = spu_cmpgt((vec_uchar16)(vec_char16)rhs, (uchar)0);
}

inline bool16::bool16(uchar16 rhs)
{
    data = spu_cmpgt((vec_uchar16)rhs, (uchar)0);
}

inline bool16::bool16(bool r0,  bool r1,  bool r2,  bool r3,
                      bool r4,  bool r5,  bool r6,  bool r7,
                      bool r8,  bool r9,  bool r10, bool r11,
                      bool r12, bool r13, bool r14, bool r15)
{
    data = (vec_uchar16){
        (uchar)-(int)r0,  (uchar)-(int)r1,  (uchar)-(int)r2,  (uchar)-(int)r3,
        (uchar)-(int)r4,  (uchar)-(int)r5,  (uchar)-(int)r6,  (uchar)-(int)r7,
        (uchar)-(int)r8,  (uchar)-(int)r9,  (uchar)-(int)r10, (uchar)-(int)r11,
        (uchar)-(int)r12, (uchar)-(int)r13, (uchar)-(int)r14, (uchar)-(int)r15
    };
}

inline bool16::operator vector unsigned char() const { return data; }

inline bool16_idx bool16::operator [] (int i) { return bool16_idx(*this, i); }
inline bool       bool16::operator [] (int i) const
{
    return (bool)spu_extract(data, i);
}

inline const bool16 bool16::operator ! () const
{
    bool16 r;
    r.data = spu_nor(data, data);
    return r;
}

inline bool16& bool16::operator =  (bool16 rhs) { data = rhs.data; return *this; }
inline bool16& bool16::operator &= (bool16 rhs) { *this = *this & rhs; return *this; }
inline bool16& bool16::operator ^= (bool16 rhs) { *this = *this ^ rhs; return *this; }
inline bool16& bool16::operator |= (bool16 rhs) { *this = *this | rhs; return *this; }

inline const bool16 operator == (bool16 lhs, bool16 rhs)
{
    bool16 r;
    r.data = spu_cmpeq((vec_uchar16)lhs, (vec_uchar16)rhs);
    return r;
}

inline const bool16 operator != (bool16 lhs, bool16 rhs) { return !(lhs == rhs); }

inline const bool16 select(bool16 lhs, bool16 rhs, bool16 rhs_slots)
{
    return bool16(spu_sel((vec_uchar16)lhs, (vec_uchar16)rhs, (vec_uchar16)rhs_slots));
}

inline const bool16 operator & (bool16 lhs, bool16 rhs)
{
    return bool16(spu_and((vec_uchar16)lhs, (vec_uchar16)rhs));
}

inline const bool16 operator | (bool16 lhs, bool16 rhs)
{
    return bool16(spu_or((vec_uchar16)lhs, (vec_uchar16)rhs));
}

inline const bool16 operator ^ (bool16 lhs, bool16 rhs)
{
    return bool16(spu_xor((vec_uchar16)lhs, (vec_uchar16)rhs));
}

inline uint gather(bool16 rhs)
{
    return spu_extract(spu_gather((vec_uchar16)rhs), 0);
}

inline bool any(bool16 rhs) { return gather(rhs) != 0; }
inline bool all(bool16 rhs) { return gather(rhs) == 0xffffu; }

inline bool16_idx::operator bool() const
{
    return (bool)spu_extract((vec_uchar16)ref, i);
}

inline bool bool16_idx::operator = (bool rhs)
{
    ref.data = spu_insert((uchar)-(int)rhs, (vec_uchar16)ref, i);
    return rhs;
}

inline bool bool16_idx::operator = (const bool16_idx& rhs)
{
    return *this = (bool)rhs;
}

inline bool bool16_idx::operator &= (bool rhs)
{
    uchar t = spu_extract((vec_uchar16)ref, i) & (uchar)-(int)rhs;
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return (bool)t;
}

inline bool bool16_idx::operator ^= (bool rhs)
{
    uchar t = spu_extract((vec_uchar16)ref, i) ^ (uchar)-(int)rhs;
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return (bool)t;
}

inline bool bool16_idx::operator |= (bool rhs)
{
    uchar t = spu_extract((vec_uchar16)ref, i) | (uchar)-(int)rhs;
    ref = spu_insert(t, (vec_uchar16)ref, i);
    return (bool)t;
}

}  // namespace simd

#endif
