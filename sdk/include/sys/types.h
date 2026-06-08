/*
 * PS3 Custom Toolchain — <sys/types.h> wrapper.
 *
 * Provides POSIX types from newlib's <sys/types.h> (off_t, pid_t,
 * useconds_t, etc.), then adds Sony-SDK compat typedefs (usecond_t,
 * second_t) from <sys/sys_types.h>.  Every translation unit that
 * includes <sys/types.h> gets the Sony types automatically,
 * matching the reference-SDK expectation that `usecond_t` is a
 * system-wide type.
 */

#ifndef _PS3DK_SYS_TYPES_H_WRAPPER
#define _PS3DK_SYS_TYPES_H_WRAPPER

#include_next <sys/types.h>

/* Defense-in-depth: a duplicate copy of this wrapper earlier on the include path
   would make this #include_next resolve to the (skipped) duplicate instead of
   the real <sys/types.h>, silently dropping libc. Gated on the PS3 PPU target
   macro (GCC-only build); excluded under __clang__ (clangd cannot model the
   -isystem #include_next ordering). Inert under host/IDE. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(__PS3DK_SDK_SELFBUILD__) && !defined(_SYS_TYPES_H)
# error "PS3DK <sys/types.h> wrapper: real <sys/types.h> not reached (a duplicate wrapper copy shadowed it); keep only one copy of the PS3DK wrapper headers on the include path."
#endif

#include <sys/sys_types.h>

#endif  /* _PS3DK_SYS_TYPES_H_WRAPPER */
