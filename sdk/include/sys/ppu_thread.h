/*
 * PS3 Custom Toolchain — <sys/ppu_thread.h>
 *
 * reference-SDK-style PPU-thread API.  sys_ppu_thread_* wrappers forward to
 * the underlying lv2 syscall set that our PSL1GHT-derived `sys/thread.h`
 * exposes as sysThread*.  A thin alias layer gives callers the
 * the reference SDK identifiers without changing the runtime.
 *
 * The stack-info struct ships with the reference SDK `pst_addr` / `pst_size`
 * field names and a static_assert proves it remains layout-compatible
 * with `sys_ppu_thread_stack_t` as declared in <sys/thread.h> — the
 * wrapper that calls sysThreadGetStackInformation can therefore pass
 * the same storage through with a pointer cast.
 */

#ifndef PS3TC_SYS_PPU_THREAD_H
#define PS3TC_SYS_PPU_THREAD_H

#include <stdint.h>
#include <ppu-types.h>
#include <sys/thread.h>
#include <lv2/thread.h>
#include <sys/return_code.h>
#include <sys/lv2_syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flags accepted by sys_ppu_thread_create. */
#define SYS_PPU_THREAD_CREATE_JOINABLE   0x0000000000000001ULL
#define SYS_PPU_THREAD_CREATE_INTERRUPT  0x0000000000000002ULL

#define SYS_PPU_THREAD_ID_INVALID        0xffffffffu

typedef void (*sys_ppu_thread_entry_t)(uint64_t arg);

/* sys_ppu_thread_stack_t is declared in <sys/thread.h> with a union
 * that makes `.addr / .size` and `.pst_addr / .pst_size` interchangeable
 * views of the same bytes — samples written against either spelling
 * compile against the same storage. */

/* librt (runtime/lv2/librt/pthread.c).  A thread created here runs its entry
 * inside a librt trampoline, so a return from the entry, like
 * sys_ppu_thread_exit, runs the thread's exit cleanup: thread_local
 * destructors, then pthread key destructors (where C++
 * notify_all_at_thread_exit and promise::set_value_at_thread_exit act).
 * SYS_PPU_THREAD_CREATE_INTERRUPT threads are created as before.  The
 * PSL1GHT-level sysThreadCreate / sysThreadExit stay raw: code calling them
 * directly gets no exit cleanup.  Objects compiled against the earlier
 * inline wrappers keep the old behaviour until rebuilt. */
extern int __lv2_ppu_thread_create(sys_ppu_thread_t *thread_id,
                                   sys_ppu_thread_entry_t entry,
                                   uint64_t arg, int prio, size_t stacksize,
                                   uint64_t flags, const char *threadname);
extern void __lv2_thread_exit(uint64_t exit_code) __attribute__((noreturn));

static inline int sys_ppu_thread_create(sys_ppu_thread_t *thread_id,
                                        sys_ppu_thread_entry_t entry,
                                        uint64_t arg, int prio,
                                        size_t stacksize, uint64_t flags,
                                        const char *threadname)
{
    return __lv2_ppu_thread_create(thread_id, entry, arg, prio, stacksize,
                                   flags, threadname);
}

static inline int sys_ppu_thread_join(sys_ppu_thread_t threadid, uint64_t *retval)
{
    return (int)sysThreadJoin(threadid, retval);
}

static inline void sys_ppu_thread_exit(uint64_t retval)
{
    __lv2_thread_exit(retval);
}

static inline int sys_ppu_thread_get_id(sys_ppu_thread_t *threadid)
{
    return (int)sysThreadGetId(threadid);
}

static inline int sys_ppu_thread_get_priority(sys_ppu_thread_t threadid, int *prio)
{
    s32 p;
    int rc = (int)sysThreadGetPriority(threadid, &p);
    *prio = (int)p;
    return rc;
}

static inline int sys_ppu_thread_set_priority(sys_ppu_thread_t threadid, int prio)
{
    return (int)sysThreadSetPriority(threadid, (s32)prio);
}

static inline int sys_ppu_thread_yield(void)
{
    return (int)sysThreadYield();
}

static inline int sys_ppu_thread_detach(sys_ppu_thread_t threadid)
{
    return (int)sysThreadDetach(threadid);
}

/* sys_ppu_thread_once: run `init` exactly once per control word, which
 * starts as SYS_PPU_THREAD_ONCE_INIT.  A sysPrxForUser export. */
#define SYS_PPU_THREAD_ONCE_INIT    0
#define SYS_PPU_THREAD_DONE_INIT    1
extern void sys_ppu_thread_once(int *once_ctrl, void (*init)(void));

/* Whether the calling thread is joinable (syscall 46). */
static inline void sys_ppu_thread_get_join_state(int *isjoinable)
{
    lv2syscall1(46, (u64)(uintptr_t)isjoinable);
}

/* Rename a thread, as debuggers and the TTY show it (syscall 56). */
static inline int sys_ppu_thread_rename(sys_ppu_thread_t thread_id, const char *name)
{
    lv2syscall2(56, (u64)thread_id, (u64)(uintptr_t)name);
    return_to_user_prog(int);
}

/* Resume a thread stopped by a page fault (syscall 57) and read its
 * registers at the fault (syscall 58). */
static inline int sys_ppu_thread_recover_page_fault(sys_ppu_thread_t thread_id)
{
    lv2syscall1(57, (u64)thread_id);
    return_to_user_prog(int);
}

static inline int sys_ppu_thread_get_page_fault_context(sys_ppu_thread_t thread_id,
                                                        sys_ppu_thread_icontext_t *ctxp)
{
    lv2syscall2(58, (u64)thread_id, (u64)(uintptr_t)ctxp);
    return_to_user_prog(int);
}

static inline int sys_ppu_thread_get_stack_information(sys_ppu_thread_stack_t *info)
{
    return (int)sysThreadGetStackInformation(info);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_PPU_THREAD_H */
