/* Deterministic scheduling seam around real profiler init and registration.
 * This is not the LV2 pthread adapter, target TLS, or wait-cost validation. */
#define _DEFAULT_SOURCE
#include "prof_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_int start_allowed;
static void *(*entry)(void *);
static void *entry_arg;
extern int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern int __real_ps3dk_prof_init(const ps3dk_prof_config *);
extern int __real_usleep(useconds_t);
#ifdef PS3DK_PROF_WITH_WAITS
extern void __ps3dk_prof_wait_thread_start(void);
extern void __ps3dk_prof_wait_thread_exit(void);
#endif
static void *start(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&start_allowed, memory_order_acquire))
        __real_usleep(100);
#ifdef PS3DK_PROF_WITH_WAITS
    __ps3dk_prof_wait_thread_start();
#endif
    void *result = entry(entry_arg);
#ifdef PS3DK_PROF_WITH_WAITS
    __ps3dk_prof_wait_thread_exit();
#endif
    return result;
}
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*fn)(void *), void *arg)
{
    entry = fn;
    entry_arg = arg;
    return __real_pthread_create(thread, attr, start, NULL);
}
int __wrap_ps3dk_prof_init(const ps3dk_prof_config *cfg)
{
    int rc = __real_ps3dk_prof_init(cfg);
    atomic_store_explicit(&start_allowed, 1, memory_order_release);
    return rc;
}
int __wrap_usleep(useconds_t usec)
{
    /* The new pre-init handshake yields here. Without it the worker cannot
     * start until init returns, exposing the invalid pre-existing premise. */
    atomic_store_explicit(&start_allowed, 1, memory_order_release);
    return __real_usleep(usec);
}
