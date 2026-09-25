/* POSIX clocks and relative sleep over Lv-2.
 * SPDX-License-Identifier: BSD-2-Clause */
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/systime.h>
#include <sys/lv2errno.h>

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    u64 sec, nsec;
    if (!tp) {
        errno = EFAULT;
        return -1;
    }
    if (clock_id == CLOCK_REALTIME) {
        s32 rc = sysGetCurrentTime(&sec, &nsec);
        if (rc)
            return lv2errno(rc);
    } else if (clock_id == CLOCK_MONOTONIC) {
        /* sysPrxForUser's monotonic microsecond counter, not wall time. */
        u64 usec = (u64)sysGetSystemTime();
        sec = usec / 1000000;
        nsec = (usec % 1000000) * 1000;
    } else {
        errno = EINVAL;
        return -1;
    }
    if ((time_t)sec < 0 || (u64)(time_t)sec != sec) {
        errno = EOVERFLOW;
        return -1;
    }
    tp->tv_sec = (time_t)sec;
    tp->tv_nsec = (long)nsec;
    return 0;
}

/* Subtract elapsed microseconds without converting a potentially enormous
 * time_t interval into a single overflowing nanosecond/microsecond integer. */
static struct timespec time_left(struct timespec request, u64 elapsed)
{
    u64 sec = elapsed / 1000000;
    long nsec = (long)(elapsed % 1000000) * 1000;
    if (sec > (u64)request.tv_sec ||
        (sec == (u64)request.tv_sec && nsec >= request.tv_nsec)) {
        request.tv_sec = 0;
        request.tv_nsec = 0;
    } else {
        request.tv_sec -= (time_t)sec;
        request.tv_nsec -= nsec;
        if (request.tv_nsec < 0) {
            --request.tv_sec;
            request.tv_nsec += 1000000000;
        }
    }
    return request;
}

int nanosleep(const struct timespec *request, struct timespec *remaining)
{
    struct timespec original, left;
    u64 start = 0;
    if (!request) {
        errno = EFAULT;
        return -1;
    }
    original = *request; /* request and remaining may alias */
    if (original.tv_sec < 0 || original.tv_nsec < 0 ||
        original.tv_nsec >= 1000000000) {
        errno = EINVAL;
        return -1;
    }
    left = original;
    if (remaining)
        start = (u64)sysGetSystemTime();
    while (left.tv_sec || left.tv_nsec) {
        /* sysUsleep's public argument is only 32 bits. Round upward and
         * chunk long sleeps instead of truncating either ABI's time_t. */
        u64 usec = UINT32_MAX;
        s32 rc;
        if ((u64)left.tv_sec <= UINT32_MAX / 1000000) {
            usec = (u64)left.tv_sec * 1000000 +
                   ((u64)left.tv_nsec + 999) / 1000;
            if (usec > UINT32_MAX)
                usec = UINT32_MAX;
        }
        rc = sysUsleep((u32)usec);
        if (rc) {
            int error = (int)lv2error(rc);
            if (error == EINTR && remaining) {
                u64 now = (u64)sysGetSystemTime();
                *remaining = time_left(original, now >= start ? now - start : 0);
            }
            errno = error;
            return -1;
        }
        left = time_left(left, usec);
    }
    return 0;
}
