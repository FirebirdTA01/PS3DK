/* SPDX-License-Identifier: BSD-2-Clause
 * These sys_net controls retain PRX return/error semantics. Accept tagged BSD
 * descriptors as well as the raw/sentinel values accepted by firmware.
 */
#include <net/net.h>
extern int __ps3dk_raw_sys_net_abort_socket(int, int);
extern int __ps3dk_raw_sys_net_get_sockinfo(int, void *, int);
extern int __ps3dk_raw_sys_net_get_sockinfo_ex(int, void *, int, int);
static int raw_descriptor(int s) { return s >= 0 ? s & ~SOCKET_FD_MASK : s; }
int sys_net_abort_socket(int s, int flags)
{ return __ps3dk_raw_sys_net_abort_socket(raw_descriptor(s), flags); }
int sys_net_get_sockinfo(int s, sys_net_sockinfo_t *p, int n)
{
    int i, result = __ps3dk_raw_sys_net_get_sockinfo(raw_descriptor(s), p, n);
    for (i = 0; p && i < result && i < n; ++i)
        if (p[i].s >= 0) p[i].s |= SOCKET_FD_MASK;
    return result;
}
int sys_net_get_sockinfo_ex(int s, sys_net_sockinfo_ex_t *p, int n, int flags)
{
    int i, result = __ps3dk_raw_sys_net_get_sockinfo_ex(raw_descriptor(s), p, n, flags);
    for (i = 0; p && i < result && i < n; ++i)
        if (p[i].s >= 0) p[i].s |= SOCKET_FD_MASK;
    return result;
}
static int reject_tagged(int s)
{
    if (s >= 0 && (s & SOCKET_FD_MASK)) { net_errno = NET_EBADF; return 1; }
    return 0;
}
int netGetSockInfo(int s, netSocketInfo *p, int n)
{
    if (reject_tagged(s)) return -1;
    return __ps3dk_raw_sys_net_get_sockinfo(s, p, n);
}
int netGetSockInfoEx(int s, netSocketInfoEx *p, int n, int flags)
{
    if (reject_tagged(s)) return -1;
    return __ps3dk_raw_sys_net_get_sockinfo_ex(s, p, n, flags);
}
