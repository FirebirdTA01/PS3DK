/*
 * PS3 Custom Toolchain — <sys/timer.h> Sony-SDK compat shim.
 *
 * Sony samples call `sys_timer_usleep(n)` / `sys_timer_sleep(n)` for
 * coarse-grained waits (frame throttling, sleep-between-retries, etc).
 * Both are thin wrappers around the LV2 sleep syscall.  We forward to
 * POSIX `usleep` from libsysbase, which newlib implements on top of the
 * same syscall, and chunk the 64-bit count so very long sleeps still
 * fit through useconds_t (typically 32-bit).
 */

#ifndef PS3TC_COMPAT_SYS_TIMER_H
#define PS3TC_COMPAT_SYS_TIMER_H

#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int sys_timer_usleep(uint64_t microseconds)
{
    /* useconds_t is typically uint32_t; drain large counts in chunks. */
    while (microseconds > (uint64_t)(1u << 31))
    {
        usleep(1u << 31);
        microseconds -= (1u << 31);
    }
    return usleep((useconds_t)microseconds);
}

static inline int sys_timer_sleep(uint32_t seconds)
{
    return sys_timer_usleep((uint64_t)seconds * 1000000ull);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_COMPAT_SYS_TIMER_H */
