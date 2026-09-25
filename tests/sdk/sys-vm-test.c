/*
 * sys-vm-test.c — Static assertion and signature verification test
 * for <sys/vm.h> constants, types, and Lv2 syscall wrappers.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/vm.h>

#if defined(__cplusplus)
static_assert(SYS_VM_STATE_INVALID == 0x0000ULL, "SYS_VM_STATE_INVALID mismatch");
static_assert(SYS_VM_STATE_UNUSED == 0x0001ULL, "SYS_VM_STATE_UNUSED mismatch");
static_assert(SYS_VM_STATE_ON_MEMORY == 0x0002ULL, "SYS_VM_STATE_ON_MEMORY mismatch");
static_assert(SYS_VM_STATE_STORED == 0x0004ULL, "SYS_VM_STATE_STORED mismatch");
static_assert(SYS_VM_POLICY_AUTO_RECOMMENDED == 1UL, "SYS_VM_POLICY_AUTO_RECOMMENDED mismatch");
static_assert(offsetof(sys_vm_statistics_t, page_fault_ppu) == 0, "offset page_fault_ppu");
static_assert(offsetof(sys_vm_statistics_t, page_fault_spu) == 8, "offset page_fault_spu");
static_assert(offsetof(sys_vm_statistics_t, page_in) == 16, "offset page_in");
static_assert(offsetof(sys_vm_statistics_t, page_out) == 24, "offset page_out");
static_assert(offsetof(sys_vm_statistics_t, pmem_total) == 32, "offset pmem_total");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(SYS_VM_STATE_INVALID == 0x0000ULL, "SYS_VM_STATE_INVALID mismatch");
_Static_assert(SYS_VM_STATE_UNUSED == 0x0001ULL, "SYS_VM_STATE_UNUSED mismatch");
_Static_assert(SYS_VM_STATE_ON_MEMORY == 0x0002ULL, "SYS_VM_STATE_ON_MEMORY mismatch");
_Static_assert(SYS_VM_STATE_STORED == 0x0004ULL, "SYS_VM_STATE_STORED mismatch");
_Static_assert(SYS_VM_POLICY_AUTO_RECOMMENDED == 1UL, "SYS_VM_POLICY_AUTO_RECOMMENDED mismatch");
_Static_assert(offsetof(sys_vm_statistics_t, page_fault_ppu) == 0, "offset page_fault_ppu");
_Static_assert(offsetof(sys_vm_statistics_t, page_fault_spu) == 8, "offset page_fault_spu");
_Static_assert(offsetof(sys_vm_statistics_t, page_in) == 16, "offset page_in");
_Static_assert(offsetof(sys_vm_statistics_t, page_out) == 24, "offset page_out");
_Static_assert(offsetof(sys_vm_statistics_t, pmem_total) == 32, "offset pmem_total");
#else
typedef char assert_vm_invalid[(SYS_VM_STATE_INVALID == 0x0000ULL) ? 1 : -1];
typedef char assert_vm_unused[(SYS_VM_STATE_UNUSED == 0x0001ULL) ? 1 : -1];
typedef char assert_vm_on_mem[(SYS_VM_STATE_ON_MEMORY == 0x0002ULL) ? 1 : -1];
typedef char assert_vm_stored[(SYS_VM_STATE_STORED == 0x0004ULL) ? 1 : -1];
typedef char assert_vm_policy[(SYS_VM_POLICY_AUTO_RECOMMENDED == 1UL) ? 1 : -1];
typedef char assert_offset_pf_ppu[(offsetof(sys_vm_statistics_t, page_fault_ppu) == 0) ? 1 : -1];
typedef char assert_offset_pf_spu[(offsetof(sys_vm_statistics_t, page_fault_spu) == 8) ? 1 : -1];
typedef char assert_offset_pi[(offsetof(sys_vm_statistics_t, page_in) == 16) ? 1 : -1];
typedef char assert_offset_po[(offsetof(sys_vm_statistics_t, page_out) == 24) ? 1 : -1];
typedef char assert_offset_pmem[(offsetof(sys_vm_statistics_t, pmem_total) == 32) ? 1 : -1];
#endif

int test_function_signatures(void)
{
    int (*fn_map)(size_t, size_t, sys_memory_container_t, uint64_t, uint64_t, sys_addr_t *) = sys_vm_memory_map;
    int (*fn_unmap)(sys_addr_t) = sys_vm_unmap;
    int (*fn_append)(sys_addr_t, size_t) = sys_vm_append_memory;
    int (*fn_return)(sys_addr_t, size_t) = sys_vm_return_memory;
    int (*fn_lock)(sys_addr_t, size_t) = sys_vm_lock;
    int (*fn_unlock)(sys_addr_t, size_t) = sys_vm_unlock;
    int (*fn_touch)(sys_addr_t, size_t) = sys_vm_touch;
    int (*fn_flush)(sys_addr_t, size_t) = sys_vm_flush;
    int (*fn_invalidate)(sys_addr_t, size_t) = sys_vm_invalidate;
    int (*fn_store)(sys_addr_t, size_t) = sys_vm_store;
    int (*fn_sync)(sys_addr_t, size_t) = sys_vm_sync;
    int (*fn_test)(sys_addr_t, size_t, uint64_t *) = sys_vm_test;
    int (*fn_stat)(sys_addr_t, sys_vm_statistics_t *) = sys_vm_get_statistics;

    (void)fn_map;
    (void)fn_unmap;
    (void)fn_append;
    (void)fn_return;
    (void)fn_lock;
    (void)fn_unlock;
    (void)fn_touch;
    (void)fn_flush;
    (void)fn_invalidate;
    (void)fn_store;
    (void)fn_sync;
    (void)fn_test;
    (void)fn_stat;
    return 0;
}

int main(void)
{
    return test_function_signatures();
}
