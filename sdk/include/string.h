#ifndef PS3DK_STRING_H
#define PS3DK_STRING_H

#include_next <string.h>

/* Defense-in-depth: a duplicate copy of this wrapper earlier on the include path
   would make this #include_next resolve to the (skipped) duplicate instead of
   the real <string.h>, silently dropping libc. Gated on the PS3 PPU target macro
   (GCC-only build); excluded under __clang__ (clangd cannot model the -isystem
   #include_next ordering and would false-positive). Inert under host/IDE. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_STRING_H_)
# error "PS3DK <string.h> wrapper: real <string.h> not reached (a duplicate wrapper copy shadowed it); keep only one copy of the PS3DK wrapper headers on the include path."
#endif

#ifdef __cplusplus
namespace std {
using ::memset;
}
#endif

#endif /* PS3DK_STRING_H */
