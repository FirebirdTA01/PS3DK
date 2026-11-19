/* SPDX-License-Identifier: BSD-2-Clause
 * Network holders (net_hold.c). Internal to the SDK: libnet's own front ends
 * and the profiler's live auto-start use it; applications call netInitialize
 * and sys_net_initialize_network_ex as before.
 *
 * The app front ends (netInitialize / netDeinitialize and
 * sys_net_initialize_network_ex / sys_net_finalize_network) behave exactly as
 * they always did, call for call and result for result, whenever the profiler
 * holds nothing. Only while the profiler holds the network are they counted:
 *
 * - The network belongs to whoever brought it up (its pool is theirs).
 * - The profiler joins an app's network without a raw call. When the app
 *   tears that network down, the profiler is told first (its evict function
 *   stops the agent and drops the hold), and the app's call then reaches the
 *   firmware exactly as before: the finalize of the pool's owner is never
 *   swallowed, and the profiler makes no network call afterwards.
 * - When the profiler brought the network up (with its own static pool),
 *   netInitialize joins it (its pool is libnet's static one too) and
 *   netDeinitialize leaves it up while the profiler holds it. A Cell-style
 *   sys_net_initialize_network_ex brings its own pool: the profiler steps
 *   aside first (evicted, its network finalized, the NET module left loaded),
 *   and the app's call is passed straight through.
 * - The profiler's release finalizes only a network it brought up and no app
 *   front end has joined, and unloads only a module it loaded.
 */
#ifndef PS3DK_LIBNET_NET_HOLD_H
#define PS3DK_LIBNET_NET_HOLD_H
#include <stdint.h>

/* The front ends, as libnet's legacy.c and init.c implement them. */
int __ps3dk_net_legacy_init(void *pool, uint32_t pool_bytes, int (*load)(void), int (*unload)(void));
int __ps3dk_net_legacy_deinit(void);
int __ps3dk_net_cell_init(void *pool, uint32_t pool_bytes, int32_t flags);
int __ps3dk_net_cell_finalize(void);

/* The profiler's hold. acquire: 0 when the network is up for it (joined or
 * brought up with pool, loading the NET module with load), or the failing
 * call's result (-1 while an eviction runs). evict is called, without libnet's
 * lock, when an app front end must take the network back; it must stop every
 * use of the network and then call release. release: 0 or the raw finalize's
 * result; during an eviction it only drops the hold. */
int __ps3dk_net_prof_acquire(void *pool, uint32_t pool_bytes, int (*load)(void), int (*unload)(void),
                             void (*evict)(void));
int __ps3dk_net_prof_release(void);

/* The 128 KiB pool netInitialize has always used (net_pool.c): the profiler
 * uses the same one by default. */
void *__ps3dk_net_default_pool(uint32_t *bytes);

#endif
