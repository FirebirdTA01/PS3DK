#ifndef NVFX_TYPES_H
#define NVFX_TYPES_H

/*
 * Minimal primitive-type header used by the NV30/NV40 register files.
 * Replaces PSL1GHT cgcomp's include/types.h (which dragged in Windows
 * helpers and macros that are irrelevant here).  Only the u8/u16/u32/
 * s8/s16/s32/u64/s64/f32/boolean typedefs that nvfx_shader.h consumes.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Fragment-program instruction precision (used by nvfx_shader.h). */
#define FLOAT32  0x0
#define FLOAT16  0x1
#define FIXED12  0x2

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef INLINE
#define INLINE inline
#endif
#ifndef boolean
#define boolean char
#endif

#endif  /* NVFX_TYPES_H */
