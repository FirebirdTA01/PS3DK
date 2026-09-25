/* SPDX-License-Identifier: BSD-2-Clause */
#include <cell/libnet.h>
#include <errno.h>
#include <stdint.h>
struct wire_init { uint32_t memory, size; int32_t flags; };
_Static_assert(sizeof(struct wire_init) == 12, "firmware init parameters");
extern int __ps3dk_raw_sys_net_initialize_network_ex(const struct wire_init *);
static int pointer_fits_ea(const void *p)
{ return (uintptr_t)p <= UINT32_MAX; }
int sys_net_initialize_network_ex(sys_net_initialize_parameter_t *p)
{
    struct wire_init wire;
    if (!p || !pointer_fits_ea(p->memory)) { errno = EFAULT; return -1; }
    wire.memory = (uint32_t)(uintptr_t)(void *)p->memory; wire.size = p->memory_size; wire.flags = p->flags;
    return __ps3dk_raw_sys_net_initialize_network_ex(&wire);
}
