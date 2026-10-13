/*
 * PS3 Custom Toolchain - <sys/memory.h>
 *
 * Process-level memory-info accessor over LV2 syscalls:
 *   sys_memory_get_user_memory_size  (352)  - total / available user memory
 *   sys_memory_container_create      (324)  - allocate a memory container
 *   sys_memory_container_destroy     (325)  - release a memory container
 *   sys_memory_container_get_size    (343)  - a container's total / available
 *   sys_memory_get_page_attribute    (351)  - attributes of the page at an address
 *
 * Fields use uint32_t explicitly to keep the kernel ABI stable under
 * both ILP32 (default) and LP64 (-mlp64 multilib) PPU userland.  The
 * LV2 kernel side writes 32-bit values regardless of the caller's
 * data model.
 */

#ifndef PS3TC_SYS_MEMORY_H
#define PS3TC_SYS_MEMORY_H
#include <cell/error.h>   /* CELL_OK */

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

#define SYS_MEMORY_PAGE_SIZE_MASK           0x0000000000000f00ULL
#define SYS_MEMORY_ACCESS_RIGHT_NONE        0x00000000000000f0ULL
#define SYS_MEMORY_ACCESS_RIGHT_MASK        0x00000000000000ffULL

/* sys_mmapper_allocate_address granularity */
#define SYS_MEMORY_GRANULARITY_1M           0x0000000000000400ULL
#define SYS_MEMORY_GRANULARITY_64K          0x0000000000000200ULL
#define SYS_MEMORY_GRANULARITY_MASK         0x0000000000000f00ULL

/* sys_mmapper_map_memory / search_and_map protection */
#define SYS_MEMORY_PROT_READ_ONLY           0x0000000000080000ULL
#define SYS_MEMORY_PROT_READ_WRITE          0x0000000000040000ULL
#define SYS_MEMORY_PROT_MASK                0x00000000000f0000ULL

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

/* Page attributes of the page holding `addr` (syscall 351).  The kernel
 * writes page_size as 32 bits whatever the caller's data model. */
typedef struct sys_page_attr {
	uint64_t attribute;
	uint64_t access_right;
	uint32_t page_size;
	uint32_t pad;
} sys_page_attr_t;

LV2_SYSCALL sys_memory_get_page_attribute(sys_addr_t addr, sys_page_attr_t *attr)
{
	lv2syscall2(351, (u64)addr, (u64)(uintptr_t)attr);
	return_to_user_prog(int);
}

/* Total / available memory of container `cid` (syscall 343). */
LV2_SYSCALL sys_memory_container_get_size(sys_memory_info_t *mem_info,
                                          sys_memory_container_t cid)
{
	lv2syscall2(343, (u64)(uintptr_t)mem_info, (u64)cid);
	return_to_user_prog(int);
}

/* Physical memory objects for the mmapper.  These five are sysPrxForUser
 * exports (liblv2_stub), not inline syscalls: allocate a memory object,
 * map it at an address reserved with sys_mmapper_allocate_address, unmap
 * it (returning its id), and free it. */
extern int sys_mmapper_allocate_memory(size_t size, uint64_t flags,
                                       sys_memory_t *mem_id);
extern int sys_mmapper_allocate_memory_from_container(size_t size,
                                                      sys_memory_container_t container,
                                                      uint64_t flags,
                                                      sys_memory_t *mem_id);
extern int sys_mmapper_free_memory(sys_memory_t mem_id);
extern int sys_mmapper_map_memory(sys_addr_t start_addr, sys_memory_t mem_id,
                                  uint64_t flags);
extern int sys_mmapper_unmap_memory(sys_addr_t start_addr, sys_memory_t *mem_id);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_MEMORY_H */
