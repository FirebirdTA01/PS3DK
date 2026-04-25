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

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SPU_THREAD_H__ */
