#ifndef PS3DK_STDIO_H
#define PS3DK_STDIO_H

#include <stdarg.h>
#include_next <stdio.h>

/* Defense-in-depth: if a duplicate copy of this wrapper sits earlier on the
   include path, the shared guard above makes this #include_next resolve to the
   (skipped) duplicate instead of the real <stdio.h>, silently dropping libc.
   Gated on the PS3 PPU target macro so the check runs only under our (GCC)
   toolchain, where _STDIO_H_ is newlib's guard; excluded under __clang__ because
   clang-based tooling (clangd) cannot model the -isystem #include_next ordering
   and would false-positive. Inert under host/IDE analysis. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(__PS3DK_SDK_SELFBUILD__) && !defined(_STDIO_H_)
# error "PS3DK <stdio.h> wrapper: real <stdio.h> not reached (a duplicate wrapper copy shadowed it); keep only one copy of the PS3DK wrapper headers on the include path."
#endif

#ifdef __cplusplus
namespace std {
using ::vfprintf;
using ::vsnprintf;
using ::vsprintf;
}
#endif

#endif /* PS3DK_STDIO_H */
