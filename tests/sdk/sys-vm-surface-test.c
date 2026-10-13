/* <sys/vm.h> and the <sys/memory.h> additions: every LV2 virtual-memory
 * call, the page-attribute and container-size syscalls, and the five
 * sysPrxForUser mmapper memory-object imports.  sys-vm-surface-test.sh
 * compiles this in C and C++, ILP32 and LP64, checks each syscall wrapper's
 * number in the object code and that each import is a call, not an sc. */
#include <stddef.h>
#include <sys/vm.h>
#include <sys/memory.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif
#define NOINLINE __attribute__((noinline))

NOINLINE int v_sys_vm_memory_map(sys_addr_t *a) { return sys_vm_memory_map(0x2000000, 0x100000, 1, SYS_MEMORY_PAGE_SIZE_64K, SYS_VM_POLICY_AUTO_RECOMMENDED, a); }
NOINLINE int v_sys_vm_unmap(sys_addr_t a) { return sys_vm_unmap(a); }
NOINLINE int v_sys_vm_append_memory(sys_addr_t a) { return sys_vm_append_memory(a, 0x10000); }
NOINLINE int v_sys_vm_return_memory(sys_addr_t a) { return sys_vm_return_memory(a, 0x10000); }
NOINLINE int v_sys_vm_lock(sys_addr_t a) { return sys_vm_lock(a, 0x10000); }
NOINLINE int v_sys_vm_unlock(sys_addr_t a) { return sys_vm_unlock(a, 0x10000); }
NOINLINE int v_sys_vm_touch(sys_addr_t a) { return sys_vm_touch(a, 0x10000); }
NOINLINE int v_sys_vm_flush(sys_addr_t a) { return sys_vm_flush(a, 0x10000); }
NOINLINE int v_sys_vm_invalidate(sys_addr_t a) { return sys_vm_invalidate(a, 0x10000); }
NOINLINE int v_sys_vm_store(sys_addr_t a) { return sys_vm_store(a, 0x10000); }
NOINLINE int v_sys_vm_sync(sys_addr_t a) { return sys_vm_sync(a, 0x10000); }
NOINLINE int v_sys_vm_test(sys_addr_t a, uint64_t *r) { return sys_vm_test(a, 0x10000, r); }
NOINLINE int v_sys_vm_get_statistics(sys_addr_t a, sys_vm_statistics_t *s) { return sys_vm_get_statistics(a, s); }
NOINLINE int v_sys_memory_container_get_size(sys_memory_info_t *i) { return sys_memory_container_get_size(i, 1); }
NOINLINE int v_sys_memory_get_page_attribute(sys_addr_t a, sys_page_attr_t *p) { return sys_memory_get_page_attribute(a, p); }
NOINLINE int v_sys_mmapper_allocate_memory(sys_memory_t *m) { return sys_mmapper_allocate_memory(0x100000, SYS_MEMORY_GRANULARITY_1M, m); }
NOINLINE int v_sys_mmapper_allocate_memory_from_container(sys_memory_t *m) { return sys_mmapper_allocate_memory_from_container(0x100000, 1, SYS_MEMORY_GRANULARITY_1M, m); }
NOINLINE int v_sys_mmapper_free_memory(sys_memory_t m) { return sys_mmapper_free_memory(m); }
NOINLINE int v_sys_mmapper_map_memory(sys_addr_t a, sys_memory_t m) { return sys_mmapper_map_memory(a, m, SYS_MEMORY_PROT_READ_WRITE); }
NOINLINE int v_sys_mmapper_unmap_memory(sys_addr_t a, sys_memory_t *m) { return sys_mmapper_unmap_memory(a, m); }

/* The kernel writes these layouts; they must not move with the data model. */
STATIC_ASSERT(sizeof(sys_vm_statistics_t) == 48, "vm statistics layout");
STATIC_ASSERT(offsetof(sys_vm_statistics_t, pmem_total) == 32, "pmem_total offset");
STATIC_ASSERT(offsetof(sys_vm_statistics_t, time) == 40, "time offset");
STATIC_ASSERT(sizeof(sys_page_attr_t) == 24, "page attribute layout");
STATIC_ASSERT(offsetof(sys_page_attr_t, page_size) == 16, "page_size offset");
STATIC_ASSERT(SYS_VM_STATE_ON_MEMORY == 2 && SYS_VM_POLICY_AUTO_RECOMMENDED == 1, "vm constants");
STATIC_ASSERT(SYS_MEMORY_PROT_READ_WRITE == 0x40000ULL && SYS_MEMORY_GRANULARITY_1M == 0x400ULL, "memory constants");
