/*
 * PS3 Custom Toolchain — <sys/sys_time.h> the platform ABI compat shim.
 *
 * existing PS3 samples read the 64-bit microsecond LV2 system time via
 * `sys_time_get_system_time()`, the sysPrxForUser import declared below.
 * `sys_time_get_timebase_frequency()` forwards to
 * `sysGetTimebaseFrequency()` unchanged.
 */

#ifndef PS3TC_COMPAT_SYS_SYS_TIME_H
#define PS3TC_COMPAT_SYS_SYS_TIME_H
#include <cell/error.h>   /* CELL_OK */

#include <stdint.h>
#include <ppu-types.h>     /* `system_time_t` already declared here */
#include <sys/systime.h>
#include <sys/sys_types.h> /* usecond_t, second_t */
#include <sys/timer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LV2 system time: microseconds since the system started, the clock the
 * kernel timers (<sys/timer.h>), event-queue timeouts and timer events use.
 * It is the sysPrxForUser export (liblv2_stub), not wall-clock time: an
 * earlier inline version here returned sys_time_get_current_time() in
 * microseconds since 1970, so an absolute timer deadline computed from it
 * lay decades in the future and never fired. */
extern system_time_t sys_time_get_system_time(void);

static inline uint64_t sys_time_get_timebase_frequency(void)
{
    return sysGetTimebaseFrequency();
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_COMPAT_SYS_SYS_TIME_H */
