/*
 * PS3 Custom Toolchain - <sys/vm.h> LV2 virtual memory (syscalls 300-312).
 *
 * sys_vm_memory_map reserves `vsize` bytes of address space backed by
 * `psize` bytes of physical memory taken from `container`; the kernel pages
 * between them on demand.  The other calls take a range inside that area:
 * append/return physical memory, lock/unlock pages, touch (page in), flush
 * and invalidate, store (page out), sync, test (SYS_VM_STATE_* per range)
 * and statistics.
 *
 * Sizes the kernel writes are 32-bit fields here (pmem_total, pmem_used), so
 * the structure has one layout in ILP32 and LP64 userland.
 */

#ifndef PS3TC_SYS_VM_H
#define PS3TC_SYS_VM_H
#include <cell/error.h>   /* CELL_OK */

#include <stdint.h>
#include <ppu-types.h>
#include <sys/lv2_syscall.h>
#include <sys/memory.h>   /* sys_memory_container_t */

#define SYS_VM_MEMORY_MAP       300
#define SYS_VM_UNMAP            301
#define SYS_VM_APPEND_MEMORY    302
#define SYS_VM_RETURN_MEMORY    303
#define SYS_VM_LOCK             304
#define SYS_VM_UNLOCK           305
#define SYS_VM_TOUCH            306
#define SYS_VM_FLUSH            307
#define SYS_VM_INVALIDATE       308
#define SYS_VM_STORE            309
#define SYS_VM_SYNC             310
#define SYS_VM_TEST             311
#define SYS_VM_GET_STATISTICS   312

/* sys_vm_test results */
#define SYS_VM_STATE_INVALID    0x0000ULL
#define SYS_VM_STATE_UNUSED     0x0001ULL
#define SYS_VM_STATE_ON_MEMORY  0x0002ULL
#define SYS_VM_STATE_STORED     0x0004ULL

/* sys_vm_memory_map policy */
#define SYS_VM_POLICY_AUTO_RECOMMENDED  1UL

typedef struct sys_vm_statistics {
	uint64_t page_fault_ppu;
	uint64_t page_fault_spu;
	uint64_t page_in;
	uint64_t page_out;
	uint32_t pmem_total;
	uint32_t pmem_used;
	uint64_t time;
} sys_vm_statistics_t;

#ifdef __cplusplus
extern "C" {
#endif

LV2_SYSCALL sys_vm_memory_map(size_t vsize, size_t psize,
                              sys_memory_container_t container,
                              uint64_t flag, uint64_t policy, sys_addr_t *addr)
{
	lv2syscall6(SYS_VM_MEMORY_MAP, (u64)vsize, (u64)psize, (u64)container,
	            flag, policy, (u64)(uintptr_t)addr);
	return_to_user_prog(int);
}

LV2_SYSCALL sys_vm_unmap(sys_addr_t addr)
{
	lv2syscall1(SYS_VM_UNMAP, (u64)addr);
	return_to_user_prog(int);
}

#define PS3TC_SYS_VM_RANGE_CALL(fn, nr)                         \
	LV2_SYSCALL fn(sys_addr_t addr, size_t size)                \
	{                                                           \
		lv2syscall2(nr, (u64)addr, (u64)size);                  \
		return_to_user_prog(int);                               \
	}

PS3TC_SYS_VM_RANGE_CALL(sys_vm_append_memory, SYS_VM_APPEND_MEMORY)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_return_memory, SYS_VM_RETURN_MEMORY)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_lock,          SYS_VM_LOCK)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_unlock,        SYS_VM_UNLOCK)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_touch,         SYS_VM_TOUCH)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_flush,         SYS_VM_FLUSH)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_invalidate,    SYS_VM_INVALIDATE)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_store,         SYS_VM_STORE)
PS3TC_SYS_VM_RANGE_CALL(sys_vm_sync,          SYS_VM_SYNC)

#undef PS3TC_SYS_VM_RANGE_CALL

LV2_SYSCALL sys_vm_test(sys_addr_t addr, size_t size, uint64_t *result)
{
	lv2syscall3(SYS_VM_TEST, (u64)addr, (u64)size, (u64)(uintptr_t)result);
	return_to_user_prog(int);
}

LV2_SYSCALL sys_vm_get_statistics(sys_addr_t addr, sys_vm_statistics_t *stat)
{
	lv2syscall2(SYS_VM_GET_STATISTICS, (u64)addr, (u64)(uintptr_t)stat);
	return_to_user_prog(int);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_VM_H */
