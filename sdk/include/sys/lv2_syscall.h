/* sys/lv2_syscall.h - lv2 syscall wrapper macros.
 *
 * Clean-room re-implementation of the lv2syscall0..8 macro family.
 * The lv2 kernel ABI:
 *
 *   r3..r10 carry args 1..8 on entry, results 1..8 on return.
 *   r11     carries the syscall number.
 *   `sc`    traps into the kernel.
 *
 * Each lv2syscallN macro pre-binds register variables to the slot the
 * kernel expects, emits a single `sc` with all of them as input +
 * output operands, and leaves the results in the same names so
 * `return_to_user_prog` and `register_passing_N` can pull them back
 * out at the call site.
 *
 * Uses the standards-conformant `__asm__` / `__asm__ __volatile__`
 * spelling so callers can use any of -std=gnu99 / -std=gnu++17 /
 * -std=c99 / -std=c++17 without the bare GCC extension keyword
 * tripping strict-mode parsing.
 *
 * This header replaces transitive use of PSL1GHT's <ppu-lv2.h> in
 * cell-SDK-style sub-headers.  Functionally identical at the kernel
 * boundary - the .word emitted for `sc` and the register pre-binds
 * are byte-for-byte the same.
 */
#ifndef __PS3DK_SYS_LV2_SYSCALL_H__
#define __PS3DK_SYS_LV2_SYSCALL_H__

#include <ppu-types.h>          /* u64 */

#define LV2_INLINE  static inline
#define LV2_SYSCALL LV2_INLINE s32

/* Common register clobber list shared by every variant: r0/r12/lr/ctr/xer
 * + the condition-register fields the kernel may scribble + memory. */
#define _LV2_CLOBBERS \
    "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory"

#define _LV2_OUT \
    "=r"(p1), "=r"(p2), "=r"(p3), "=r"(p4), \
    "=r"(p5), "=r"(p6), "=r"(p7), "=r"(p8), "=r"(scn)

#define _LV2_IN \
    "r"(p1), "r"(p2), "r"(p3), "r"(p4), \
    "r"(p5), "r"(p6), "r"(p7), "r"(p8), "r"(scn)

#define lv2syscall0(syscall)                                                     \
        register u64 p1 __asm__("3");                                            \
        register u64 p2 __asm__("4");                                            \
        register u64 p3 __asm__("5");                                            \
        register u64 p4 __asm__("6");                                            \
        register u64 p5 __asm__("7");                                            \
        register u64 p6 __asm__("8");                                            \
        register u64 p7 __asm__("9");                                            \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall1(syscall,a1)                                                  \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4");                                            \
        register u64 p3 __asm__("5");                                            \
        register u64 p4 __asm__("6");                                            \
        register u64 p5 __asm__("7");                                            \
        register u64 p6 __asm__("8");                                            \
        register u64 p7 __asm__("9");                                            \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall2(syscall,a1,a2)                                               \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4") = (a2);                                     \
        register u64 p3 __asm__("5");                                            \
        register u64 p4 __asm__("6");                                            \
        register u64 p5 __asm__("7");                                            \
        register u64 p6 __asm__("8");                                            \
        register u64 p7 __asm__("9");                                            \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall3(syscall,a1,a2,a3)                                            \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4") = (a2);                                     \
        register u64 p3 __asm__("5") = (a3);                                     \
        register u64 p4 __asm__("6");                                            \
        register u64 p5 __asm__("7");                                            \
        register u64 p6 __asm__("8");                                            \
        register u64 p7 __asm__("9");                                            \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall4(syscall,a1,a2,a3,a4)                                         \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4") = (a2);                                     \
        register u64 p3 __asm__("5") = (a3);                                     \
        register u64 p4 __asm__("6") = (a4);                                     \
        register u64 p5 __asm__("7");                                            \
        register u64 p6 __asm__("8");                                            \
        register u64 p7 __asm__("9");                                            \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall5(syscall,a1,a2,a3,a4,a5)                                      \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4") = (a2);                                     \
        register u64 p3 __asm__("5") = (a3);                                     \
        register u64 p4 __asm__("6") = (a4);                                     \
        register u64 p5 __asm__("7") = (a5);                                     \
        register u64 p6 __asm__("8");                                            \
        register u64 p7 __asm__("9");                                            \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall6(syscall,a1,a2,a3,a4,a5,a6)                                   \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4") = (a2);                                     \
        register u64 p3 __asm__("5") = (a3);                                     \
        register u64 p4 __asm__("6") = (a4);                                     \
        register u64 p5 __asm__("7") = (a5);                                     \
        register u64 p6 __asm__("8") = (a6);                                     \
        register u64 p7 __asm__("9");                                            \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall7(syscall,a1,a2,a3,a4,a5,a6,a7)                                \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4") = (a2);                                     \
        register u64 p3 __asm__("5") = (a3);                                     \
        register u64 p4 __asm__("6") = (a4);                                     \
        register u64 p5 __asm__("7") = (a5);                                     \
        register u64 p6 __asm__("8") = (a6);                                     \
        register u64 p7 __asm__("9") = (a7);                                     \
        register u64 p8 __asm__("10");                                           \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

#define lv2syscall8(syscall,a1,a2,a3,a4,a5,a6,a7,a8)                             \
        register u64 p1 __asm__("3") = (a1);                                     \
        register u64 p2 __asm__("4") = (a2);                                     \
        register u64 p3 __asm__("5") = (a3);                                     \
        register u64 p4 __asm__("6") = (a4);                                     \
        register u64 p5 __asm__("7") = (a5);                                     \
        register u64 p6 __asm__("8") = (a6);                                     \
        register u64 p7 __asm__("9") = (a7);                                     \
        register u64 p8 __asm__("10") = (a8);                                    \
        register u64 scn __asm__("11") = (syscall);                              \
        __asm__ __volatile__("sc" : _LV2_OUT : _LV2_IN : _LV2_CLOBBERS)

/* Result accessors.  After an lv2syscallN call, p1..p8 hold result
 * slots 1..8 and `return_to_user_prog(T)` casts slot 1 as the typical
 * "return code" path.  register_passing_K(T) pulls the K-th
 * additional out-arg (e.g. an event-queue receive that returns four
 * u64s from one syscall). */
#define return_to_user_prog(ret_type) return (ret_type)(p1)

#define register_passing_1(type)      ((type)(p2))
#define register_passing_2(type)      ((type)(p3))
#define register_passing_3(type)      ((type)(p4))
#define register_passing_4(type)      ((type)(p5))
#define register_passing_5(type)      ((type)(p6))
#define register_passing_6(type)      ((type)(p7))
#define register_passing_7(type)      ((type)(p8))

#endif /* __PS3DK_SYS_LV2_SYSCALL_H__ */
