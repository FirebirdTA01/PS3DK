/*
 * PS3 Custom Toolchain — <ppu-lv2.h> compatibility wrapper.
 *
 * Provides the legacy PSL1GHT <ppu-lv2.h> interface by delegating the
 * lv2syscall0..8 and register_passing_1..7 macro families to the SDK's
 * standards-conformant <sys/lv2_syscall.h>, preventing macro redefinition
 * clashes when both SDK and PSL1GHT headers are included in the same TU.
 * Installed into $(PS3DK)/ppu/include by sdk/Makefile install-headers,
 * overwriting any legacy PSL1GHT ppu-lv2.h during SDK installation.
 */

#ifndef __PPU_LV2_H__
#define __PPU_LV2_H__

#include <sys/lv2_syscall.h>

#define REG_PASS_SYS_EVENT_QUEUE_RECEIVE \
    event->source = register_passing_1(u64); \
    event->data_1 = register_passing_2(u64); \
    event->data_2 = register_passing_3(u64); \
    event->data_3 = register_passing_4(u64)

#endif /* __PPU_LV2_H__ */
