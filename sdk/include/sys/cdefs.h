/*! \file sys/cdefs.h
 \brief Reference-SDK source-compat wrapper around newlib's <sys/cdefs.h>.

  Newlib ships its own <sys/cdefs.h> with __BEGIN_DECLS / __END_DECLS
  etc.  The reference SDK shipped a different <sys/cdefs.h> that
  *also* defined CDECL_BEGIN / CDECL_END / NAMESPACE_LV2_BEGIN /
  NAMESPACE_LV2_END.  Reference samples reach those macros through
  <sys/cdefs.h>; rather than patch every sample, we shadow newlib's
  header with this wrapper that pulls in newlib's content via
  #include_next then adds the reference-only macros on top.

  This header is installed to $PS3DK/ppu/include/sys/cdefs.h, which
  appears in the include search path (via -I$PS3DK/ppu/include) before
  newlib's $PS3DEV/.../include/sys/cdefs.h, so transitive includes
  resolve here first.
*/

#ifndef PS3TC_SYS_CDEFS_H
#define PS3TC_SYS_CDEFS_H

#include_next <sys/cdefs.h>

#ifdef __cplusplus
# ifndef CDECL_BEGIN
#  define CDECL_BEGIN          extern "C" {
# endif
# ifndef CDECL_END
#  define CDECL_END            }
# endif
# ifndef NAMESPACE_LV2_BEGIN
#  define NAMESPACE_LV2_BEGIN  namespace lv2 {
# endif
# ifndef NAMESPACE_LV2_END
#  define NAMESPACE_LV2_END    }
# endif
#else
# ifndef CDECL_BEGIN
#  define CDECL_BEGIN
# endif
# ifndef CDECL_END
#  define CDECL_END
# endif
# ifndef NAMESPACE_LV2_BEGIN
#  define NAMESPACE_LV2_BEGIN
# endif
# ifndef NAMESPACE_LV2_END
#  define NAMESPACE_LV2_END
# endif
#endif

#endif  /* PS3TC_SYS_CDEFS_H */
