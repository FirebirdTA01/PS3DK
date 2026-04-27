/*! \file sys/sce_cdefs.h
 \brief Reference-SDK source-compat macros that aren't part of newlib's
        <sys/cdefs.h>.

  The reference SDK's <sys/cdefs.h> (which our newlib-shipped one
  shadows) defined CDECL_BEGIN / CDECL_END and the LV2 namespace
  guards.  Reference-source samples reach them transitively through
  <sys/cdefs.h>; rather than patching our newlib copy, we ship the
  extras here and pull them in from the headers that need them.
*/

#ifndef PS3TC_SYS_SCE_CDEFS_H
#define PS3TC_SYS_SCE_CDEFS_H

#ifdef __cplusplus
# define CDECL_BEGIN          extern "C" {
# define CDECL_END            }
# define NAMESPACE_LV2_BEGIN  namespace lv2 {
# define NAMESPACE_LV2_END    }
#else
# define CDECL_BEGIN
# define CDECL_END
# define NAMESPACE_LV2_BEGIN
# define NAMESPACE_LV2_END
#endif

#endif  /* PS3TC_SYS_SCE_CDEFS_H */
