/* PPU time APIs implemented by librt. Do not enable _POSIX_TIMERS: that
 * would advertise unimplemented timers and change newlib header visibility. */
#ifndef PS3DK_TIME_WRAPPER_H
#define PS3DK_TIME_WRAPPER_H
#include_next <time.h>
#if defined(__lv2ppu__)
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
