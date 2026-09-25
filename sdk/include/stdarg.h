/* Forward to GCC's <stdarg.h> on EVERY inclusion, outside any include
   guard.  GCC's header is designed to be entered more than once: newlib's
   <wchar.h> does `#define __need___va_list` then `#include <stdarg.h>`, a
   partial mode that provides only __gnuc_va_list and leaves the header open;
   a later plain #include <stdarg.h> (or <cstdarg>) must reach it again to
   get va_list and va_start.  Guarding the forward made that second
   inclusion a no-op.

   NB: no duplicate-shadow detector here (unlike the other libc wrappers):
   in the partial mode GCC's header does not set _STDARG_H, so any "real
   header reached" sentinel would false-positive. */
#include_next <stdarg.h>

#ifndef PS3DK_STDARG_H
#define PS3DK_STDARG_H

/* The only addition, std::va_list, is __builtin_va_list-based and so
   independent of which mode the forward above ran in. */
#ifdef __cplusplus
namespace std {
typedef __builtin_va_list va_list;
}
#endif

#endif /* PS3DK_STDARG_H */
