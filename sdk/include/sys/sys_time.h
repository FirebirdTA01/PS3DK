/*
 * PS3 Custom Toolchain — <sys/sys_time.h> Sony-SDK compat shim.
 *
 * Sony samples read a 64-bit microsecond monotonic counter via
 * `sys_time_get_system_time()`.  Our PSL1GHT tree exposes the same
 * underlying LV2 syscall through <sys/systime.h>'s `sysGetCurrentTime`
 * (seconds + nanoseconds), so we compose the microsecond counter from
 * that.  `sys_time_get_timebase_frequency()` forwards to
 * `sysGetTimebaseFrequency()` unchanged.
 */

#ifndef PS3TC_COMPAT_SYS_SYS_TIME_H
#define PS3TC_COMPAT_SYS_SYS_TIME_H

#include <stdint.h>
#include <ppu-types.h>     /* `system_time_t` already declared here */
#include <sys/systime.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline system_time_t sys_time_get_system_time(void)
{
    uint64_t sec = 0;
    uint64_t nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    return (system_time_t)(sec * 1000000ull + nsec / 1000ull);
}

static inline uint64_t sys_time_get_timebase_frequency(void)
{
    return sysGetTimebaseFrequency();
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_COMPAT_SYS_SYS_TIME_H */
