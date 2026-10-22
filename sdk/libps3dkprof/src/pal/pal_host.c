/* The host (POSIX) platform layer for the B tests.  Test faults (PS3DK_PROF_TEST_HOOKS) are injected here, so the
 * portable core sees them exactly as it would see real platform failures. */
#define _POSIX_C_SOURCE 200809L
#include "pal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifdef PS3DK_PROF_TEST_HOOKS
#include "../core/prof_test_hooks.h"
#define T_HOOKS ps3dk_prof_test_hooks
#endif

void *pal_alloc(size_t bytes)
{
    void *p = NULL;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.alloc_fail)
        return NULL;
#endif
    if (posix_memalign(&p, 128, bytes) != 0)
        return NULL;
    return p;
}

void pal_free(void *p) { free(p); }

int pal_mutex_init(pal_mutex *m) { return pthread_mutex_init(m, NULL) == 0 ? 0 : -1; }
int pal_mutex_destroy(pal_mutex *m) { return pthread_mutex_destroy(m) == 0 ? 0 : -1; }
int pal_mutex_lock(pal_mutex *m) { return pthread_mutex_lock(m) == 0 ? 0 : -1; }
int pal_mutex_unlock(pal_mutex *m) { return pthread_mutex_unlock(m) == 0 ? 0 : -1; }

int pal_owner_get(pal_owner *o)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.id_fail)
        return -1;
#endif
    o->t = pthread_self();
    return 0;
}

int pal_owner_is_self(const pal_owner *o)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.id_fail)
        return 0;   /* a failed lookup is never the owner */
#endif
    return pthread_equal(pthread_self(), o->t) ? 1 : 0;
}

int pal_thread_create(pal_thread *t, void *(*fn)(void *), void *arg, size_t stack_bytes)
{
    pthread_attr_t a;
    int rc;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.thread_create_fail)
        return -1;
#endif
    if (pthread_attr_init(&a) != 0)
        return -1;
    /* Host stacks need more than the PPU default; never go below the host minimum. */
    if (stack_bytes < (size_t)256 * 1024)
        stack_bytes = (size_t)256 * 1024;
    rc = pthread_attr_setstacksize(&a, stack_bytes);
    if (rc == 0)
        rc = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return rc == 0 ? 0 : -1;
}

int pal_thread_join(pal_thread t)
{
    int rc = pthread_join(t, NULL);
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.join_fail)
        return -1;
#endif
    return rc == 0 ? 0 : -1;
}

uint64_t pal_tb(void)
{
    struct timespec ts;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.clock)
        return T_HOOKS.clock();
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t pal_tb_freq(void)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.freq_zero)
        return 0;
#endif
    return 1000000000ull;   /* the host timebase is nanoseconds */
}

int pal_mono_now_us(uint64_t *us)
{
    struct timespec ts;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.mono_clock_fail)
        return -1;
#endif
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    *us = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
    return 0;
}

void pal_sleep_us(uint64_t us)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

uint64_t pal_os_thread_id(void) { return (uint64_t)(uintptr_t)pthread_self(); }

int pal_open_excl(const char *path, int *fd, int *err)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.open_errno) {
        *err = T_HOOKS.open_errno;
        return -1;
    }
#endif
    *fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (*fd < 0) {
        *err = errno;
        return -1;
    }
    return 0;
}

long pal_write(int fd, const void *buf, unsigned long n, int *err)
{
    long r;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.write_fault) {
        long fr = 0;
        int fe = 0;
        if (T_HOOKS.write_fault(n, &fr, &fe)) {
            if (fr < 0)
                *err = fe;
            if (fr > 0)
                fr = (long)write(fd, buf, (size_t)fr);   /* a short write of fr bytes really happens */
            return fr;
        }
    }
#endif
    r = (long)write(fd, buf, (size_t)n);
    if (r < 0)
        *err = errno;
    return r;
}

int pal_close(int fd, int *err)
{
    int rc = close(fd);
#ifdef PS3DK_PROF_TEST_HOOKS
    if (T_HOOKS.close_fail) {
        *err = T_HOOKS.close_fail;
        return -1;
    }
#endif
    if (rc != 0) {
        *err = errno;
        return -1;
    }
    return 0;
}
