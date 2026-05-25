/*
 * PS3 Custom Toolchain — <sys/sys_types.h> Sony-SDK compat typedefs.
 *
 * The reference reference-SDK toolchain (SNC) provides `usecond_t` and
 * `second_t` as compiler built-ins (64-bit unsigned).  Our GCC-based
 * PS3DK defines these here so reference-SDK samples that use them
 * compile without modification.
 */

#ifndef _PS3DK_SYS_SYS_TYPES_H
#define _PS3DK_SYS_SYS_TYPES_H

#include <stdint.h>

typedef uint64_t usecond_t;
typedef uint64_t second_t;

#endif  /* _PS3DK_SYS_SYS_TYPES_H */
