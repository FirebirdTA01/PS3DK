/*
 * PS3 Custom Toolchain — <sys/thread.h>
 *
 * Underlying LV2 syscall surface for PPU threads.  <sys/ppu_thread.h>
 * layers the Sony-style sys_ppu_thread_* wrapper names on top of
 * these; most user code should #include the higher-level header
 * rather than going directly to the syscalls.
 *
 * The stack-info struct carries both the long-standing addr / size
 * field spellings and Sony's pst_addr / pst_size aliases through a
 * transparent union, so callers that declare
 *   sys_ppu_thread_stack_t info;
 * can read either `info.addr` or `info.pst_addr` and get the same
 * bytes.  Layout (pointer + u32) is unchanged.
 */

#ifndef PS3TC_SYS_THREAD_H
#define PS3TC_SYS_THREAD_H

#include <ppu-lv2.h>
#include <lv2/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flags accepted by sys_ppu_thread_create / sysThreadCreate. */
#define THREAD_JOINABLE   1
#define THREAD_INTERRUPT  2

/* Stack-info record returned by sysThreadGetStackInformation.
 *
 * Sony's ABI has this as { sys_addr_t pst_addr; size_t pst_size; }.
 * The stack base is always a user-space address (32-bit fits via
 * sys_addr_t), and the length is size_t.  We expose both the Sony
 * pst_* spellings and the long-standing PSL1GHT addr / size spellings
 * at the same storage offsets via anonymous unions — either name
 * works on a single `sys_ppu_thread_stack_t` variable. */
typedef struct _sys_ppu_thread_stack_t
{
    union { sys_addr_t pst_addr; sys_addr_t addr; };
    union { size_t     pst_size; size_t     size; };
} sys_ppu_thread_stack_t;

/* --- create / destroy ----------------------------------------------- */

/* Out-of-line — too large to inline as a syscall wrapper; the real
 * implementation lives in liblv2.a. */
s32 sysThreadCreate(sys_ppu_thread_t *threadid,
                    void (*entry)(void *), void *arg,
                    s32 priority, u64 stacksize, u64 flags,
                    char *threadname);

LV2_SYSCALL sysThreadJoin(sys_ppu_thread_t threadid, u64 *retval)
{
    lv2syscall2(44, threadid, (u64)retval);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysThreadYield(void)
{
    lv2syscall0(43);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysThreadDetach(sys_ppu_thread_t threadid)
{
    lv2syscall1(45, threadid);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysThreadJoinState(s32 *joinable)
{
    lv2syscall1(46, (u64)joinable);
    return_to_user_prog(s32);
}

/* --- priority / identity -------------------------------------------- */

LV2_SYSCALL sysThreadSetPriority(sys_ppu_thread_t threadid, s32 prio)
{
    lv2syscall2(47, threadid, (u64)prio);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysThreadGetPriority(sys_ppu_thread_t threadid, s32 *prio)
{
    lv2syscall2(48, threadid, (u64)prio);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysThreadRename(sys_ppu_thread_t threadid, const char *name)
{
    lv2syscall2(56, threadid, (u64)name);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysThreadRecoverPageFault(sys_ppu_thread_t threadid)
{
    lv2syscall1(57, threadid);
    return_to_user_prog(s32);
}

/* --- stack information ---------------------------------------------- *
 *
 * Syscall 49 returns the stack buffer pointer as a 32-bit address
 * (the primary thread's stack lives in the low 4 GiB of the 32-bit
 * address space) plus a 32-bit length.  We widen the pointer into
 * the caller's native `void *` before returning.
 */
LV2_SYSCALL sysThreadGetStackInformation(sys_ppu_thread_stack_t *info)
{
    struct { u32 addr; u32 size; } raw;
    lv2syscall1(49, (u64)(&raw));
    info->pst_addr = (sys_addr_t)raw.addr;
    info->pst_size = (size_t)raw.size;
    return_to_user_prog(s32);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_THREAD_H */
