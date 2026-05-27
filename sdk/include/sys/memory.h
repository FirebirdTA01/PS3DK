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

#define SYS_MEMORY_PAGE_SIZE_1M             0x0000000000000400ULL
#define SYS_MEMORY_PAGE_SIZE_64K            0x0000000000000200ULL
#define SYS_MEMORY_ACCESS_RIGHT_PPU_THR     0x0000000000000008ULL
#define SYS_MEMORY_ACCESS_RIGHT_HANDLER      0x0000000000000004ULL
#define SYS_MEMORY_ACCESS_RIGHT_SPU_THR     0x0000000000000002ULL
#define SYS_MEMORY_ACCESS_RIGHT_RAW_SPU     0x0000000000000001ULL
#define SYS_MEMORY_ACCESS_RIGHT_ANY \
	(SYS_MEMORY_ACCESS_RIGHT_PPU_THR | SYS_MEMORY_ACCESS_RIGHT_HANDLER | \
	 SYS_MEMORY_ACCESS_RIGHT_SPU_THR | SYS_MEMORY_ACCESS_RIGHT_RAW_SPU)

typedef uint32_t sys_memory_container_t;
typedef uint32_t sys_memory_t;

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

LV2_SYSCALL sysMemContainerCreate(sys_memory_container_t *cid,
                                  uint32_t yield_size)
{
	return sys_memory_container_create(cid, yield_size);
}

LV2_SYSCALL sysMemContainerDestroy(sys_memory_container_t cid)
{
	return sys_memory_container_destroy(cid);
}

LV2_SYSCALL sys_mmapper_allocate_fixed_address(void)
{
	lv2syscall0(326);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mmapper_enable_page_fault_notification(
	sys_addr_t start_addr, sys_event_queue_t queue_id)
{
	lv2syscall2(327, (u64)start_addr, (u64)queue_id);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mmapper_allocate_address(
	uint32_t size, uint64_t flags, uint32_t alignment,
	sys_addr_t *alloc_addr)
{
	lv2syscall4(330, (u64)size, flags, (u64)alignment,
	            (u64)(uintptr_t)alloc_addr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mmapper_free_address(sys_addr_t start_addr)
{
	lv2syscall1(331, (u64)start_addr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mmapper_change_address_access_right(
	sys_addr_t start_addr, uint64_t flags)
{
	lv2syscall2(336, (u64)start_addr, flags);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mmapper_search_and_map(
	sys_addr_t start_addr, sys_memory_t mem_id,
	uint64_t flags, sys_addr_t *map_addr)
{
	lv2syscall4(337, (u64)start_addr, (u64)mem_id, flags,
	            (u64)(uintptr_t)map_addr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_memory_allocate(uint32_t size, uint64_t flags,
                                sys_addr_t *alloc_addr)
{
	lv2syscall3(348, (u64)size, flags,
	            (u64)(uintptr_t)alloc_addr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_memory_free(sys_addr_t start_addr)
{
	lv2syscall1(349, (u64)start_addr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_memory_allocate_from_container(
	uint32_t size, sys_memory_container_t cid,
	uint64_t flags, sys_addr_t *alloc_addr)
{
	lv2syscall4(350, (u64)size, (u64)cid, flags,
	            (u64)(uintptr_t)alloc_addr);
	return_to_user_prog(s32);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_MEMORY_H */
