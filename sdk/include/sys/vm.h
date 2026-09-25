/*
 * PS3 Custom Toolchain — <sys/vm.h>
 *
 * CellOS Lv-2 Virtual Memory Syscall Interface.
 *
 * Implements canonical inline syscall wrappers (syscalls 300..312)
 * for virtual memory management, page-fault handling and VM statistics.
 */

#ifndef __PS3DK_SYS_VM_H__
#define __PS3DK_SYS_VM_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/return_code.h>
#include <sys/lv2_types.h>
#include <sys/lv2_syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VM page state flags */
#define SYS_VM_STATE_INVALID                    0x0000ULL
#define SYS_VM_STATE_UNUSED                     0x0001ULL
#define SYS_VM_STATE_ON_MEMORY                  0x0002ULL
#define SYS_VM_STATE_STORED                     0x0004ULL

/* VM policy */
#define SYS_VM_POLICY_AUTO_RECOMMENDED          1UL

#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef uint32_t sys_memory_container_t;
#endif

/* VM statistics structure */
struct sys_vm_statistics {
    uint64_t page_fault_ppu;
    uint64_t page_fault_spu;
    uint64_t page_in;
    uint64_t page_out;
    size_t pmem_total;
    size_t pmem_used;
    uint64_t time;
};
typedef struct sys_vm_statistics sys_vm_statistics_t;

/* ---- Lv2 Virtual Memory syscall wrappers (300..312) -------------- */

static inline int sys_vm_memory_map(size_t vsize, size_t psize,
                                    sys_memory_container_t container,
                                    uint64_t flag, uint64_t policy,
                                    sys_addr_t *addr)
{
    lv2syscall6(300, (uint64_t)vsize, (uint64_t)psize, (uint64_t)container,
                flag, policy, (uint64_t)(uintptr_t)addr);
    return_to_user_prog(int);
}

static inline int sys_vm_unmap(sys_addr_t addr)
{
    lv2syscall1(301, (uint64_t)addr);
    return_to_user_prog(int);
}

static inline int sys_vm_append_memory(sys_addr_t addr, size_t size)
{
    lv2syscall2(302, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_return_memory(sys_addr_t addr, size_t size)
{
    lv2syscall2(303, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_lock(sys_addr_t addr, size_t size)
{
    lv2syscall2(304, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_unlock(sys_addr_t addr, size_t size)
{
    lv2syscall2(305, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_touch(sys_addr_t addr, size_t size)
{
    lv2syscall2(306, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_flush(sys_addr_t addr, size_t size)
{
    lv2syscall2(307, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_invalidate(sys_addr_t addr, size_t size)
{
    lv2syscall2(308, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_store(sys_addr_t addr, size_t size)
{
    lv2syscall2(309, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_sync(sys_addr_t addr, size_t size)
{
    lv2syscall2(310, (uint64_t)addr, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_vm_test(sys_addr_t addr, size_t size, uint64_t *result)
{
    lv2syscall3(311, (uint64_t)addr, (uint64_t)size, (uint64_t)(uintptr_t)result);
    return_to_user_prog(int);
}

static inline int sys_vm_get_statistics(sys_addr_t addr,
                                        sys_vm_statistics_t *stat)
{
    lv2syscall2(312, (uint64_t)addr, (uint64_t)(uintptr_t)stat);
    return_to_user_prog(int);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_VM_H__ */
