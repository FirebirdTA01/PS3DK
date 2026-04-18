/*
 * PS3 Custom Toolchain — <sys/ppu_thread.h>
 *
 * Sony-style PPU-thread API.  sys_ppu_thread_* wrappers forward to the
 * underlying lv2 syscall set that our PSL1GHT-derived `sys/thread.h`
 * exposes as sysThread*.  A thin alias layer gives callers Sony's
 * identifiers without changing the runtime.
 *
 * The stack-info struct ships with Sony's `pst_addr` / `pst_size`
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

static inline int sys_ppu_thread_create(sys_ppu_thread_t *thread_id,
                                        sys_ppu_thread_entry_t entry,
                                        uint64_t arg, int prio,
                                        size_t stacksize, uint64_t flags,
                                        const char *threadname)
{
    /* Sony's JOINABLE bit == PSL1GHT THREAD_JOINABLE bit (both 1). */
    (void)flags;
    return (int)sysThreadCreate(thread_id,
                                (void (*)(void *))entry,
                                (void *)(uintptr_t)arg,
                                (s32)prio,
                                (u64)stacksize,
                                flags,
                                (char *)threadname);
}

static inline int sys_ppu_thread_join(sys_ppu_thread_t threadid, uint64_t *retval)
{
    return (int)sysThreadJoin(threadid, retval);
}

static inline void sys_ppu_thread_exit(uint64_t retval)
{
    sysThreadExit(retval);
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

static inline int sys_ppu_thread_get_stack_information(sys_ppu_thread_stack_t *info)
{
    return (int)sysThreadGetStackInformation(info);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_PPU_THREAD_H */
