// bool4 - 4-lane boolean SIMD type, backed by vec_uint4.
//
// "false" is stored as 0; "true" as all-1 bits.  Implicit conversion to
// vec_uint4 surfaces that mask form for use as a select() control.

#ifndef PS3TC_SPU_SIMD_BOOL4_H
#define PS3TC_SPU_SIMD_BOOL4_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool4;
class int4;
class uint4;
class float4;
class bool4_idx;

class bool4
{
private:
    vec_uint4 data;

public:
    inline bool4() {}
    inline bool4(int4);
    inline bool4(uint4);
    inline bool4(float4);
    inline bool4(bool r0, bool r1, bool r2, bool r3);

    explicit inline bool4(bool);

    // Implicit ctor from a raw mask (any nonzero lane becomes true).
    inline bool4(vec_uint4 rhs);

    // Surface as a vec_uint4 mask suitable for spu_sel.
    inline operator vector unsigned int() const;

    inline bool4_idx operator [] (int i);
    inline bool       operator [] (int i) const;

    inline const bool4 operator ! () const;

    inline bool4& operator = (bool4 rhs);
    inline bool4& operator &= (bool4 rhs);
    inline bool4& operator ^= (bool4 rhs);
    inline bool4& operator |= (bool4 rhs);

    friend inline const bool4 operator == (bool4 lhs, bool4 rhs);
    friend inline const bool4 operator != (bool4 lhs, bool4 rhs);
    friend inline const bool4 operator < (int4 lhs, int4 rhs);
    friend inline const bool4 operator <= (int4 lhs, int4 rhs);
    friend inline const bool4 operator > (int4 lhs, int4 rhs);
    friend inline const bool4 operator >= (int4 lhs, int4 rhs);
    friend inline const bool4 operator == (int4 lhs, int4 rhs);
    friend inline const bool4 operator != (int4 lhs, int4 rhs);
    friend inline const bool4 operator < (uint4 lhs, uint4 rhs);
    friend inline const bool4 operator <= (uint4 lhs, uint4 rhs);
    friend inline const bool4 operator > (uint4 lhs, uint4 rhs);
    friend inline const bool4 operator >= (uint4 lhs, uint4 rhs);
    friend inline const bool4 operator == (uint4 lhs, uint4 rhs);
    friend inline const bool4 operator != (uint4 lhs, uint4 rhs);
    friend inline const bool4 operator < (float4 lhs, float4 rhs);
    friend inline const bool4 operator <= (float4 lhs, float4 rhs);
    friend inline const bool4 operator > (float4 lhs, float4 rhs);
    friend inline const bool4 operator >= (float4 lhs, float4 rhs);
    friend inline const bool4 operator == (float4 lhs, float4 rhs);
    friend inline const bool4 operator != (float4 lhs, float4 rhs);
    friend class bool4_idx;
};

inline const bool4 operator == (bool4 lhs, bool4 rhs);
inline const bool4 operator != (bool4 lhs, bool4 rhs);

inline const bool4 select(bool4 lhs, bool4 rhs, bool4 rhs_slots);

inline const bool4 operator & (bool4 lhs, bool4 rhs);
inline const bool4 operator ^ (bool4 lhs, bool4 rhs);
inline const bool4 operator | (bool4 lhs, bool4 rhs);

// Reduction: pack the 4 bool lanes into the low 4 bits of a uint.
inline uint gather(bool4 rhs);
inline bool any(bool4 rhs);
inline bool all(bool4 rhs);

class bool4_idx
{
private:
    bool4 &ref __attribute__((aligned(16)));
    int    i   __attribute__((aligned(16)));
public:
    inline bool4_idx(bool4& vec, int idx) : ref(vec) { i = idx; }
    inline operator bool() const;
    inline bool operator = (bool rhs);
    inline bool operator = (const bool4_idx& rhs);
    inline bool operator &= (bool rhs);
    inline bool operator ^= (bool rhs);
    inline bool operator |= (bool rhs);
};

}  // namespace simd

#include "simd_int4.h"
#include "simd_uint4.h"
#include "simd_float4.h"

namespace simd {

inline bool4::bool4(bool rhs)
{
    data = spu_splats((uint)-(int)rhs);
}

inline bool4::bool4(vec_uint4 rhs)
{
    // Normalise: zero stays zero, nonzero becomes all-ones.
    data = spu_cmpgt(rhs, (uint)0);
}

inline bool4::bool4(int4 rhs)
{
    data = spu_cmpgt((vec_uint4)(vec_int4)rhs, (uint)0);
}

inline bool4::bool4(uint4 rhs)
{
    data = spu_cmpgt((vec_uint4)rhs, (uint)0);
}

inline bool4::bool4(float4 rhs)
{
    *this = (rhs != float4(0.0f));
}

inline bool4::bool4(bool r0, bool r1, bool r2, bool r3)
{
    data = (vec_uint4){ (uint)-(int)r0, (uint)-(int)r1,
                        (uint)-(int)r2, (uint)-(int)r3 };
}

inline bool4::operator vector unsigned int() const { return data; }

inline bool4_idx bool4::operator [] (int i) { return bool4_idx(*this, i); }
inline bool      bool4::operator [] (int i) const
{
    return (bool)spu_extract(data, i);
}

inline const bool4 bool4::operator ! () const
{
    bool4 r;
    r.data = spu_nor(data, data);
    return r;
}

inline bool4& bool4::operator =  (bool4 rhs) { data = rhs.data; return *this; }
inline bool4& bool4::operator &= (bool4 rhs) { *this = *this & rhs; return *this; }
inline bool4& bool4::operator ^= (bool4 rhs) { *this = *this ^ rhs; return *this; }
inline bool4& bool4::operator |= (bool4 rhs) { *this = *this | rhs; return *this; }

inline const bool4 operator == (bool4 lhs, bool4 rhs)
{
    bool4 r;
    r.data = spu_cmpeq((vec_uint4)lhs, (vec_uint4)rhs);
    return r;
}

inline const bool4 operator != (bool4 lhs, bool4 rhs) { return !(lhs == rhs); }

inline const bool4 select(bool4 lhs, bool4 rhs, bool4 rhs_slots)
{
    return bool4(spu_sel((vec_uint4)lhs, (vec_uint4)rhs, (vec_uint4)rhs_slots));
}

inline const bool4 operator & (bool4 lhs, bool4 rhs)
{
    return bool4(spu_and((vec_uint4)lhs, (vec_uint4)rhs));
}

inline const bool4 operator | (bool4 lhs, bool4 rhs)
{
    return bool4(spu_or((vec_uint4)lhs, (vec_uint4)rhs));
}

inline const bool4 operator ^ (bool4 lhs, bool4 rhs)
{
    return bool4(spu_xor((vec_uint4)lhs, (vec_uint4)rhs));
}

inline uint gather(bool4 rhs)
{
    return spu_extract(spu_gather((vec_uint4)rhs), 0);
}

inline bool any(bool4 rhs) { return gather(rhs) != 0; }
inline bool all(bool4 rhs) { return gather(rhs) == 0xfu; }

inline bool4_idx::operator bool() const
{
    return (bool)spu_extract((vec_uint4)ref, i);
}

inline bool bool4_idx::operator = (bool rhs)
{
    ref.data = spu_insert((uint)-(int)rhs, (vec_uint4)ref, i);
    return rhs;
}

inline bool bool4_idx::operator = (const bool4_idx& rhs)
{
    return *this = (bool)rhs;
}

inline bool bool4_idx::operator &= (bool rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) & (uint)-(int)rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return (bool)t;
}

inline bool bool4_idx::operator ^= (bool rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) ^ (uint)-(int)rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return (bool)t;
}

inline bool bool4_idx::operator |= (bool rhs)
{
    uint t = spu_extract((vec_uint4)ref, i) | (uint)-(int)rhs;
    ref = spu_insert(t, (vec_uint4)ref, i);
    return (bool)t;
}

}  // namespace simd

#endif
