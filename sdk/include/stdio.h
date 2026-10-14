/* Forward to the next <stdio.h> on every inclusion, outside the include
   guard (as stdarg.h does): every copy of this wrapper on the include path
   then chains on, and libc's header is always reached.  With the forward
   inside the guard, a second copy (the SDK's own build sees the source and
   the installed one) was skipped whole and libc's <stdio.h> never read. */
#include <stdarg.h>
#include_next <stdio.h>

#ifndef PS3DK_STDIO_H
#define PS3DK_STDIO_H

/* Defense-in-depth: the forward above must have reached libc's <stdio.h>.
   Gated on the PS3 PPU target macro so the check runs only under our (GCC)
   toolchain, where _STDIO_H_ is newlib's guard; excluded under __clang__ because
   clang-based tooling (clangd) cannot model the -isystem #include_next ordering
   and would false-positive. Inert under host/IDE analysis. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_STDIO_H_)
# error "PS3DK <stdio.h> wrapper: real <stdio.h> not reached; check the include path for a libc <stdio.h>."
#endif

#ifdef __cplusplus
namespace std {
using ::vfprintf;
using ::vsnprintf;
using ::vsprintf;
}
#endif

#endif /* PS3DK_STDIO_H */
