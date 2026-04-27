/*
 * PS3 Custom Toolchain — <sys/sys_time.h> cell-SDK compat shim.
 *
 * Cell-SDK samples read a 64-bit microsecond monotonic counter via
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

/* Reference-SDK monotonic-clock units.  Our SDK consumes them as
 * 64-bit unsigned counters; sample code typedefs CTimer fields to
 * usecond_t / second_t expecting them to be available system-wide. */
#ifndef PS3TC_HAVE_USECOND_T
#define PS3TC_HAVE_USECOND_T
typedef uint64_t usecond_t;
typedef uint64_t second_t;
#endif

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
