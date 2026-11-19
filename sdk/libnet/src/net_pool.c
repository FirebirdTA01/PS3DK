/* SPDX-License-Identifier: BSD-2-Clause
 * The 128 KiB pool netInitialize brings the network up with (net_hold.h).
 * Its own member, so a program that never uses it does not carry it.
 */
#include "net_hold.h"
static unsigned char pool[128 * 1024] __attribute__((aligned(16)));
void *__ps3dk_net_default_pool(uint32_t *bytes)
{
    *bytes = sizeof(pool);
    return pool;
}
