/* Forward to the next <string.h> on every inclusion, outside the include
   guard (as stdarg.h does): every copy of this wrapper on the include path
   then chains on, and libc's header is always reached.  With the forward
   inside the guard, a second copy (the SDK's own build sees the source and
   the installed one) was skipped whole and libc's <string.h> never read. */
#include_next <string.h>

#ifndef PS3DK_STRING_H
#define PS3DK_STRING_H

/* Defense-in-depth: the forward above must have reached libc's <string.h>.
   Gated on the PS3 PPU target macro (GCC-only build); excluded under
   __clang__ (clangd cannot model the -isystem #include_next ordering and
   would false-positive). Inert under host/IDE. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_STRING_H_)
# error "PS3DK <string.h> wrapper: real <string.h> not reached; check the include path for a libc <string.h>."
#endif

#ifdef __cplusplus
namespace std {
using ::memset;
}
#endif

#endif /* PS3DK_STRING_H */
