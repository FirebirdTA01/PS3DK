/*
 * PS3 Custom Toolchain — <sys/types.h> wrapper.
 *
 * Provides POSIX types from newlib's <sys/types.h> (off_t, pid_t,
 * useconds_t, etc.), then adds Sony-SDK compat typedefs (usecond_t,
 * second_t) from <sys/sys_types.h>.  Every translation unit that
 * includes <sys/types.h> gets the Sony types automatically,
 * matching the reference-SDK expectation that `usecond_t` is a
 * system-wide type.
 *
 * The forward runs on every inclusion, outside the include guard (as
 * stdarg.h does): every copy of this wrapper on the include path then
 * chains on, and libc's header is always reached.  With the forward inside
 * the guard, a second copy (the SDK's own build sees the source and the
 * installed one) was skipped whole and libc's <sys/types.h> never read.
 */

#include_next <sys/types.h>

#ifndef _PS3DK_SYS_TYPES_H_WRAPPER
#define _PS3DK_SYS_TYPES_H_WRAPPER

/* Defense-in-depth: the forward above must have reached libc's <sys/types.h>.
   Gated on the PS3 PPU target macro (GCC-only build); excluded under __clang__
   (clangd cannot model the -isystem #include_next ordering). Inert under
   host/IDE. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_SYS_TYPES_H)
# error "PS3DK <sys/types.h> wrapper: real <sys/types.h> not reached; check the include path for a libc <sys/types.h>."
#endif

#include <sys/sys_types.h>

#endif  /* _PS3DK_SYS_TYPES_H_WRAPPER */
