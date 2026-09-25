# PPU clocks, sleep and scheduling attributes

Include `<time.h>` for `clock_gettime` and `nanosleep`, and `<pthread.h>`
for the thread attribute setters. Both PPU ABIs link these functions from
`librt.a`; no feature macros that advertise unsupported POSIX timers are
enabled by the SDK wrapper.

`CLOCK_REALTIME` uses Lv-2 current time (seconds since the Epoch and
nanoseconds). `CLOCK_MONOTONIC` uses `sysGetSystemTime`, the sysPrxForUser
monotonic microsecond counter, with an unspecified origin and microsecond
granularity. It does not use
the wall-clock-derived `sys_time_get_system_time` compatibility wrapper.
Other clock IDs return -1/EINVAL. Null output returns -1/EFAULT; a seconds
value outside `time_t` returns -1/EOVERFLOW. Failed calls leave output alone.
`time_t` is signed 64-bit in both PPU ABIs, including ILP32, so this overflow
check is not a Y2038 boundary.

`nanosleep` rejects negative seconds or nanoseconds and nanoseconds at least
1,000,000,000 with -1/EINVAL. A null request returns -1/EFAULT. It rounds up
to microseconds and splits intervals longer than the 32-bit `sysUsleep`
argument can represent. Zero is an immediate success. Scheduler delays may
extend the sleep. Success leaves the optional remaining-time output alone.

On Lv-2 EINTR, the function returns -1, sets errno to EINTR, and, if requested,
writes the original interval minus elapsed monotonic microseconds, clamped
to zero. Request and remaining may share storage. It does not restart the
sleep. Other translated errors leave remaining alone. This describes how
a returned Lv-2 interruption is handled; the SDK does not add POSIX signal
delivery or pthread cancellation. Host boundary controls cover EINTR;
the RPCS3 regression covers normal clock advancement and sleep only.
The contract follows [POSIX nanosleep](https://pubs.opengroup.org/onlinepubs/9799919799/functions/nanosleep.html).

`pthread_attr_setschedpolicy` accepts only `SCHED_OTHER`, and
`pthread_attr_setinheritsched` accepts only `PTHREAD_INHERIT_SCHED`. Other
values return ENOTSUP without changing the attribute; null/uninitialized
attributes return EINVAL. These return error numbers directly, leaving errno
alone. They retain the shim's existing mode: `pthread_create` uses its
default Lv-2 priority or the positive `sched_priority` already stored by
`pthread_attr_setschedparam`. They do not implement POSIX realtime policies
or add dynamic priority inheritance from the creating thread.
