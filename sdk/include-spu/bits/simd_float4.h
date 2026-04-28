// float4 - 4-lane float SIMD type, backed by vec_float4.

#ifndef PS3TC_SPU_SIMD_FLOAT4_H
#define PS3TC_SPU_SIMD_FLOAT4_H

#include <spu_intrinsics.h>
#include <simdmath/simdmath.h>     // divf4
#include "simd_scalars.h"

namespace simd {

class int4;
class uint4;
class bool4;
class float4_idx;

class float4
{
private:
    vec_float4 data;

public:
    inline float4() {}
    inline float4(int4);
    inline float4(uint4);
    inline float4(bool4);
    inline float4(float r0, float r1, float r2, float r3);

    explicit inline float4(float);

    inline float4(vec_float4 rhs);
    inline operator vector float() const;

    inline float4_idx operator [] (int i);
    inline float       operator [] (int i) const;

    inline const float4 operator ++ (int);
    inline const float4 operator -- (int);
    inline float4& operator ++ ();
    inline float4& operator -- ();

    inline const float4 operator - () const;

    inline float4& operator =  (float4 rhs);
    inline float4& operator *= (float4 rhs);
    inline float4& operator /= (float4 rhs);
    inline float4& operator += (float4 rhs);
    inline float4& operator -= (float4 rhs);
};

inline const float4 operator * (float4 lhs, float4 rhs);
inline const float4 operator / (float4 lhs, float4 rhs);
inline const float4 operator + (float4 lhs, float4 rhs);
inline const float4 operator - (float4 lhs, float4 rhs);

inline const bool4 operator < (float4 lhs, float4 rhs);
inline const bool4 operator <= (float4 lhs, float4 rhs);
inline const bool4 operator > (float4 lhs, float4 rhs);
inline const bool4 operator >= (float4 lhs, float4 rhs);
inline const bool4 operator == (float4 lhs, float4 rhs);
inline const bool4 operator != (float4 lhs, float4 rhs);

inline const float4 select(float4 lhs, float4 rhs, bool4 rhs_slots);

class float4_idx
{
private:
    float4 &ref __attribute__((aligned(16)));
    int     i   __attribute__((aligned(16)));
public:
    inline float4_idx(float4& vec, int idx) : ref(vec) { i = idx; }
    inline operator float() const;
    inline float operator = (float rhs);
    inline float operator = (const float4_idx& rhs);
    inline float operator ++ (int);
    inline float operator -- (int);
    inline float operator ++ ();
    inline float operator -- ();
    inline float operator *= (float rhs);
    inline float operator /= (float rhs);
    inline float operator += (float rhs);
    inline float operator -= (float rhs);
};

}  // namespace simd

#include "simd_int4.h"
#include "simd_uint4.h"
#include "simd_bool4.h"

namespace simd {

inline float4::float4(float rhs)        { data = spu_splats((float)rhs); }
inline float4::float4(vec_float4 rhs)   { data = rhs; }

inline float4::float4(int4 rhs)
{
    data = spu_convtf((vec_int4)rhs, 0);
}

inline float4::float4(uint4 rhs)
{
    data = spu_convtf((vec_uint4)rhs, 0);
}

inline float4::float4(bool4 rhs)
{
    *this = float4(int4(rhs));
}

inline float4::float4(float r0, float r1, float r2, float r3)
{
    data = (vec_float4){ r0, r1, r2, r3 };
}

inline float4::operator vector float() const { return data; }

inline float4_idx float4::operator [] (int i) { return float4_idx(*this, i); }
inline float      float4::operator [] (int i) const
{
    return spu_extract(data, i);
}

inline const float4 float4::operator ++ (int) { vec_float4 p = data; operator ++(); return float4(p); }
inline const float4 float4::operator -- (int) { vec_float4 p = data; operator --(); return float4(p); }
inline float4& float4::operator ++ () { *this += float4(1.0f); return *this; }
inline float4& float4::operator -- () { *this -= float4(1.0f); return *this; }

inline const float4 float4::operator - () const
{
    // Flip sign bit on every lane.
    return float4((vec_float4)spu_xor((vec_uint4)data,
                                      spu_splats((uint)0x80000000u)));
}

inline float4& float4::operator =  (float4 rhs) { data = rhs.data; return *this; }
inline float4& float4::operator *= (float4 rhs) { *this = *this * rhs; return *this; }
inline float4& float4::operator /= (float4 rhs) { *this = *this / rhs; return *this; }
inline float4& float4::operator += (float4 rhs) { *this = *this + rhs; return *this; }
inline float4& float4::operator -= (float4 rhs) { *this = *this - rhs; return *this; }

inline const float4 operator * (float4 lhs, float4 rhs)
{
    return float4(spu_mul((vec_float4)lhs, (vec_float4)rhs));
}

inline const float4 operator / (float4 num, float4 den)
{
    return float4(divf4((vec_float4)num, (vec_float4)den));
}

inline const float4 operator + (float4 lhs, float4 rhs)
{
    return float4(spu_add((vec_float4)lhs, (vec_float4)rhs));
}

inline const float4 operator - (float4 lhs, float4 rhs)
{
    return float4(spu_sub((vec_float4)lhs, (vec_float4)rhs));
}

inline const bool4 operator < (float4 lhs, float4 rhs)
{
    bool4 r;
    r.data = spu_cmpgt((vec_float4)rhs, (vec_float4)lhs);
    return r;
}

inline const bool4 operator <= (float4 lhs, float4 rhs) { return !(lhs > rhs); }

inline const bool4 operator > (float4 lhs, float4 rhs)
{
    bool4 r;
    r.data = spu_cmpgt((vec_float4)lhs, (vec_float4)rhs);
    return r;
}

inline const bool4 operator >= (float4 lhs, float4 rhs) { return !(lhs < rhs); }

inline const bool4 operator == (float4 lhs, float4 rhs)
{
    bool4 r;
    r.data = spu_cmpeq((vec_float4)lhs, (vec_float4)rhs);
    return r;
}

inline const bool4 operator != (float4 lhs, float4 rhs) { return !(lhs == rhs); }

inline const float4 select(float4 lhs, float4 rhs, bool4 rhs_slots)
{
    return float4(spu_sel((vec_float4)lhs, (vec_float4)rhs, (vec_uint4)rhs_slots));
}

inline float4_idx::operator float() const
{
    return spu_extract((vec_float4)ref, i);
}

inline float float4_idx::operator = (float rhs)
{
    ref = spu_insert(rhs, (vec_float4)ref, i);
    return rhs;
}

inline float float4_idx::operator = (const float4_idx& rhs)
{
    return *this = (float)rhs;
}

inline float float4_idx::operator ++ (int)
{
    float t = spu_extract((vec_float4)ref, i);
    ref = spu_insert(t + 1.0f, (vec_float4)ref, i);
    return t;
}

inline float float4_idx::operator -- (int)
{
    float t = spu_extract((vec_float4)ref, i);
    ref = spu_insert(t - 1.0f, (vec_float4)ref, i);
    return t;
}

inline float float4_idx::operator ++ ()
{
    float t = spu_extract((vec_float4)ref, i) + 1.0f;
    ref = spu_insert(t, (vec_float4)ref, i);
    return t;
}

inline float float4_idx::operator -- ()
{
    float t = spu_extract((vec_float4)ref, i) - 1.0f;
    ref = spu_insert(t, (vec_float4)ref, i);
    return t;
}

inline float float4_idx::operator *= (float rhs)
{
    float t = spu_extract((vec_float4)ref, i) * rhs;
    ref = spu_insert(t, (vec_float4)ref, i);
    return t;
}

inline float float4_idx::operator /= (float rhs)
{
    float t = spu_extract((vec_float4)ref, i) / rhs;
    ref = spu_insert(t, (vec_float4)ref, i);
    return t;
}

inline float float4_idx::operator += (float rhs)
{
    float t = spu_extract((vec_float4)ref, i) + rhs;
    ref = spu_insert(t, (vec_float4)ref, i);
    return t;
}

inline float float4_idx::operator -= (float rhs)
{
    float t = spu_extract((vec_float4)ref, i) - rhs;
    ref = spu_insert(t, (vec_float4)ref, i);
    return t;
}

}  // namespace simd

#endif
