/*
 * Verifies that <cell/gcm.h>, <cell/gcm/gcm_command_c.h>, <cell/gcm/gcm_enum.h>
 * and <rsx/gcm_sys.h> can be included in any order without triggering
 * macro redefinition warnings or errors under -Wall -Wextra -Werror in both C
 * and C++, for both ILP32 and LP64 ABIs.
 *
 * Also verifies via compile-time assertions that CELL_GCM_DEBUG_LEVEL0..2 and
 * CELL_GCM_ZCULL_Z16 / CELL_GCM_ZCULL_Z24S8 retain their canonical values.
 * The canonical umbrella-header marker must appear only after cell/gcm.h.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(ORDER_RSX_FIRST)
/* RSX headers first, followed by Cell GCM headers */
#  include <rsx/gcm_sys.h>
#  include <rsx/rsx.h>
#  ifdef __CELL_GCM_H__
#    error "RSX headers must not advertise the Cell GCM umbrella"
#  endif
#  include <cell/gcm.h>
#  include <cell/gcm/gcm_command_c.h>
#elif defined(ORDER_ENUM_FIRST)
/* GCM enum first, followed by Cell GCM and RSX headers */
#  include <cell/gcm/gcm_enum.h>
#  ifdef __CELL_GCM_H__
#    error "GCM enum subheader must not advertise the Cell GCM umbrella"
#  endif
#  include <cell/gcm.h>
#  include <cell/gcm/gcm_command_c.h>
#  include <rsx/gcm_sys.h>
#  include <rsx/rsx.h>
#elif defined(ORDER_ENUM_RSX)
/* GCM enum and RSX headers first, followed by Cell GCM */
#  include <cell/gcm/gcm_enum.h>
#  include <rsx/gcm_sys.h>
#  include <rsx/rsx.h>
#  ifdef __CELL_GCM_H__
#    error "GCM enum and RSX headers must not advertise the Cell GCM umbrella"
#  endif
#  include <cell/gcm.h>
#  include <cell/gcm/gcm_command_c.h>
#else
/* Default: ORDER_CELL_FIRST - Cell GCM headers first, then RSX headers */
#  include <cell/gcm.h>
#  include <cell/gcm/gcm_command_c.h>
#  include <rsx/gcm_sys.h>
#  include <rsx/rsx.h>
#endif

#ifndef __CELL_GCM_H__
#  error "cell/gcm.h must expose its canonical presence marker"
#endif
#include <cell/gcm.h> /* A second inclusion must retain the marker and types. */

/* An independent consumer selects its texture member through the marker,
 * as clients do when supporting both the SDK and standalone asset tools. */
struct GcmMarkerConsumer {
#ifdef __CELL_GCM_H__
    CellGcmTexture image;
#else
    unsigned char image;
#endif
};

CellGcmTexture *gcm_marker_texture(struct GcmMarkerConsumer *consumer)
{
    return &consumer->image;
}

/* Compile-time assertion helper compatible across C99, C11, and C++17 */
#define GCM_GLUE2(a, b) a##b
#define GCM_GLUE(a, b) GCM_GLUE2(a, b)

#if defined(__cplusplus)
#  if __cplusplus >= 201103L
#    define GCM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  else
#    define GCM_STATIC_ASSERT(cond, msg) typedef char GCM_GLUE(gcm_static_assert_, __LINE__)[(cond) ? 1 : -1]
#  endif
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define GCM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#  define GCM_STATIC_ASSERT(cond, msg) typedef char GCM_GLUE(gcm_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

/* Verify values are unchanged and match canonical reference specification */
GCM_STATIC_ASSERT(CELL_GCM_DEBUG_LEVEL0 == 0, "debug_level0_val");
GCM_STATIC_ASSERT(CELL_GCM_DEBUG_LEVEL1 == 1, "debug_level1_val");
GCM_STATIC_ASSERT(CELL_GCM_DEBUG_LEVEL2 == 2, "debug_level2_val");

GCM_STATIC_ASSERT(CELL_GCM_ZCULL_Z16 == 1, "zcull_z16_val");
GCM_STATIC_ASSERT(CELL_GCM_ZCULL_Z24S8 == 2, "zcull_z24s8_val");

GCM_STATIC_ASSERT(GCM_ZCULL_Z16 == 1, "gcm_zcull_z16_val");
GCM_STATIC_ASSERT(GCM_ZCULL_Z24S8 == 2, "gcm_zcull_z24s8_val");

int main(void)
{
    volatile int d0 = CELL_GCM_DEBUG_LEVEL0;
    volatile int d1 = CELL_GCM_DEBUG_LEVEL1;
    volatile int d2 = CELL_GCM_DEBUG_LEVEL2;
    volatile int z16 = CELL_GCM_ZCULL_Z16;
    volatile int z24s8 = CELL_GCM_ZCULL_Z24S8;

    (void)d0;
    (void)d1;
    (void)d2;
    (void)z16;
    (void)z24s8;

    return 0;
}
