/*
 * PS3 Custom Toolchain — <sys/sys_types.h> Sony-SDK compat typedefs.
 *
 * The reference SDK toolchain (SNC) provides `usecond_t` and
 * `second_t` as compiler built-ins (64-bit unsigned).  Our GCC-based
 * PS3DK defines these here so reference-SDK samples that use them
 * compile without modification.
 */

#ifndef _PS3DK_SYS_SYS_TYPES_H
#define _PS3DK_SYS_SYS_TYPES_H

#include <stdint.h>

typedef uint64_t usecond_t;
typedef uint64_t second_t;
typedef uint32_t sys_event_type_t;

#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef uint32_t sys_memory_container_t;
#endif

#endif  /* _PS3DK_SYS_SYS_TYPES_H */
