/* SPDX-License-Identifier: BSD-2-Clause
 * Process-wide network holders (net_hold.c). Internal to the SDK: libnet's
 * own front ends and the profiler's live agent use it; applications call
 * netInitialize / sys_net_initialize_network_ex as before.
 *
 * Every user of the network is a holder bit. The first acquire with no
 * holder brings the network up (optionally loading the NET sysmodule through
 * the caller's functions, then the raw sys_net_initialize_network_ex with the
 * caller's pool); a later acquire by another holder only records its bit.
 * A release clears its own bit; the raw sys_net_finalize_network (and the
 * unload of a module this layer loaded) happens only when the last holder
 * releases. With one holder the raw calls, their order and their results are
 * exactly those of the front end before holders existed.
 *
 * The CELL holder keeps the raw semantics of repeated calls: a second
 * sys_net_initialize_network_ex while CELL holds, or a finalize with no
 * holder at all, goes to the raw call as before.
 */
#ifndef PS3DK_LIBNET_NET_HOLD_H
#define PS3DK_LIBNET_NET_HOLD_H
#include <stdint.h>

#define PS3DK_NET_HOLD_LEGACY 1u /* netInitialize / netDeinitialize */
#define PS3DK_NET_HOLD_CELL 2u   /* sys_net_initialize_network_ex / sys_net_finalize_network */
#define PS3DK_NET_HOLD_PROF 4u   /* the profiler's live agent (libps3dkprof live_os_lv2.c) */

/* 0 on success, or the failing call's result (-1 when the holder lock cannot
 * be taken; -1 with errno EFAULT for a pool above 4 GiB). load and unload may
 * be NULL (no module handling); they are used only when this acquire is the
 * one that brings the network up, and unload is then kept for the final
 * release. */
int __ps3dk_net_hold_acquire(unsigned holder, void *pool, uint32_t pool_bytes, int32_t flags,
                             int (*load)(void), int (*unload)(void));

/* 0, or the failing raw call's result; see the front ends for the value each
 * returns when it is the last holder. */
int __ps3dk_net_hold_release(unsigned holder);

/* The 128 KiB pool netInitialize has always used (net_pool.c): the profiler
 * uses the same one by default, so an app that also calls netInitialize pays
 * for it once. */
void *__ps3dk_net_default_pool(uint32_t *bytes);

#endif
