/* The LV2 platform layer (step 3, target half) per TARGET-PLAN-r2 section 1 and its rulings R1-R8.  Written for review
 * only: not built, not in SUBLIBS or any install rule.  It is hook-free: test faults are injected only in the host PAL,
 * so this file refuses PS3DK_PROF_TEST_HOOKS.  Both ABIs: pointers and size_t are 4 bytes in ILP32 and 8 in LP64;
 * every LV2 value the core sees (thread ids, timebase, microseconds) is a u64 in both, and there is no pointer-to-
 * integer arithmetic here (the one alignment check reads the low bits only). */
#ifndef PS3DK_PROF_PAL_LV2
#error "ps3dk prof: build every libps3dkprof object with -DPS3DK_PROF_PAL_LV2, so the core and this PAL see one pal.h"
#endif
#ifdef PS3DK_PROF_TEST_HOOKS
#error "ps3dk prof: the LV2 PAL is hook-free; test hooks exist only in the host PAL"
#endif
#include "pal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/ppu_thread.h>
#include <sys/sys_time.h>

/* One 128-byte-aligned block at init (R2: posix_memalign / free, pending the link-map proof of the provider).  The
 * core sizes the block with overflow checks before the call (prof_core.c take()); this rejects a zero size and a
 * provider that hands back a block without the requested alignment, which would otherwise go unnoticed. */
void *pal_alloc(size_t bytes)
{
    void *p = NULL;
    if (bytes == 0)
        return NULL;
    if (posix_memalign(&p, 128, bytes) != 0 || !p)
        return NULL;
    if (((uintptr_t)p & 127u) != 0) {
        free(p);
        return NULL;
    }
    return p;
}

void pal_free(void *p) { free(p); }

/* R5: librt mutexes, created explicitly here (pthread_mutex_init calls sysMutexCreate), so the lazy creation path for
 * statically initialised mutexes is never taken. */
int pal_mutex_init(pal_mutex *m) { return pthread_mutex_init(m, NULL) == 0 ? 0 : -1; }
int pal_mutex_destroy(pal_mutex *m) { return pthread_mutex_destroy(m) == 0 ? 0 : -1; }
int pal_mutex_lock(pal_mutex *m) { return pthread_mutex_lock(m) == 0 ? 0 : -1; }
int pal_mutex_unlock(pal_mutex *m) { return pthread_mutex_unlock(m) == 0 ? 0 : -1; }

/* The APP owner: the checked LV2 thread id (plan 5.4, Q1), NOT librt pthread_self, whose table is PT_THREADS_MAX 32
 * and which can return 0.  Validity is the return code; SYS_PPU_THREAD_ID_INVALID is not used as a sentinel. */
int pal_owner_get(pal_owner *o)
{
    sys_ppu_thread_t id;
    o->valid = 0;
    if (sys_ppu_thread_get_id(&id) != 0)
        return -1;
    o->id = (uint64_t)id;
    o->valid = 1;
    return 0;
}

/* 1 only when the owner was recorded AND the current id is read successfully AND equals it: a failure is never the
 * owner (fail closed). */
int pal_owner_is_self(const pal_owner *o)
{
    sys_ppu_thread_t id;
    if (!o->valid)
        return 0;
    if (sys_ppu_thread_get_id(&id) != 0)
        return 0;
    return (uint64_t)id == o->id ? 1 : 0;
}

/* The drain thread (THREAD mode) on librt pthreads: one of librt's PT_THREADS_MAX slots.  The core bounds stack_bytes
 * (16 KiB to 1 MiB) before the call. */
int pal_thread_create(pal_thread *t, void *(*fn)(void *), void *arg, size_t stack_bytes)
{
    pthread_attr_t a;
    int rc;
    if (pthread_attr_init(&a) != 0)
        return -1;
    rc = pthread_attr_setstacksize(&a, stack_bytes);
    if (rc == 0)
        rc = pthread_create(t, &a, fn, arg);
    /* Deliberately unchecked (codex 03:21): an error path here could lose ownership of a drain thread that
     * pthread_create already started.  Source-bound assumption: the pinned librt pthread_attr_destroy returns 0 for a
     * non-NULL attribute (runtime/lv2/librt/pthread.c:1009-1015@ea58c6ea: EINVAL only for NULL, else clears
     * is_initialized and returns 0), and &a is a non-NULL local. */
    (void)pthread_attr_destroy(&a);
    return rc == 0 ? 0 : -1;
}

int pal_thread_join(pal_thread t) { return pthread_join(t, NULL) == 0 ? 0 : -1; }

/* The raw timebase, one full-width read; the core puts the compiler barriers around it (prof_core.c p_tb, plan 5.5).
 * No hardware serialization is implied. */
uint64_t pal_tb(void) { return __builtin_ppc_get_timebase(); }

/* 0 means unavailable (init then fails with ECLOCK). */
uint64_t pal_tb_freq(void) { return sys_time_get_timebase_frequency(); }

/* librt CLOCK_MONOTONIC is sysGetSystemTime microseconds split into sec/nsec; recombined in u64. */
int pal_mono_now_us(uint64_t *us)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0)
        return -1;
    *us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    return 0;
}

/* librt nanosleep, not sysUsleep(u32), whose argument truncates at 2^32 microseconds. */
void pal_sleep_us(uint64_t us)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* R3: status plus out-param; *out is written only on success, so no placeholder id is ever recorded. */
int pal_os_thread_id(uint64_t *out)
{
    sys_ppu_thread_t id;
    if (sys_ppu_thread_get_id(&id) != 0)
        return -1;
    *out = (uint64_t)id;
    return 0;
}

/* The file sink through librt: open remaps O_CREAT / O_EXCL to the LV2 flags and maps the LV2 "exists" error to the
 * platform EEXIST (R1: the core compares against PAL_EEXIST = EEXIST from this build's errno.h). */
int pal_open_excl(const char *path, int *fd, int *err)
{
    *fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (*fd < 0) {
        *err = errno;
        return -1;
    }
    return 0;
}

long pal_write(int fd, const void *buf, unsigned long n, int *err)
{
    long r = (long)write(fd, buf, (size_t)n);
    if (r < 0)
        *err = errno;
    return r;
}

int pal_close(int fd, int *err)
{
    if (close(fd) != 0) {
        *err = errno;
        return -1;
    }
    return 0;
}
