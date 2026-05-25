#ifndef PS3DK_STDIO_H
#define PS3DK_STDIO_H

#include <stdarg.h>
#include_next <stdio.h>

#ifdef __cplusplus
namespace std {
using ::vfprintf;
using ::vsnprintf;
using ::vsprintf;
}
#endif

#endif /* PS3DK_STDIO_H */
