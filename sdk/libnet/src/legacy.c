/* SPDX-License-Identifier: BSD-2-Clause
 * Explicit raw-PRX compatibility API. Tagged BSD descriptors are rejected.
 */
#include <pthread.h>
#include <net/net.h>
#include <cell/sysmodule.h>
#include <limits.h>
#include <stdint.h>
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned init_users;
static unsigned char init_memory[128 * 1024] __attribute__((aligned(16)));
int *netErrnoLoc(void) { return _sys_net_errno_loc(); }
int *netHErrnoLoc(void) { return _sys_net_h_errno_loc(); }
static int raw_fd_ok(int fd)
{
    if (fd >= 0 && !(fd & SOCKET_FD_MASK)) return 1;
    int *e = _sys_net_errno_loc(); if (e) *e = 9;
    return 0;
}
int netInitialize(void)
{
    int rc = pthread_mutex_lock(&init_lock);
    if (rc) return -1;
    if (init_users) rc = 0;
    else {
        sys_net_initialize_parameter_t p = {init_memory, sizeof(init_memory), 0};
        rc = cellSysmoduleLoadModule(CELL_SYSMODULE_NET);
        if (!rc) {
            rc = sys_net_initialize_network_ex(&p);
            if (!rc) init_users = 1;
            else cellSysmoduleUnloadModule(CELL_SYSMODULE_NET);
        }
    }
    pthread_mutex_unlock(&init_lock); return rc;
}
int netDeinitialize(void)
{
    int rc = pthread_mutex_lock(&init_lock);
    if (rc) return -1;
    if (!init_users) rc = 0;
    else {
        rc = sys_net_finalize_network();
        if (!rc) { init_users = 0; rc = cellSysmoduleUnloadModule(CELL_SYSMODULE_NET); }
    }
    pthread_mutex_unlock(&init_lock); return rc;
}
int netInitializeNetworkEx(netInitParam *p)
{
    if (!p) return sys_net_initialize_network_ex(NULL);
    sys_net_initialize_parameter_t native = {(void *)(uintptr_t)p->memory, p->memory_size, p->flags};
    return sys_net_initialize_network_ex(&native);
}
int netFinalizeNetwork(void) { return sys_net_finalize_network(); }
int netShowIfConfig(void) { return sys_net_show_ifconfig(); }
int netShowNameServer(void) { return sys_net_show_nameserver(); }
int netShowRoute(void) { return sys_net_show_route(); }
extern int __ps3dk_raw_socket(int domain, int type, int protocol);
int netSocket(int domain, int type, int protocol)
{ return __ps3dk_raw_socket(domain, type, protocol); }
extern int __ps3dk_raw_accept(int fd, const struct sockaddr *a, socklen_t *n);
int netAccept(int fd, const struct sockaddr *a, socklen_t *n)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_accept(fd, a, n); }
extern int __ps3dk_raw_bind(int fd, const struct sockaddr *a, socklen_t n);
int netBind(int fd, const struct sockaddr *a, socklen_t n)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_bind(fd, a, n); }
extern int __ps3dk_raw_connect(int fd, const struct sockaddr *a, socklen_t n);
int netConnect(int fd, const struct sockaddr *a, socklen_t n)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_connect(fd, a, n); }
extern int __ps3dk_raw_listen(int fd, int backlog);
int netListen(int fd, int backlog)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_listen(fd, backlog); }
extern int __ps3dk_raw_socketclose(int fd);
int netClose(int fd)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_socketclose(fd); }
extern int __ps3dk_raw_shutdown(int fd, int how);
int netShutdown(int fd, int how)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_shutdown(fd, how); }
extern int32_t __ps3dk_raw_send(int fd, const void *b, size_t n, int f);
ssize_t netSend(int fd, const void *b, size_t n, int f)
{ if (n > INT_MAX) { net_errno = NET_EMSGSIZE; return -1; } if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_send(fd, b, n, f); }
extern int32_t __ps3dk_raw_recv(int fd, void *b, size_t n, int f);
ssize_t netRecv(int fd, void *b, size_t n, int f)
{ if (n > INT_MAX) { net_errno = NET_EMSGSIZE; return -1; } if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_recv(fd, b, n, f); }
extern int32_t __ps3dk_raw_sendto(int fd, const void *b, size_t n, int f, const struct sockaddr *a, socklen_t l);
ssize_t netSendTo(int fd, const void *b, size_t n, int f, const struct sockaddr *a, socklen_t l)
{ if (n > INT_MAX) { net_errno = NET_EMSGSIZE; return -1; } if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_sendto(fd, b, n, f, a, l); }
extern int32_t __ps3dk_raw_recvfrom(int fd, void *b, size_t n, int f, struct sockaddr *a, socklen_t *l);
ssize_t netRecvFrom(int fd, void *b, size_t n, int f, struct sockaddr *a, socklen_t *l)
{ if (n > INT_MAX) { net_errno = NET_EMSGSIZE; return -1; } if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_recvfrom(fd, b, n, f, a, l); }
extern int32_t __ps3dk_raw_sendmsg(int fd, const struct net_msghdr *m, int f);
ssize_t netSendMsg(int fd, const struct net_msghdr *m, int f)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_sendmsg(fd, m, f); }
extern int32_t __ps3dk_raw_recvmsg(int fd, struct net_msghdr *m, int f);
ssize_t netRecvMsg(int fd, struct net_msghdr *m, int f)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_recvmsg(fd, m, f); }
extern int __ps3dk_raw_getsockname(int fd, struct sockaddr *a, socklen_t *n);
int netGetSockName(int fd, struct sockaddr *a, socklen_t *n)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_getsockname(fd, a, n); }
extern int __ps3dk_raw_getpeername(int fd, struct sockaddr *a, socklen_t *n);
int netGetPeerName(int fd, struct sockaddr *a, socklen_t *n)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_getpeername(fd, a, n); }
extern int __ps3dk_raw_getsockopt(int fd, int level, int option, void *value, socklen_t *len);
int netGetSockOpt(int fd, int level, int option, void *value, socklen_t *len)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_getsockopt(fd, level, option, value, len); }
extern int __ps3dk_raw_setsockopt(int fd, int level, int option, const void *value, socklen_t len);
int netSetSockOpt(int fd, int level, int option, const void *value, socklen_t len)
{ if (!raw_fd_ok(fd)) return -1; return __ps3dk_raw_setsockopt(fd, level, option, value, len); }
extern uint32_t __ps3dk_raw_gethostbyname(const char *);
extern uint32_t __ps3dk_raw_gethostbyaddr(const char *, socklen_t, int);
struct net_hostent *netGetHostByName(const char *name)
{ return (void *)(uintptr_t)__ps3dk_raw_gethostbyname(name); }
struct net_hostent *netGetHostByAddr(const char *a, socklen_t n, int t)
{ return (void *)(uintptr_t)__ps3dk_raw_gethostbyaddr(a, n, t); }
extern int __ps3dk_raw_socketpoll(struct pollfd *, nfds_t, int);
int netPoll(struct pollfd *fds, nfds_t n, int timeout)
{
    unsigned i;
    if (!fds && n) { net_errno = 14; return -1; }
    for (i = 0; i < n; ++i) if (fds[i].fd >= 0 && !raw_fd_ok(fds[i].fd)) return -1;
    return __ps3dk_raw_socketpoll(fds, n, timeout);
}
struct raw_timeval { int64_t sec, usec; };
extern int __ps3dk_raw_socketselect(int, fd_set *, fd_set *, fd_set *, struct raw_timeval *);
int netSelect(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
{
    struct raw_timeval wire;
    if (n < 0 || n > FD_SETSIZE) { net_errno = 22; return -1; }
    if (t) { wire.sec = t->tv_sec; wire.usec = t->tv_usec; }
    return __ps3dk_raw_socketselect(n, r, w, e, t ? &wire : NULL);
}

/* Historical exported spellings, including their original capitalization. */
int netAbortResolver(sys_net_thread_id_t t, int f)
{ return sys_net_abort_resolver(t, f); }
extern int __ps3dk_raw_sys_net_abort_socket(int, int);
int netAbortSocket(int s, int f)
{ if (!raw_fd_ok(s)) return -1; return __ps3dk_raw_sys_net_abort_socket(s, f); }
int netCloseDump(int id, int *f)
{ return sys_net_close_dump(id, f); }
int netFreethreadContext(sys_net_thread_id_t t, int f)
{ return sys_net_free_thread_context(t, f); }
int netGetLibNameServer(struct in_addr *a, struct in_addr *b)
{ return sys_net_get_lib_name_server(a, b); }
int netGetNetEmuTestParam(sys_net_test_param_t *p)
{ return sys_net_get_netemu_test_param(p); }
int netGetTestParam(sys_net_test_param_t *p)
{ return sys_net_get_test_param(p); }
int netGetUdpp2pTestparam(sys_net_test_param_t *p)
{ return sys_net_get_udpp2p_test_param(p); }
int netIfCtl(int i, int c, void *p, int n)
{ return sys_net_if_ctl(i, c, p, n); }
int netOpenDump(int n, int f)
{ return sys_net_open_dump(n, f); }
int netReadDump(int i, void *b, int n, int *f)
{ return sys_net_read_dump(i, b, n, f); }
int netSetResolverConfigurations(int a, int b, int c)
{ return sys_net_set_resolver_configurations(a, b, c); }
int netSetTestParam(sys_net_test_param_t *p)
{ return sys_net_set_test_param(p); }
int netSetUdpp2pTestParam(sys_net_test_param_t *p)
{ return sys_net_set_udpp2p_test_param(p); }
int netSetlibNameServer(struct in_addr *a, struct in_addr *b)
{ return sys_net_set_lib_name_server(a, b); }
int netSetnetemutestparam(sys_net_test_param_t *p)
{ return sys_net_set_netemu_test_param(p); }
int netShowNameserver(void)
{ return sys_net_show_nameserver(); }
