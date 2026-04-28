// bool2 - 2-lane boolean SIMD type, backed by vec_ullong2.

#ifndef PS3TC_SPU_SIMD_BOOL2_H
#define PS3TC_SPU_SIMD_BOOL2_H

#include <spu_intrinsics.h>
#include "simd_scalars.h"

namespace simd {

class bool2;
class llong2;
class ullong2;
class double2;
class bool2_idx;

class bool2
{
private:
    vec_ullong2 data;

public:
    inline bool2() {}
    inline bool2(llong2);
    inline bool2(ullong2);
    inline bool2(double2);
    inline bool2(bool r0, bool r1);

    explicit inline bool2(bool);

    inline bool2(vec_ullong2 rhs);
    inline operator vector unsigned long long() const;

    inline bool2_idx operator [] (int i);
    inline bool       operator [] (int i) const;

    inline const bool2 operator ! () const;

    inline bool2& operator = (bool2 rhs);
    inline bool2& operator &= (bool2 rhs);
    inline bool2& operator ^= (bool2 rhs);
    inline bool2& operator |= (bool2 rhs);

    friend inline const bool2 operator == (bool2 lhs, bool2 rhs);
    friend inline const bool2 operator != (bool2 lhs, bool2 rhs);
    friend inline const bool2 operator < (llong2 lhs, llong2 rhs);
    friend inline const bool2 operator <= (llong2 lhs, llong2 rhs);
    friend inline const bool2 operator > (llong2 lhs, llong2 rhs);
    friend inline const bool2 operator >= (llong2 lhs, llong2 rhs);
    friend inline const bool2 operator == (llong2 lhs, llong2 rhs);
    friend inline const bool2 operator != (llong2 lhs, llong2 rhs);
    friend inline const bool2 operator < (ullong2 lhs, ullong2 rhs);
    friend inline const bool2 operator <= (ullong2 lhs, ullong2 rhs);
    friend inline const bool2 operator > (ullong2 lhs, ullong2 rhs);
    friend inline const bool2 operator >= (ullong2 lhs, ullong2 rhs);
    friend inline const bool2 operator == (ullong2 lhs, ullong2 rhs);
    friend inline const bool2 operator != (ullong2 lhs, ullong2 rhs);
    friend inline const bool2 operator < (double2 lhs, double2 rhs);
    friend inline const bool2 operator <= (double2 lhs, double2 rhs);
    friend inline const bool2 operator > (double2 lhs, double2 rhs);
    friend inline const bool2 operator >= (double2 lhs, double2 rhs);
    friend inline const bool2 operator == (double2 lhs, double2 rhs);
    friend inline const bool2 operator != (double2 lhs, double2 rhs);
    friend class bool2_idx;
};

inline const bool2 operator == (bool2 lhs, bool2 rhs);
inline const bool2 operator != (bool2 lhs, bool2 rhs);

inline const bool2 select(bool2 lhs, bool2 rhs, bool2 rhs_slots);

inline const bool2 operator & (bool2 lhs, bool2 rhs);
inline const bool2 operator ^ (bool2 lhs, bool2 rhs);
inline const bool2 operator | (bool2 lhs, bool2 rhs);

inline uint gather(bool2 rhs);
inline bool any(bool2 rhs);
inline bool all(bool2 rhs);

class bool2_idx
{
private:
    bool2 &ref __attribute__((aligned(16)));
    int    i   __attribute__((aligned(16)));
public:
    inline bool2_idx(bool2& vec, int idx) : ref(vec) { i = idx; }
    inline operator bool() const;
    inline bool operator = (bool rhs);
    inline bool operator = (const bool2_idx& rhs);
    inline bool operator &= (bool rhs);
    inline bool operator ^= (bool rhs);
    inline bool operator |= (bool rhs);
};

}  // namespace simd

#include "simd_llong2.h"
#include "simd_ullong2.h"
#include "simd_double2.h"

namespace simd {

// Helper: turn any nonzero 64-bit lane into all-ones (and zero into zero).
// vec_ullong2 has no direct cmpgt; fold the two 32-bit halves of each
// lane down to a single mask using cmpeq-with-zero on the integer view.
namespace bool2_detail {
    inline vec_ullong2 mask_nonzero_u64(vec_ullong2 v)
    {
        const vec_uchar16 lo32 = (vec_uchar16)(vec_uint4){
            0x00010203u, 0x00010203u, 0x08090a0bu, 0x08090a0bu };
        const vec_uchar16 hi32 = (vec_uchar16)(vec_uint4){
            0x04050607u, 0x04050607u, 0x0c0d0e0fu, 0x0c0d0e0fu };
        vec_uint4 z = spu_cmpeq((vec_uint4)v, 0u);  // 32-bit zero mask
        vec_ullong2 z64 = (vec_ullong2)spu_and(
            spu_shuffle(z, z, lo32),
            spu_shuffle(z, z, hi32));
        return spu_nor(z64, z64);
    }
}

inline bool2::bool2(bool rhs)
{
    data = spu_splats((ullong)-(llong)rhs);
}

inline bool2::bool2(vec_ullong2 rhs)
{
    data = bool2_detail::mask_nonzero_u64(rhs);
}

inline bool2::bool2(llong2 rhs)
{
    data = bool2_detail::mask_nonzero_u64((vec_ullong2)(vec_llong2)rhs);
}

inline bool2::bool2(ullong2 rhs)
{
    data = bool2_detail::mask_nonzero_u64((vec_ullong2)rhs);
}

inline bool2::bool2(double2 rhs)
{
    *this = (rhs != double2(0.0));
}

inline bool2::bool2(bool r0, bool r1)
{
    data = (vec_ullong2){ (ullong)-(llong)r0, (ullong)-(llong)r1 };
}

inline bool2::operator vector unsigned long long() const { return data; }

inline bool2_idx bool2::operator [] (int i) { return bool2_idx(*this, i); }
inline bool      bool2::operator [] (int i) const
{
    return (bool)spu_extract(data, i);
}

inline const bool2 bool2::operator ! () const
{
    bool2 r;
    r.data = spu_nor(data, data);
    return r;
}

inline bool2& bool2::operator =  (bool2 rhs) { data = rhs.data; return *this; }
inline bool2& bool2::operator &= (bool2 rhs) { *this = *this & rhs; return *this; }
inline bool2& bool2::operator ^= (bool2 rhs) { *this = *this ^ rhs; return *this; }
inline bool2& bool2::operator |= (bool2 rhs) { *this = *this | rhs; return *this; }

inline const bool2 operator == (bool2 lhs, bool2 rhs)
{
    return ullong2(lhs) == ullong2(rhs);
}

inline const bool2 operator != (bool2 lhs, bool2 rhs) { return !(lhs == rhs); }

inline const bool2 select(bool2 lhs, bool2 rhs, bool2 rhs_slots)
{
    return bool2(spu_sel((vec_ullong2)lhs, (vec_ullong2)rhs, (vec_ullong2)rhs_slots));
}

inline const bool2 operator & (bool2 lhs, bool2 rhs)
{
    return bool2(spu_and((vec_ullong2)lhs, (vec_ullong2)rhs));
}

inline const bool2 operator | (bool2 lhs, bool2 rhs)
{
    return bool2(spu_or((vec_ullong2)lhs, (vec_ullong2)rhs));
}

inline const bool2 operator ^ (bool2 lhs, bool2 rhs)
{
    return bool2(spu_xor((vec_ullong2)lhs, (vec_ullong2)rhs));
}

inline uint gather(bool2 rhs)
{
    // Two lanes -> low 2 bits.  Bit 1 is lane 0, bit 0 is lane 1.
    vec_ullong2 d = (vec_ullong2)rhs;
    uint l0 = (uint)spu_extract(d, 0) & 1u;
    uint l1 = (uint)spu_extract(d, 1) & 1u;
    return (l0 << 1) | l1;
}

inline bool any(bool2 rhs) { return gather(rhs) != 0; }
inline bool all(bool2 rhs) { return gather(rhs) == 0x3u; }

inline bool2_idx::operator bool() const
{
    return (bool)spu_extract((vec_ullong2)ref, i);
}

inline bool bool2_idx::operator = (bool rhs)
{
    ref.data = spu_insert((ullong)-(llong)rhs, (vec_ullong2)ref, i);
    return rhs;
}

inline bool bool2_idx::operator = (const bool2_idx& rhs)
{
    return *this = (bool)rhs;
}

inline bool bool2_idx::operator &= (bool rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) & (ullong)-(llong)rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return (bool)t;
}

inline bool bool2_idx::operator ^= (bool rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) ^ (ullong)-(llong)rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return (bool)t;
}

inline bool bool2_idx::operator |= (bool rhs)
{
    ullong t = spu_extract((vec_ullong2)ref, i) | (ullong)-(llong)rhs;
    ref = spu_insert(t, (vec_ullong2)ref, i);
    return (bool)t;
}

}  // namespace simd

#endif
