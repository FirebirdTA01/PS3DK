#ifndef PS3TC_PROF_WAIT_ADAPTER_H
#define PS3TC_PROF_WAIT_ADAPTER_H
/* Included only by the observed pthread variant, after pt_err and LV2 types.
 * Weak references do not extract the profiler: variant link rules own that. */
#include <ps3dk/prof_wait_internal.h>
#pragma weak ps3dk_prof__wait_begin
#pragma weak ps3dk_prof__wait_end
#pragma weak __ps3dk_prof_wait_thread_start
#pragma weak __ps3dk_prof_wait_thread_exit

static __thread unsigned pt_wait_observer_busy;
#define PT_WAIT_SITE ((uint64_t)(uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))

static void
pt_wait_notify(void (*hook)(void))
{
    int saved_errno;
    if (hook == NULL || pt_wait_observer_busy)
        return;
    saved_errno = errno;
    pt_wait_observer_busy = 1;
    hook();
    pt_wait_observer_busy = 0;
    errno = saved_errno;
}

static int
pt_wait_call(uint32_t operation, uint32_t target, uint32_t secondary,
             uint64_t timeout, uint64_t site)
{
    ps3dk_prof_wait_cookie cookie;
    s32 raw;
    int result, saved_errno;

    if (pt_wait_observer_busy || ps3dk_prof__wait_begin == NULL || ps3dk_prof__wait_end == NULL)
        return pt_err(operation == 3 ? sysCondWait(target, timeout) : sysMutexLock(target, timeout));

    saved_errno = errno;
    pt_wait_observer_busy = 1;
    cookie = ps3dk_prof__wait_begin(operation, target, secondary, timeout, site);
    errno = saved_errno;
    raw = operation == 3 ? sysCondWait(target, timeout) : sysMutexLock(target, timeout);
    result = pt_err(raw);
    saved_errno = errno;
    if (cookie.admitted)
        ps3dk_prof__wait_end(cookie, raw, result);
    errno = saved_errno;
    pt_wait_observer_busy = 0;
    return result;
}
#endif
