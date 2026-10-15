/* PPU time APIs implemented by librt. Do not enable _POSIX_TIMERS: that
 * would advertise unimplemented timers and change newlib header visibility.
 * The forward runs on every inclusion, outside the include guard (as
 * stdarg.h does), so every copy of this wrapper on the include path chains
 * on to libc's <time.h>. */
#include_next <time.h>
#ifndef PS3DK_TIME_WRAPPER_H
#define PS3DK_TIME_WRAPPER_H
/* Newlib patch 0018 declares these itself (_LV2_TIME_DECLS); older newlib does not. */
#if defined(__lv2ppu__) && !defined(_LV2_TIME_DECLS)
#include <sys/types.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif
#ifdef __cplusplus
extern "C" {
#endif
int clock_gettime(clockid_t clock_id, struct timespec *tp);
int nanosleep(const struct timespec *request, struct timespec *remaining);
#ifdef __cplusplus
}
#endif
#endif
#endif
