/* sys/spu_thread.h - SPU-side spu thread / group lifecycle syscalls.
 *
 * The SPU traps into lv2 via the `stop` instruction with an opcode
 * that selects which kind of exit to perform (this thread / whole
 * group).  Implementations live in libsputhread.a (SPU).  All
 * routines are noreturn — control transfers to the kernel scheduler
 * which tears the SPU thread or group down and returns the supplied
 * exit code to the PPU-side joiner.
 */
#ifndef __PS3DK_SYS_SPU_THREAD_H__
#define __PS3DK_SYS_SPU_THREAD_H__

#ifndef __SPU__
#error "sys/spu_thread.h (SPU side) needs __SPU__; PPU has its own copy"
#endif

#include <stdint.h>
#include <spu_intrinsics.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU-thread MMIO problem-state base + per-thread signal-notify register
 * offsets, used by SPU code that pokes a sibling SPU's signalling
 * registers (e.g. the spuchain sample).  Matches the reference layout:
 * each SPU thread's problem-state area is SPU_THREAD_BASE + n*OFFSET, and
 * the two signal-notify registers sit at the fixed local offsets below. */
#ifndef SPU_THREAD_BASE
#define SPU_THREAD_BASE          0xF0000000ULL
#endif
#ifndef SPU_THREAD_OFFSET
#define SPU_THREAD_OFFSET        0x00100000ULL
#endif
#ifndef SPU_THREAD_Sig_Notify_1
#define SPU_THREAD_Sig_Notify_1  0x5400C
#endif
#ifndef SPU_THREAD_Sig_Notify_2
#define SPU_THREAD_Sig_Notify_2  0x5C00C
#endif

extern void sys_spu_thread_exit(int exit_code)        __attribute__((noreturn));
extern void sys_spu_thread_group_exit(int exit_code)  __attribute__((noreturn));

/* Cooperative yield: returns control to the kernel scheduler, which
 * may resume this thread immediately or run another in the group.
 * Returns to the caller after the kernel re-dispatches us. */
extern void sys_spu_thread_group_yield(void);

/* Driver-only: replace the running SPU image with a new system
 * module loaded at `newAddress`.  Returns CELL_OK on success or a
 * negative status code; on EAGAIN (0x8001000a) the wrapper retries
 * internally before returning. */
extern int  sys_spu_thread_switch_system_module(uint32_t newAddress);

#ifdef __cplusplus
}
#endif

/* --- PSL1GHT-compat aliases (deprecated) ----------------------------
 * Upstream PSL1GHT names these SPU thread routines without the sys_
 * prefix.  libsputhread provides weak alias symbols (see
 * src/spu_thread_exit.S) so unmodified PSL1GHT SPU code links; these
 * deprecated extern decls make each call site warn to switch to the
 * modern sys_spu_thread_* name.  Real symbols (not static inline) so a
 * sample that self-declares the legacy name does not conflict.
 * Define __PS3DK_NO_PSL1GHT_COMPAT__ to drop the legacy aliases. */
#ifndef __PS3DK_NO_PSL1GHT_COMPAT__
extern void spu_thread_exit(int exit_code)
    __attribute__((noreturn, deprecated("PSL1GHT-compat: use sys_spu_thread_exit()")));
extern void spu_thread_group_exit(int exit_code)
    __attribute__((noreturn, deprecated("PSL1GHT-compat: use sys_spu_thread_group_exit()")));
extern void spu_thread_group_yield(void)
    __attribute__((deprecated("PSL1GHT-compat: use sys_spu_thread_group_yield()")));
#endif /* __PS3DK_NO_PSL1GHT_COMPAT__ */

#endif /* __PS3DK_SYS_SPU_THREAD_H__ */
