#ifndef PS3DK_STDARG_H
#define PS3DK_STDARG_H

#include_next <stdarg.h>

#ifdef __cplusplus
namespace std {
typedef __builtin_va_list va_list;
}
#endif

#endif /* PS3DK_STDARG_H */
