/*
 * PS3 Custom Toolchain - <sys/interrupt.h>
 *
 * PPU interrupt tags and interrupt threads: the lv2 system calls 80, 81, 84, 88 and 89 as the system
 * declares them, with PSL1GHT's sysInterrupt* spellings.  The types (sys_interrupt_tag_t,
 * sys_irqoutlet_id_t, sys_hw_thread_t, sys_interrupt_thread_handle_t) come from <ppu-types.h>.
 * sys_interrupt_thread_disestablish is a library call (liblv2_stub, also exported as PSL1GHT's
 * sysInterruptThreadDisestablish, which <lv2/interrupt.h> declares); it is not the raw system call 89
 * that _sys_interrupt_thread_disestablish makes.  SDK-owned: this header replaces PSL1GHT's.
 */
#ifndef __PS3DK_SYS_INTERRUPT_H__
#define __PS3DK_SYS_INTERRUPT_H__
#define __SYS_INTERRUPT_H__   /* PSL1GHT's guard: a later include of its copy is a no-op */

#include <stdint.h>
#include <ppu-types.h>
#include <ppu-lv2.h>          /* as PSL1GHT's copy: the LV2_* macros and lv2syscallN */
#include <lv2/interrupt.h>     /* sysInterruptThreadDisestablish, as PSL1GHT's sys/interrupt.h provided */

#define SYS_HW_THREAD_ANY               0xFFFFFFFEU
#define SYS_HW_THREAD_INVALID           0xFFFFFFFFU
#define SYS_INTERRUPT_TAG_ID_INVALID    0xFFFFFFFFU

#ifdef __cplusplus
extern "C" {
#endif

/* Pointers go to the kernel as 64-bit values: through uintptr_t, so an ILP32 pointer is zero-extended. */
static inline int sys_interrupt_tag_create(sys_interrupt_tag_t *intrtag, sys_irqoutlet_id_t irq,
                                           sys_hw_thread_t hwthread)
{
    lv2syscall3(80, (uint64_t)(uintptr_t)intrtag, (uint64_t)irq, (uint64_t)hwthread);
    return_to_user_prog(int);
}

static inline int sys_interrupt_tag_destroy(sys_interrupt_tag_t intrtag)
{
    lv2syscall1(81, (uint64_t)intrtag);
    return_to_user_prog(int);
}

static inline int _sys_interrupt_thread_establish(sys_interrupt_thread_handle_t *ih, sys_interrupt_tag_t intrtag,
                                                  uint64_t intrthread, uint64_t arg1, uint64_t arg2)
{
    lv2syscall5(84, (uint64_t)(uintptr_t)ih, (uint64_t)intrtag, intrthread, arg1, arg2);
    return_to_user_prog(int);
}

static inline int sys_interrupt_thread_establish(sys_interrupt_thread_handle_t *ih, sys_interrupt_tag_t intrtag,
                                                 sys_ppu_thread_t intrthread, uint64_t arg)
{
    return _sys_interrupt_thread_establish(ih, intrtag, (uint64_t)intrthread, arg, 0);
}

static inline void sys_interrupt_thread_eoi(void)
{
    lv2syscall0(88);
}

/* The library call: tears down the interrupt thread and its resources (liblv2_stub). */
extern int sys_interrupt_thread_disestablish(sys_interrupt_thread_handle_t ih);

/* The raw system call 89 underneath it. */
static inline int _sys_interrupt_thread_disestablish(sys_interrupt_thread_handle_t ih, uint64_t *tls_mem)
{
    lv2syscall2(89, (uint64_t)ih, (uint64_t)(uintptr_t)tls_mem);
    return_to_user_prog(int);
}

/* PSL1GHT spellings */
static inline s32 sysInterruptTagCreate(sys_interrupt_tag_t *intrTag, sys_irqoutlet_id_t irq, sys_hw_thread_t hwThread)
{
    return sys_interrupt_tag_create(intrTag, irq, hwThread);
}

static inline s32 sysInterruptTagDestroy(sys_interrupt_tag_t intrTag)
{
    return sys_interrupt_tag_destroy(intrTag);
}

static inline s32 sysInterruptThreadEstablish(sys_interrupt_thread_handle_t *ih, sys_interrupt_tag_t intrTag,
                                              sys_ppu_thread_t intrThread, u64 arg)
{
    return sys_interrupt_thread_establish(ih, intrTag, intrThread, arg);
}

static inline void sysInterruptThreadEOI(void)
{
    sys_interrupt_thread_eoi();
}

static inline s32 _sysInterruptThreadDisestablish(sys_interrupt_thread_handle_t ih, u64 *tlsMem)
{
    return _sys_interrupt_thread_disestablish(ih, tlsMem);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_INTERRUPT_H__ */
