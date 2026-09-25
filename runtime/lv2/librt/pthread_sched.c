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
