/* stdlib.h wrapper -- chains to newlib via #include_next, then adds the
 * memalign declaration that binary-compatible samples expect in <stdlib.h>.
 * newlib provides the implementation (malloc.h) but not the stdlib.h
 * declaration.
 *
 * The forward runs on every inclusion, outside the include guard (as
 * stdarg.h does): every copy of this wrapper on the include path then
 * chains on, and libc's header is always reached.  With the forward inside
 * the guard, a second copy (the SDK's own build sees the source and the
 * installed one) was skipped whole and libc's <stdlib.h> never read.
 */
#include_next <stdlib.h>

#ifndef _PS3DK_STDLIB_WRAPPER_H
#define _PS3DK_STDLIB_WRAPPER_H

/* Defense-in-depth: the forward above must have reached libc's <stdlib.h>.
   Gated on the PS3 PPU target macro so the check runs only under our (GCC)
   toolchain, where _STDLIB_H_ is newlib's guard; excluded under __clang__ because
   clang-based tooling (clangd) cannot model the -isystem #include_next ordering
   and would false-positive. Inert under host/IDE analysis. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_STDLIB_H_)
# error "PS3DK <stdlib.h> wrapper: real <stdlib.h> not reached; check the include path for a libc <stdlib.h>."
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *memalign(size_t, size_t);

#ifdef __cplusplus
}

/* Every <stdlib.h> function is also a std:: name, memalign included, so
 * std::memalign works after <cstdlib>. */
namespace std { using ::memalign; }
#endif

#endif /* _PS3DK_STDLIB_WRAPPER_H */
