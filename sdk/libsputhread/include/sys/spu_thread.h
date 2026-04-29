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

#endif /* __PS3DK_SYS_SPU_THREAD_H__ */
