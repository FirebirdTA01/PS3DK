/*! \file sys/cdefs.h
 \brief Cell OS source-compat wrapper around newlib's <sys/cdefs.h>.

  Newlib ships its own <sys/cdefs.h> with __BEGIN_DECLS / __END_DECLS
  etc.  The platform ABI shipped a different <sys/cdefs.h> that
  *also* defined CDECL_BEGIN / CDECL_END / NAMESPACE_LV2_BEGIN /
  NAMESPACE_LV2_END.  Existing samples reach those macros through
  <sys/cdefs.h>; rather than patch every sample, we shadow newlib's
  header with this wrapper that pulls in newlib's content via
  #include_next then adds the system-only macros on top.

  This header is installed to $PS3DK/ppu/include/sys/cdefs.h, which
  appears in the include search path (via -I$PS3DK/ppu/include) before
  newlib's $PS3DEV/.../include/sys/cdefs.h, so transitive includes
  resolve here first.

  The forward runs on every inclusion, outside the include guard (as
  stdarg.h does): every copy of this wrapper on the include path then
  chains on, and newlib's header is always reached.  With the forward
  inside the guard, a second copy (the SDK's own build sees the source and
  the installed one) was skipped whole and newlib's <sys/cdefs.h> never read.
*/

#include_next <sys/cdefs.h>

#ifndef PS3TC_SYS_CDEFS_H
#define PS3TC_SYS_CDEFS_H

/* Defense-in-depth: the forward above must have reached newlib's <sys/cdefs.h>
   (_ATTRIBUTE, __BEGIN_DECLS et al.).  Gated on the PS3 PPU target macro
   (GCC-only build); excluded under __clang__ (clangd cannot model the -isystem
   #include_next ordering). Inert under host/IDE. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_SYS_CDEFS_H_)
# error "PS3DK <sys/cdefs.h> wrapper: real <sys/cdefs.h> not reached; check the include path for newlib's <sys/cdefs.h>."
#endif

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
