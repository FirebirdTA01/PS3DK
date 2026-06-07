/* stdlib.h wrapper -- chains to newlib via #include_next, then adds the
 * memalign declaration that binary-compatible samples expect in <stdlib.h>.
 * newlib provides the implementation (malloc.h) but not the stdlib.h
 * declaration.
 */
#ifndef _PS3DK_STDLIB_WRAPPER_H
#define _PS3DK_STDLIB_WRAPPER_H

#include_next <stdlib.h>

/* Defense-in-depth: if a duplicate copy of this wrapper sits earlier on the
   include path, the shared guard above makes this #include_next resolve to the
   (skipped) duplicate instead of the real <stdlib.h>, silently dropping libc.
   Gated on the PS3 PPU target macro so the check runs only under our (GCC)
   toolchain, where _STDLIB_H_ is newlib's guard; excluded under __clang__ because
   clang-based tooling (clangd) cannot model the -isystem #include_next ordering
   and would false-positive. Inert under host/IDE analysis. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_STDLIB_H_)
# error "PS3DK <stdlib.h> wrapper: real <stdlib.h> not reached (a duplicate wrapper copy shadowed it); keep only one copy of the PS3DK wrapper headers on the include path."
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *memalign(size_t, size_t);

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_STDLIB_WRAPPER_H */
