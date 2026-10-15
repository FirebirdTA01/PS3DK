/* PPU sched priority-range queries implemented by librt. Do not define
 * _POSIX_THREAD_PRIORITY_SCHEDULING globally: that would advertise
 * priority scheduling as available system-wide and change newlib header
 * visibility for every <sched.h> consumer. The forward runs on every
 * inclusion, outside the include guard (as time.h and pthread.h do), so
 * every copy of this wrapper on the include path chains on to libc's
 * <sched.h> (which itself supplies SCHED_OTHER / SCHED_FIFO / SCHED_RR
 * and `struct sched_param`).
 *
 * newlib declares sched_get_priority_min()/max() only when that feature
 * macro is set; on this target it is off, so the two prototypes are added
 * here. librt (runtime/lv2/librt/pthread_sched.c) implements them over
 * the Lv-2 scheduler, where only SCHED_OTHER is meaningful: both queries
 * return 0 for SCHED_OTHER and -1/errno=EINVAL for any other policy. */
#include_next <sched.h>
#ifndef PS3DK_SCHED_WRAPPER_H
#define PS3DK_SCHED_WRAPPER_H
#if defined(__lv2ppu__)
#ifdef __cplusplus
extern "C" {
#endif
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
#ifdef __cplusplus
}
#endif
#endif
#endif
