/* stdlib.h wrapper (SPU) -- chains to newlib SPU sysroot via
 * #include_next, then adds the memalign declaration that
 * binary-compatible SPU samples expect in <stdlib.h>.
 * newlib provides the implementation (malloc.h) but not the
 * stdlib.h declaration.
 */
#ifndef _PS3DK_SPU_STDLIB_WRAPPER_H
#define _PS3DK_SPU_STDLIB_WRAPPER_H

#include_next <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memalign(size_t, size_t);

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_SPU_STDLIB_WRAPPER_H */
