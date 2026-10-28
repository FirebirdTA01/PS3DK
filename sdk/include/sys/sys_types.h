/*
 * PS3 Custom Toolchain — <sys/sys_types.h> Cell OS compat typedefs.
 *
 * The platform ABI toolchain (SNC) provides `usecond_t` and
 * `second_t` as compiler built-ins (64-bit unsigned).  Our GCC-based
 * PS3DK defines these here so existing PS3 samples that use them
 * compile without modification.
 */

#ifndef _PS3DK_SYS_SYS_TYPES_H
#define _PS3DK_SYS_SYS_TYPES_H

#include <stdint.h>

typedef uint64_t usecond_t;
typedef uint64_t second_t;
/* a 64-bit file offset (newlib names it only _off64_t) */
typedef int64_t off64_t;

typedef struct sys_ppu_guid_info {
    char magic[5];
    uint8_t id_revision;
    uint16_t reserved1;
    uint32_t ppu_guid[5];
    uint32_t reserved2;
} sys_ppu_guid_info_t;

#endif  /* _PS3DK_SYS_SYS_TYPES_H */
