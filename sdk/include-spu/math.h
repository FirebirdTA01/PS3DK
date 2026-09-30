/* math.h wrapper -- chains to newlib via #include_next.  Reference-SDK code
 * uses M_PI in strict language modes, where newlib hides it; the SDK math.h
 * always defines it, as a single-precision constant.  A definition newlib
 * already made (double) is kept. */
#ifndef _PS3DK_MATH_WRAPPER_H
#define _PS3DK_MATH_WRAPPER_H

#include_next <math.h>

#ifndef M_PI
#define M_PI 3.141592653589793f
#endif

#endif /* _PS3DK_MATH_WRAPPER_H */
