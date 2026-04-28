// bool8 - 8-lane boolean SIMD type, backed by vec_ushort8.

#ifndef PS3TC_SPU_SIMD_BOOL8_H
#define PS3TC_SPU_SIMD_BOOL8_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool8;
class short8;
class ushort8;
class bool8_idx;

class bool8
{
private:
    vec_ushort8 data;

public:
    inline bool8() {}
    inline bool8(short8);
    inline bool8(ushort8);
    inline bool8(bool r0, bool r1, bool r2, bool r3,
                 bool r4, bool r5, bool r6, bool r7);

    explicit inline bool8(bool);

    inline bool8(vec_ushort8 rhs);
    inline operator vector unsigned short() const;

    inline bool8_idx operator [] (int i);
    inline bool       operator [] (int i) const;

    inline const bool8 operator ! () const;

    inline bool8& operator = (bool8 rhs);
    inline bool8& operator &= (bool8 rhs);
    inline bool8& operator ^= (bool8 rhs);
    inline bool8& operator |= (bool8 rhs);

    friend inline const bool8 operator == (bool8 lhs, bool8 rhs);
    friend inline const bool8 operator != (bool8 lhs, bool8 rhs);
    friend inline const bool8 operator < (short8 lhs, short8 rhs);
    friend inline const bool8 operator <= (short8 lhs, short8 rhs);
    friend inline const bool8 operator > (short8 lhs, short8 rhs);
    friend inline const bool8 operator >= (short8 lhs, short8 rhs);
    friend inline const bool8 operator == (short8 lhs, short8 rhs);
    friend inline const bool8 operator != (short8 lhs, short8 rhs);
    friend inline const bool8 operator < (ushort8 lhs, ushort8 rhs);
    friend inline const bool8 operator <= (ushort8 lhs, ushort8 rhs);
    friend inline const bool8 operator > (ushort8 lhs, ushort8 rhs);
    friend inline const bool8 operator >= (ushort8 lhs, ushort8 rhs);
    friend inline const bool8 operator == (ushort8 lhs, ushort8 rhs);
    friend inline const bool8 operator != (ushort8 lhs, ushort8 rhs);
    friend class bool8_idx;
};

inline const bool8 operator == (bool8 lhs, bool8 rhs);
inline const bool8 operator != (bool8 lhs, bool8 rhs);

inline const bool8 select(bool8 lhs, bool8 rhs, bool8 rhs_slots);

inline const bool8 operator & (bool8 lhs, bool8 rhs);
inline const bool8 operator ^ (bool8 lhs, bool8 rhs);
inline const bool8 operator | (bool8 lhs, bool8 rhs);

inline uint gather(bool8 rhs);
inline bool any(bool8 rhs);
inline bool all(bool8 rhs);

class bool8_idx
{
private:
    bool8 &ref __attribute__((aligned(16)));
    int    i   __attribute__((aligned(16)));
public:
    inline bool8_idx(bool8& vec, int idx) : ref(vec) { i = idx; }
    inline operator bool() const;
    inline bool operator = (bool rhs);
    inline bool operator = (const bool8_idx& rhs);
    inline bool operator &= (bool rhs);
    inline bool operator ^= (bool rhs);
    inline bool operator |= (bool rhs);
};

}  // namespace simd

#include "simd_short8.h"
#include "simd_ushort8.h"

namespace simd {

inline bool8::bool8(bool rhs)
{
    data = spu_splats((ushort)-(int)rhs);
}

inline bool8::bool8(vec_ushort8 rhs)
{
    data = spu_cmpgt(rhs, (ushort)0);
}

inline bool8::bool8(short8 rhs)
{
    data = spu_cmpgt((vec_ushort8)(vec_short8)rhs, (ushort)0);
}

inline bool8::bool8(ushort8 rhs)
{
    data = spu_cmpgt((vec_ushort8)rhs, (ushort)0);
}

inline bool8::bool8(bool r0, bool r1, bool r2, bool r3,
                    bool r4, bool r5, bool r6, bool r7)
{
    data = (vec_ushort8){
        (ushort)-(int)r0, (ushort)-(int)r1, (ushort)-(int)r2, (ushort)-(int)r3,
        (ushort)-(int)r4, (ushort)-(int)r5, (ushort)-(int)r6, (ushort)-(int)r7
    };
}

inline bool8::operator vector unsigned short() const { return data; }

inline bool8_idx bool8::operator [] (int i) { return bool8_idx(*this, i); }
inline bool      bool8::operator [] (int i) const
{
    return (bool)spu_extract(data, i);
}

inline const bool8 bool8::operator ! () const
{
    bool8 r;
    r.data = spu_nor(data, data);
    return r;
}

inline bool8& bool8::operator =  (bool8 rhs) { data = rhs.data; return *this; }
inline bool8& bool8::operator &= (bool8 rhs) { *this = *this & rhs; return *this; }
inline bool8& bool8::operator ^= (bool8 rhs) { *this = *this ^ rhs; return *this; }
inline bool8& bool8::operator |= (bool8 rhs) { *this = *this | rhs; return *this; }

inline const bool8 operator == (bool8 lhs, bool8 rhs)
{
    bool8 r;
    r.data = spu_cmpeq((vec_ushort8)lhs, (vec_ushort8)rhs);
    return r;
}

inline const bool8 operator != (bool8 lhs, bool8 rhs) { return !(lhs == rhs); }

inline const bool8 select(bool8 lhs, bool8 rhs, bool8 rhs_slots)
{
    return bool8(spu_sel((vec_ushort8)lhs, (vec_ushort8)rhs, (vec_ushort8)rhs_slots));
}

inline const bool8 operator & (bool8 lhs, bool8 rhs)
{
    return bool8(spu_and((vec_ushort8)lhs, (vec_ushort8)rhs));
}

inline const bool8 operator | (bool8 lhs, bool8 rhs)
{
    return bool8(spu_or((vec_ushort8)lhs, (vec_ushort8)rhs));
}

inline const bool8 operator ^ (bool8 lhs, bool8 rhs)
{
    return bool8(spu_xor((vec_ushort8)lhs, (vec_ushort8)rhs));
}

inline uint gather(bool8 rhs)
{
    return spu_extract(spu_gather((vec_ushort8)rhs), 0);
}

inline bool any(bool8 rhs) { return gather(rhs) != 0; }
inline bool all(bool8 rhs) { return gather(rhs) == 0xffu; }

inline bool8_idx::operator bool() const
{
    return (bool)spu_extract((vec_ushort8)ref, i);
}

inline bool bool8_idx::operator = (bool rhs)
{
    ref.data = spu_insert((ushort)-(int)rhs, (vec_ushort8)ref, i);
    return rhs;
}

inline bool bool8_idx::operator = (const bool8_idx& rhs)
{
    return *this = (bool)rhs;
}

inline bool bool8_idx::operator &= (bool rhs)
{
    ushort t = spu_extract((vec_ushort8)ref, i) & (ushort)-(int)rhs;
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return (bool)t;
}

inline bool bool8_idx::operator ^= (bool rhs)
{
    ushort t = spu_extract((vec_ushort8)ref, i) ^ (ushort)-(int)rhs;
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return (bool)t;
}

inline bool bool8_idx::operator |= (bool rhs)
{
    ushort t = spu_extract((vec_ushort8)ref, i) | (ushort)-(int)rhs;
    ref = spu_insert(t, (vec_ushort8)ref, i);
    return (bool)t;
}

}  // namespace simd

#endif
