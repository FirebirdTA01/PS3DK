/* SPDX-License-Identifier: BSD-2-Clause
 * Process-wide network holders: see net_hold.h. One lock, one holder set.
 */
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
static unsigned holders;          /* PS3DK_NET_HOLD_* bits */
static uint32_t active_pool;      /* the pool the network was brought up with */
static int (*active_unload)(void); /* set when this layer loaded the module */

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

int __ps3dk_net_hold_acquire(unsigned holder, void *pool, uint32_t bytes, int32_t flags,
                             int (*load)(void), int (*unload)(void))
{
    int rc;
    if (pthread_mutex_lock(&hold_lock)) return -1;
    if (holders & holder) {
        /* already held: the legacy and profiler front ends are idempotent; a repeated Cell init reaches the
         * raw call exactly as before holders existed */
        rc = holder == PS3DK_NET_HOLD_CELL ? raw_init(pool, bytes, flags) : 0;
    } else if (holders) {
        /* the network is up for another holder: join it; its pool stays in use */
        holders |= holder;
        rc = 0;
        if (bytes > active_pool)
            say("libnet: the network is already initialized with a smaller memory pool; that pool stays in use\n");
    } else {
        rc = load ? load() : 0;
        if (!rc) {
            rc = raw_init(pool, bytes, flags);
            if (!rc) {
                holders = holder;
                active_pool = bytes;
                active_unload = load ? unload : 0;
            } else if (load && unload) {
                unload();
            }
        }
    }
    pthread_mutex_unlock(&hold_lock);
    return rc;
}

int __ps3dk_net_hold_release(unsigned holder)
{
    int rc = 0;
    if (pthread_mutex_lock(&hold_lock)) return -1;
    if (!(holders & holder)) {
        /* not a holder: the legacy and profiler front ends do nothing; a Cell finalize with no holder at all
         * reaches the raw call as before, but never finalizes a network another holder still uses */
        if (holder == PS3DK_NET_HOLD_CELL && !holders)
            rc = __ps3dk_raw_sys_net_finalize_network();
    } else if (holders != holder) {
        holders &= ~holder;
    } else {
        rc = __ps3dk_raw_sys_net_finalize_network();
        if (!rc) {
            int (*unload)(void) = active_unload;
            holders = 0;
            active_pool = 0;
            active_unload = 0;
            if (unload) {
                int urc = unload();
                if (holder == PS3DK_NET_HOLD_LEGACY) rc = urc; /* netDeinitialize has always returned it */
            }
        }
    }
    pthread_mutex_unlock(&hold_lock);
    return rc;
}
