/*
 * PS3 Custom Toolchain — <sys/interrupt.h>
 *
 * PPU Interrupt management Lv2 syscall interface.
 * Implements canonical reference-SDK prototypes and constants,
 * with inline syscall forwarders and PSL1GHT compatibility aliases.
 */

#ifndef __PS3DK_SYS_INTERRUPT_H__
#define __PS3DK_SYS_INTERRUPT_H__

#include <stdint.h>
#include <ppu-types.h>
#include <sys/lv2_syscall.h>
#include <sys/return_code.h>

#define SYS_HW_THREAD_ANY               0xFFFFFFFEU
#define SYS_HW_THREAD_INVALID           0xFFFFFFFFU
#define SYS_INTERRUPT_TAG_ID_INVALID    0xFFFFFFFFU

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t sys_irqoutlet_id_t;
typedef uint32_t sys_hw_thread_t;
typedef uint32_t sys_interrupt_tag_t;
typedef uint32_t sys_interrupt_thread_handle_t;
typedef uint32_t sys_interrupt_handler_handle_t;
typedef void (*__ppu_intr_handler_entry)(uint64_t, uint64_t);

static inline int sys_interrupt_tag_create(sys_interrupt_tag_t *intrtag,
                                          sys_irqoutlet_id_t irq,
                                          sys_hw_thread_t hwthread)
{
    lv2syscall3(80, (uint64_t)intrtag, irq, (uint64_t)hwthread);
    return_to_user_prog(int);
}

static inline int sys_interrupt_tag_destroy(sys_interrupt_tag_t intrtag)
{
    lv2syscall1(81, (uint64_t)intrtag);
    return_to_user_prog(int);
}

static inline int _sys_interrupt_thread_establish(sys_interrupt_thread_handle_t *ih,
                                                  sys_interrupt_tag_t intrtag,
                                                  uint64_t intrthread,
                                                  uint64_t arg1,
                                                  uint64_t arg2)
{
    lv2syscall5(84, (uint64_t)ih, (uint64_t)intrtag, intrthread, arg1, arg2);
    return_to_user_prog(int);
}

static inline int sys_interrupt_thread_establish(sys_interrupt_thread_handle_t *ih,
                                                 sys_interrupt_tag_t intrtag,
                                                 sys_ppu_thread_t intrthread,
                                                 uint64_t arg)
{
    return _sys_interrupt_thread_establish(ih, intrtag, (uint64_t)intrthread, arg, 0);
}

static inline void sys_interrupt_thread_eoi(void)
{
    lv2syscall0(88);
}

static inline int _sys_interrupt_thread_disestablish(sys_interrupt_thread_handle_t ih,
                                                     uint64_t *tls_mem)
{
    lv2syscall2(89, (uint64_t)ih, (uint64_t)tls_mem);
    return_to_user_prog(int);
}

/* ---- PSL1GHT compatibility aliases ------------------------------ */
static inline int sysInterruptTagCreate(sys_interrupt_tag_t *intrTag,
                                        sys_irqoutlet_id_t irq,
                                        sys_hw_thread_t hwThread)
{
    return sys_interrupt_tag_create(intrTag, irq, hwThread);
}

static inline int sysInterruptTagDestroy(sys_interrupt_tag_t intrTag)
{
    return sys_interrupt_tag_destroy(intrTag);
}

static inline int sysInterruptThreadEstablish(sys_interrupt_thread_handle_t *ih,
                                              sys_interrupt_tag_t intrTag,
                                              sys_ppu_thread_t intrThread,
                                              uint64_t arg)
{
    return sys_interrupt_thread_establish(ih, intrTag, intrThread, arg);
}

static inline void sysInterruptThreadEOI(void)
{
    sys_interrupt_thread_eoi();
}

static inline int _sysInterruptThreadDisestablish(sys_interrupt_thread_handle_t ih,
                                                  uint64_t *tlsMem)
{
    return _sys_interrupt_thread_disestablish(ih, tlsMem);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_INTERRUPT_H__ */
