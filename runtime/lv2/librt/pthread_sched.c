/* Supported scheduling attributes for the existing Lv-2 pthread shim.
 * SPDX-License-Identifier: BSD-2-Clause */
#include <errno.h>
#include <pthread.h>
#include <sched.h>

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    if (policy != SCHED_OTHER)
        return ENOTSUP;
    attr->schedpolicy = policy;
    return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inherit)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    if (inherit != PTHREAD_INHERIT_SCHED)
        return ENOTSUP;
    attr->inheritsched = inherit;
    return 0;
}

/* ------------------------------------------------------------------ *
 * sched_get_priority_min / sched_get_priority_max
 *
 * Our runtime supports only SCHED_OTHER; any other policy
 * (SCHED_FIFO, SCHED_RR, an out-of-range value) is not a valid input.
 *
 * Contract (agreed with codex, t_a2644ed1):
 *   - SCHED_OTHER : return 0.  The portable code pattern
 *         sched_param p = { .sched_priority = sched_get_priority_min(SCHED_OTHER) };
 *     is expected to yield a priority the shim honours.  A priority of
 *     0 here means "default" — pthread_create in `pthread.c` uses the
 *     Lv-2 default (PT_DEFAULT_PRIORITY, 1000) unless
 *     `sched_priority > 0`, so 0 falls through to that default.  This
 *     matches the portable expectation that a thread set to the
 *     priority range minimum of the default policy gets the system
 *     default, not a special "highest" slot.
 *   - Any other policy value: return -1, errno = EINVAL.  This is the
 *     POSIX error channel for sched_get_priority_* (unlike
 *     pthread_attr_*, which return the error number on the
 *     return-value channel).
 *
 * The existing non-POSIX behaviour of `pthread_create`: a
 * `sched_priority > 0` is passed straight through as the Lv-2 PPU
 * thread priority (lower value runs first).  sched_priority = 0 means
 * "default" (Lv-2 1000).  This is a non-POSIX extension OUTSIDE the
 * portable range: POSIX requires sched_priority to lie within the
 * min/max of the policy, and for SCHED_OTHER that range is [0, 0] on
 * this shim.  Values above 0 are the caller's choice, not the shim's
 * promise.  The pass-through in `pthread.c` and
 * `pthread_attr_setschedparam` are left unchanged.
 * ------------------------------------------------------------------ */

int sched_get_priority_min(int policy)
{
    if (policy != SCHED_OTHER) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sched_get_priority_max(int policy)
{
    if (policy != SCHED_OTHER) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
