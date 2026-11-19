/* SPDX-License-Identifier: BSD-2-Clause
 * Network holders: see net_hold.h. One lock; the app front ends keep their
 * historical behaviour unless the profiler holds the network.
 */
#define _DEFAULT_SOURCE 1 /* usleep under -std=c11 (newlib and glibc) */
#include "net_hold.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

struct wire_init { uint32_t memory, size; int32_t flags; };
_Static_assert(sizeof(struct wire_init) == 12, "firmware init parameters");
extern int __ps3dk_raw_sys_net_initialize_network_ex(const struct wire_init *);
extern int __ps3dk_raw_sys_net_finalize_network(void);

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
/* The app front ends: legacy_up is netInitialize's historical flag; legacy_joined marks a netInitialize that joined
 * the profiler's network without a raw call; cell_up records a Cell initialize that succeeded (bookkeeping only). */
static int legacy_up, legacy_joined, cell_up;
static int (*legacy_unload)(void);
/* The profiler: held, and whether it brought the network up (owner) and loaded the module. */
static int prof_held, prof_owner;
static int (*prof_unload)(void);
static void (*prof_evict)(void);
static int evicting;

static int raw_init(void *pool, uint32_t bytes, int32_t flags)
{
    struct wire_init wire;
    if ((uintptr_t)pool > UINT32_MAX) { errno = EFAULT; return -1; }
    wire.memory = (uint32_t)(uintptr_t)pool; wire.size = bytes; wire.flags = flags;
    return __ps3dk_raw_sys_net_initialize_network_ex(&wire);
}

static void say(const char *s)
{
    unsigned long n = 0;
    while (s[n]) ++n;
    (void)!write(2, s, n);
}

/* With the lock held: wait out an eviction another thread runs. */
static void settle(void)
{
    while (evicting) {
        pthread_mutex_unlock(&hold_lock);
        usleep(1000);
        pthread_mutex_lock(&hold_lock);
    }
}

/* With the lock held and the profiler holding: tell it to stop using the network, without the lock (it takes its
 * own locks and calls __ps3dk_net_prof_release), then forget its hold. */
static void evict(void)
{
    void (*fn)(void) = prof_evict;
    evicting = 1;
    pthread_mutex_unlock(&hold_lock);
    if (fn) fn();
    pthread_mutex_lock(&hold_lock);
    prof_held = 0;
    evicting = 0;
}

int __ps3dk_net_legacy_init(void *pool, uint32_t bytes, int (*load)(void), int (*unload)(void))
{
    int rc = pthread_mutex_lock(&hold_lock);
    if (rc) return -1;
    settle();
    legacy_unload = unload;
    if (legacy_up) rc = 0;
    else if (prof_held && prof_owner) {
        legacy_up = legacy_joined = 1; /* the profiler's static pool, never freed: safe to share */
        rc = 0;
    } else {
        rc = load();
        if (!rc) {
            rc = raw_init(pool, bytes, 0);
            if (!rc) legacy_up = 1;
            else unload();
        }
    }
    pthread_mutex_unlock(&hold_lock); return rc;
}

int __ps3dk_net_legacy_deinit(void)
{
    int rc = pthread_mutex_lock(&hold_lock);
    if (rc) return -1;
    settle();
    if (!legacy_up) rc = 0;
    else if (legacy_joined && prof_held && prof_owner) {
        legacy_up = legacy_joined = 0; /* the profiler's network stays up for the profiler */
        rc = 0;
    } else {
        if (prof_held) evict(); /* the profiler joined this network: it lets go first */
        rc = __ps3dk_raw_sys_net_finalize_network();
        if (!rc) {
            legacy_up = legacy_joined = 0;
            rc = legacy_unload ? legacy_unload() : 0;
        }
    }
    pthread_mutex_unlock(&hold_lock); return rc;
}

int __ps3dk_net_cell_init(void *pool, uint32_t bytes, int32_t flags)
{
    int rc = pthread_mutex_lock(&hold_lock);
    if (rc) return -1;
    settle();
    if (prof_held && prof_owner && legacy_joined) {
        /* netInitialize joined the profiler's network: without the profiler that network would have been
         * netInitialize's own.  Hand it (and the module the profiler loaded) to netInitialize, so this call and a later
         * netDeinitialize give exactly the results the same sequence gives without the profiler. */
        int (*unload)(void) = prof_unload;
        evict();
        prof_owner = 0;
        prof_unload = 0;
        legacy_joined = 0;
        if (unload) legacy_unload = unload;
    } else if (prof_held && prof_owner) {
        /* The app brings its own pool: the profiler steps aside and its network goes; the NET module stays loaded
         * (the app loaded it as well, and an unload here could not be counted against the app's). */
        evict();
        (void)__ps3dk_raw_sys_net_finalize_network();
        prof_owner = 0;
        prof_unload = 0;
        say("libnet: the app initialized the network with its own pool; the profiler's live auto-start stopped\n");
    }
    rc = raw_init(pool, bytes, flags);
    if (!rc) cell_up = 1;
    pthread_mutex_unlock(&hold_lock); return rc;
}

int __ps3dk_net_cell_finalize(void)
{
    int rc = pthread_mutex_lock(&hold_lock);
    if (rc) return -1;
    settle();
    if (prof_held && cell_up) {
        evict(); /* the Cell app's own network: the profiler lets go first */
        prof_owner = 0;
        prof_unload = 0;
    }
    /* A finalize from an app that never initialized reaches the firmware exactly as before, and the profiler keeps
     * its hold and its module count: the call is the app's, its result the firmware's. */
    rc = __ps3dk_raw_sys_net_finalize_network();
    if (!rc) cell_up = 0;
    pthread_mutex_unlock(&hold_lock); return rc;
}

int __ps3dk_net_prof_acquire(void *pool, uint32_t bytes, int (*load)(void), int (*unload)(void), int (*is_loaded)(void),
                             void (*evict_fn)(void))
{
    int rc = pthread_mutex_lock(&hold_lock);
    if (rc) return -1;
    if (evicting) rc = -1;
    else if (prof_held) rc = 0;
    else if (legacy_up || cell_up) {
        prof_held = 1; prof_owner = 0; prof_evict = evict_fn; /* the app's network, the app's pool */
        rc = 0;
    } else if (is_loaded && is_loaded()) {
        /* No libnet front end brought the network up, but the NET module is already loaded: someone outside this
         * layer (a library, or a hook built against another libnet) may own a live network.  The firmware answers a
         * second initialize with 0 (measured on HEN and in RPCS3), so a raw init cannot tell; assume it is foreign:
         * join without a raw call, and never finalize it or unload anything (prof_owner stays 0). */
        prof_held = 1; prof_owner = 0; prof_evict = evict_fn;
        rc = 0;
    } else {
        rc = load ? load() : 0;
        if (!rc) {
            rc = raw_init(pool, bytes, 0);
            if (!rc) {
                prof_held = prof_owner = 1;
                prof_unload = load ? unload : 0;
                prof_evict = evict_fn;
            } else if (load && unload) {
                unload();
            }
        }
    }
    pthread_mutex_unlock(&hold_lock); return rc;
}

int __ps3dk_net_prof_release(void)
{
    int rc = pthread_mutex_lock(&hold_lock);
    if (rc) return -1;
    if (!prof_held) rc = 0;
    else if (evicting) { prof_held = 0; rc = 0; } /* the evicting front end finishes the teardown */
    else if (prof_owner && !legacy_up) {
        rc = __ps3dk_raw_sys_net_finalize_network();
        if (!rc) {
            int (*unload)(void) = prof_unload;
            prof_held = prof_owner = 0;
            prof_unload = 0;
            if (unload) (void)unload();
        }
    } else {
        if (prof_owner) {
            /* netInitialize joined: the network (and the module the profiler loaded) is now netDeinitialize's */
            legacy_joined = 0;
            if (prof_unload) legacy_unload = prof_unload;
        }
        prof_held = prof_owner = 0;
        prof_unload = 0;
        rc = 0;
    }
    pthread_mutex_unlock(&hold_lock); return rc;
}
