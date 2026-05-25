/*
 * PS3 Custom Toolchain — <sys/timer.h> cell-SDK compat shim.
 *
 * Cell-SDK samples call `sys_timer_usleep(n)` / `sys_timer_sleep(n)` for
 * coarse-grained waits (frame throttling, sleep-between-retries, etc).
 * Both wrappers enter the kernel directly through the LV2 timer
 * syscalls.
 */

#ifndef PS3TC_COMPAT_SYS_TIMER_H
#define PS3TC_COMPAT_SYS_TIMER_H

#include <sys/lv2_syscall.h>
#include <sys/sys_types.h>

#ifndef SYS_TIMER_USLEEP
#define SYS_TIMER_USLEEP 141
#endif

#ifndef SYS_TIMER_SLEEP
#define SYS_TIMER_SLEEP 142
#endif

#ifdef __cplusplus
extern "C" {
#endif

LV2_SYSCALL sys_timer_usleep(usecond_t sleep_time)
{
    lv2syscall1(SYS_TIMER_USLEEP, (u64)sleep_time);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_sleep(second_t sleep_time)
{
    lv2syscall1(SYS_TIMER_SLEEP, (u64)sleep_time);
    return_to_user_prog(int);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_COMPAT_SYS_TIMER_H */
