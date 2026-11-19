/* SPDX-License-Identifier: BSD-2-Clause */
#include <cell/libnet.h>
#include <errno.h>
#include <stdint.h>
#include "net_hold.h"
static int pointer_fits_ea(const void *p)
{ return (uintptr_t)p <= UINT32_MAX; }
int sys_net_initialize_network_ex(sys_net_initialize_parameter_t *p)
{
    if (!p || !pointer_fits_ea(p->memory)) { errno = EFAULT; return -1; }
    return __ps3dk_net_cell_init((void *)p->memory, p->memory_size, p->flags);
}
int sys_net_finalize_network(void)
{
    return __ps3dk_net_cell_finalize();
}
