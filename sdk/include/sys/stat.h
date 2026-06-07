/*! \file sys/stat.h
 \brief PSL1GHT override of newlib's <sys/stat.h>.

  PSL1GHT-owned shadow.  Pulls newlib's declarations via #include_next,
  then undefines the POSIX-2008-compat macros that otherwise rewrite
  `st_atime` / `st_mtime` / `st_ctime` into `st_atim.tv_sec` etc.
  wherever those tokens appear in subsequent source — including in
  unrelated structs (PSL1GHT's sysFSStat, Sony's CellFsStat, any
  user-defined struct that happens to use the same member name).

  Rationale:
    - The reference SDK never defined these macros.
    - Newlib 4.4's unconditional #defines made it impossible to
      declare `st_atime` as a struct member without pragma gymnastics
      (see the original patches/psl1ght/0003 push_macro/pop_macro dance).
    - Undefining once, early, is cleaner than pragma-guarding every
      site.

  Impact on user code:
    - Nanosecond-precision access via `sb.st_atim.tv_sec` / `.tv_nsec`
      still works; those are real struct members.
    - `sb.st_atime` as a shortcut for `sb.st_atim.tv_sec` NO LONGER
      compiles.  Migrate to the full timespec form.  This matches the
      the reference SDK convention and POSIX.1-2017 guidance.
*/
#ifndef _PSL1GHT_SYS_STAT_H
#define _PSL1GHT_SYS_STAT_H

#include_next <sys/stat.h>

/* Defense-in-depth: a duplicate copy of this wrapper earlier on the include path
   would make this #include_next resolve to the (skipped) duplicate instead of
   the real <sys/stat.h>, silently dropping libc. Gated on the PS3 PPU target
   macro (GCC-only build); excluded under __clang__ (clangd cannot model the
   -isystem #include_next ordering). Inert under host/IDE. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(_SYS_STAT_H)
# error "PS3DK <sys/stat.h> wrapper: real <sys/stat.h> not reached (a duplicate wrapper copy shadowed it); keep only one copy of the PS3DK wrapper headers on the include path."
#endif

#ifdef st_atime
#undef st_atime
#endif
#ifdef st_mtime
#undef st_mtime
#endif
#ifdef st_ctime
#undef st_ctime
#endif

#endif /* _PSL1GHT_SYS_STAT_H */
