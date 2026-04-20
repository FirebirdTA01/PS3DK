/*! \file sys/stat.h
 \brief PSL1GHT override of newlib's <sys/stat.h>.

  PSL1GHT-owned shadow.  Pulls newlib's declarations via #include_next,
  then undefines the POSIX-2008-compat macros that otherwise rewrite
  `st_atime` / `st_mtime` / `st_ctime` into `st_atim.tv_sec` etc.
  wherever those tokens appear in subsequent source — including in
  unrelated structs (PSL1GHT's sysFSStat, Sony's CellFsStat, any
  user-defined struct that happens to use the same member name).

  Rationale:
    - Sony's reference SDK (475.001) never defined these macros.
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
      Sony SDK convention and POSIX.1-2017 guidance.
*/
#ifndef _PSL1GHT_SYS_STAT_H
#define _PSL1GHT_SYS_STAT_H

#include_next <sys/stat.h>

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
