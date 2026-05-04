/* sys/crashdump.h — crash dump user log area registration.
 *
 * Clean-room header.  Provides sys_crash_dump_set_user_log_area and
 * sys_crash_dump_get_user_log_area — the 2-entry crashdump surface
 * for registering user-defined memory regions to include in system
 * crash dump files.
 *
 * The SPRX reads/writes `sys_crash_dump_log_area_info_t` with a
 * 32-bit-pointer ABI, so `addr` and `size` are declared as uint32_t
 * rather than the LP64-native width of sys_addr_t / size_t.
 *
 * This library is 475+ only; pre-475 firmware does not ship it.
 */
#ifndef __PS3DK_SYS_CRASHDUMP_H__
#define __PS3DK_SYS_CRASHDUMP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants -------------------------------------------------- */

#define SYS_CRASH_DUMP_MAX_LABEL_SIZE  16

/* ---- types ------------------------------------------------------ */

typedef struct {
    char     label[SYS_CRASH_DUMP_MAX_LABEL_SIZE];
    uint32_t addr;    /* sys_addr_t — 4-byte EA in SPRX ABI */
    uint32_t size;    /* size_t — 4 bytes in SPRX ABI */
} sys_crash_dump_log_area_info_t;

/* ---- API (2 entry points) --------------------------------------- */

int sys_crash_dump_set_user_log_area(
    uint8_t                          index,
    sys_crash_dump_log_area_info_t  *new_entry
);

int sys_crash_dump_get_user_log_area(
    uint8_t                          index,
    sys_crash_dump_log_area_info_t  *entry
);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_SYS_CRASHDUMP_H__ */
