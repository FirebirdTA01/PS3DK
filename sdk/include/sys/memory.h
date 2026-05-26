/*
 * PS3 Custom Toolchain - <sys/memory.h>
 *
 * Process-level memory-info accessor over LV2 syscalls:
 *   sys_memory_get_user_memory_size  (352)  - total / available user memory
 *   sys_memory_container_create      (324)  - allocate a memory container
 *   sys_memory_container_destroy     (325)  - release a memory container
 *
 * Fields use uint32_t explicitly to keep the kernel ABI stable under
 * both ILP32 (default) and LP64 (-mlp64 multilib) PPU userland.  The
 * LV2 kernel side writes 32-bit values regardless of the caller's
 * data model.
 */

#ifndef PS3TC_SYS_MEMORY_H
#define PS3TC_SYS_MEMORY_H

#include <ppu-types.h>
#include <sys/lv2_syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_MEMORY_CONTAINER_ID_INVALID  0xFFFFFFFFU

typedef uint32_t sys_memory_container_t;

typedef struct sys_memory_info {
	uint32_t total_user_memory;
	uint32_t available_user_memory;
} sys_memory_info_t;

LV2_SYSCALL sys_memory_get_user_memory_size(sys_memory_info_t *mem_info)
{
	lv2syscall1(352, (u64)(uintptr_t)mem_info);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_memory_container_create(sys_memory_container_t *cid,
                                        uint32_t yield_size)
{
	lv2syscall2(324, (u64)(uintptr_t)cid, (u64)yield_size);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_memory_container_destroy(sys_memory_container_t cid)
{
	lv2syscall1(325, (u64)cid);
	return_to_user_prog(s32);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_MEMORY_H */
