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
#include <sys/sys_types.h>

#endif  /* _PS3DK_SYS_TYPES_H_WRAPPER */
