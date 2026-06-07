#ifndef PS3DK_STDARG_H
#define PS3DK_STDARG_H

#include_next <stdarg.h>

/* NB: no duplicate-shadow detector here (unlike the other libc wrappers).
   <stdarg.h> is intentionally re-includable: newlib's <wchar.h> does
   `#define __need___va_list` then `#include <stdarg.h>`, in which mode GCC's
   <stdarg.h> provides only __gnuc_va_list and does NOT set _STDARG_H, so any
   "real header reached" sentinel would false-positive. This wrapper also needs
   no such guard: its only addition (std::va_list) is __builtin_va_list-based,
   independent of whether newlib was reached. */

#ifdef __cplusplus
namespace std {
typedef __builtin_va_list va_list;
}
#endif

#endif /* PS3DK_STDARG_H */
