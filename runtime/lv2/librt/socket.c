/*
 * socket.c — Weak POSIX socket wrappers for Lv-2 sockets.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <poll.h>
#include <sys/reent.h>
#include <sys/lv2_syscall.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include "net_errno.h"
#include "net_wire.h"

#define FD(socket) ((socket) & ~SOCKET_FD_MASK)
static int valid_socket(int s)
{ return s >= 0 && (s & SOCKET_FD_MASK) && FD(s) < 1024; }

/* SDK fd_set and Lv-2 both use 32-bit words in either ABI. Rebuild only
 * the requested range, normalizing tagged descriptors and clearing padding.
 */
#define LV2_FD_WORDS   32
#define LV2_FD_SETSIZE (LV2_FD_WORDS * 32)

typedef struct {
	u32 bits[LV2_FD_WORDS];
} lv2_fd_set;

/*
 * Lv-2's timeval is two 64-bit fields.  newlib's is NOT the same shape on
 * our default ABI: under the ILP32 hybrid, sizeof(struct timeval) is 16 and
 * tv_sec is 8 bytes, but tv_usec is only 4, followed by 4 bytes of padding.
 * Reading 8 big-endian bytes at that offset therefore yields
 * (tv_usec << 32) | padding -- a 500us timeout becomes roughly 2e12us, i.e.
 * "never".  Under lp64 the two layouts happen to agree, so passing the
 * caller's struct straight through would work on the multilib nobody builds
 * with and be silently wrong on the one every sample uses.
 *
 * This is the same class as the gettod/settod width bugs the librt audit
 * found; the fix is to convert explicitly rather than to rely on a layout
 * coincidence.
 */
typedef struct {
	s64 tv_sec;
	s64 tv_usec;
} lv2_timeval;

static void
lv2_fd_zero(lv2_fd_set *set)
{
	int i;

	for (i = 0; i < LV2_FD_WORDS; i++)
		set->bits[i] = 0;
}

static void
lv2_fd_set_bit(lv2_fd_set *set, int s)
{
	set->bits[(s >> 5) & 31] |= (u32)1 << (s & 31);
}

static int
lv2_fd_is_set(const lv2_fd_set *set, int s)
{
	return (set->bits[(s >> 5) & 31] >> (s & 31)) & 1u;
}

/*
 * Translate a POSIX fd_set into Lv-2's, returning the highest Lv-2
 * descriptor seen plus one (select()'s nfds is expressed in Lv-2 numbering,
 * not in POSIX fd numbering).  A NULL `from' yields an empty set.
 */
static int
lv2_fd_from_posix(lv2_fd_set *to, const fd_set *from, int nfds)
{
	int fd, lv2fd, maxfd = 0;

	lv2_fd_zero(to);
	if (!from)
		return 0;

	for (fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
		if (!FD_ISSET(fd, from))
			continue;

		lv2fd = FD(fd);
		if (lv2fd < 0 || lv2fd >= LV2_FD_SETSIZE) {
			/* Outside what Lv-2 can express; report it rather
			 * than silently dropping the descriptor. */
			return -1;
		}

		lv2_fd_set_bit(to, lv2fd);
		if (lv2fd + 1 > maxfd)
			maxfd = lv2fd + 1;
	}

	return maxfd;
}

/*
 * Fold the Lv-2 result back onto the caller's set, clearing every
 * descriptor Lv-2 did not report as ready.
 */
static void
lv2_fd_to_posix(fd_set *to, const lv2_fd_set *from, int nfds)
{
	int fd, lv2fd;

	if (!to)
		return;

	for (fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
		if (!FD_ISSET(fd, to))
			continue;

		lv2fd = FD(fd);
		if (lv2fd < 0 || lv2fd >= LV2_FD_SETSIZE ||
		    !lv2_fd_is_set(from, lv2fd))
			FD_CLR(fd, to);
	}
}

LV2_SYSCALL
sysNetAccept(int socket, const struct sockaddr *addr, socklen_t *addr_len)
{
	lv2syscall3(700, socket, (u64)addr, (u64)addr_len);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetBind(int socket, const struct sockaddr *addr, socklen_t addr_len)
{
	lv2syscall3(701, socket, (u64)addr, addr_len);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetConnect(int socket, const struct sockaddr *addr, socklen_t addr_len)
{
	lv2syscall3(702, socket, (u64)addr, addr_len);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetListen(int socket, int backlog)
{
	lv2syscall2(706, socket, backlog);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetRecvfrom(int socket, void *buffer, size_t len, int flags,
               const struct sockaddr *addr, socklen_t *addr_len)
{
	lv2syscall6(707, socket, (u64)buffer, len, flags, (u64)addr,
	            (u64)addr_len);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetSendto(int socket, const void *message, size_t len, int flags,
             const struct sockaddr *addr, socklen_t addr_len)
{
	lv2syscall6(710, socket, (u64)message, len, flags, (u64)addr, addr_len);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetShutdown(int socket, int how)
{
	lv2syscall2(712, socket, how);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetSocket(int domain, int type, int protocol)
{
	lv2syscall3(713, domain, type, protocol);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetClose(int socket)
{
	lv2syscall1(714, socket);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetGetPeerName(int socket, struct sockaddr *addr, socklen_t *addr_len)
{
	lv2syscall3(703, socket, (u64)addr, (u64)addr_len);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetGetSockName(int socket, struct sockaddr *addr, socklen_t *addr_len)
{
	lv2syscall3(704, socket, (u64)addr, (u64)addr_len);
	return_to_user_prog(s32);
}

LV2_SYSCALL
sysNetSelect(int nfds, lv2_fd_set *readfds, lv2_fd_set *writefds,
	     lv2_fd_set *exceptfds, lv2_timeval *timeout)
{
	lv2syscall5(716, nfds, (u64)readfds, (u64)writefds, (u64)exceptfds,
		    (u64)timeout);
	return_to_user_prog(s32);
}

int __attribute__((weak))
accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    if (addr && !addrlen) { errno = EFAULT; return -1; }
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	s32 ret;
	socklen_t len;
	socklen_t *lenp = (addr && addrlen) ? &len : NULL;

	if (lenp)
		len = *addrlen;

	ret = sysNetAccept(FD(s), addr, lenp);
	if (ret < 0)
		return net_result(ret);

	if (lenp)
		*addrlen = len;

	return ret | SOCKET_FD_MASK;
}

int __attribute__((weak))
bind(int s, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return net_result(sysNetBind(FD(s), addr, addrlen));
}

int __attribute__((weak))
connect(int s, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return net_result(sysNetConnect(FD(s), addr, addrlen));
}

int __attribute__((weak))
listen(int s, int backlog)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return net_result(sysNetListen(FD(s), backlog));
}

int __attribute__((weak))
socket(int domain, int type, int protocol)
{
	s32 ret = sysNetSocket(domain, type, protocol);
	if (ret < 0)
		return net_result(ret);

	return ret | SOCKET_FD_MASK;
}

ssize_t __attribute__((weak))
send(int s, const void *buf, size_t len, int flags)
{
    if (len > INT_MAX) { errno = EMSGSIZE; return -1; }
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return (ssize_t)net_result(sysNetSendto(FD(s), buf, len, flags, NULL, 0));
}

ssize_t __attribute__((weak))
sendto(int s, const void *buf, size_t len, int flags,
       const struct sockaddr *addr, socklen_t addrlen)
{
    if (len > INT_MAX) { errno = EMSGSIZE; return -1; }
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return (ssize_t)net_result(sysNetSendto(FD(s), buf, len, flags, addr,
	                                      addrlen));
}

ssize_t __attribute__((weak))
recv(int s, void *buf, size_t len, int flags)
{
    if (len > INT_MAX) { errno = EMSGSIZE; return -1; }
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return (ssize_t)net_result(sysNetRecvfrom(FD(s), buf, len, flags, NULL,
	                                        NULL));
}

ssize_t __attribute__((weak))
recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from,
         socklen_t *fromlen)
{
    if (from && !fromlen) { errno = EFAULT; return -1; }
    if (len > INT_MAX) { errno = EMSGSIZE; return -1; }
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	s32 ret;
	socklen_t len_out;
	socklen_t *lenp = NULL;

	if (from && fromlen) {
		len_out = *fromlen;
		lenp = &len_out;
	}

	ret = sysNetRecvfrom(FD(s), buf, len, flags, from, lenp);
	if (ret < 0)
		return (ssize_t)net_result(ret);

	if (lenp)
		*fromlen = len_out;

	return (ssize_t)ret;
}

int __attribute__((weak))
shutdown(int s, int how)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return net_result(sysNetShutdown(FD(s), how));
}

int __attribute__((weak))
socketclose(int s)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	return net_result(sysNetClose(FD(s)));
}

int __attribute__((weak))
closesocket(int s)
{
	return socketclose(s);
}

int
__librt_socketclose_r(struct _reent *r, int s)
{
	if (!valid_socket(s)) { r->_errno = EBADF; return -1; }
	return net_result_r(r, sysNetClose(FD(s)));
}

int __attribute__((weak))
inet_aton(const char *cp, struct in_addr *inp)
{
	unsigned int a, b, c, d;

	if (!cp || !inp || sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return 0;
	if ((a | b | c | d) & 0xffffff00u)
		return 0;

	inp->s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
	return 1;
}

int __attribute__((weak))
inet_pton(int af, const char *src, void *dst)
{
	if (af == AF_INET)
		return inet_aton(src, (struct in_addr *)dst);
	if (af == AF_INET6)
		return 0;

	errno = EAFNOSUPPORT;
	return -1;
}

/* Length outputs are only published after a successful syscall. */
int __attribute__((weak))
getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	s32 ret;
	socklen_t len;

	if (!name || !namelen) {
		errno = EINVAL;
		return -1;
	}

	len = *namelen;
	ret = sysNetGetPeerName(FD(s), name, &len);
	if (ret < 0)
		return net_result(ret);

	*namelen = len;
	return 0;
}

int __attribute__((weak))
getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
	s32 ret;
	socklen_t len;

	if (!name || !namelen) {
		errno = EINVAL;
		return -1;
	}

	len = *namelen;
	ret = sysNetGetSockName(FD(s), name, &len);
	if (ret < 0)
		return net_result(ret);

	*namelen = len;
	return 0;
}

/* Translate tagged nfds and native timeval at the kernel boundary. */
int __attribute__((weak))
select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
       struct timeval *timeout)
{
	lv2_fd_set lv2_read, lv2_write, lv2_except;
	lv2_timeval lv2_timeout, *lv2_timeoutp = NULL;
	int lv2_nfds = 0, n;
	s32 ret;

	if (nfds < 0 || (nfds & ~SOCKET_FD_MASK) > FD_SETSIZE) {
		errno = EINVAL;
		return -1;
	}

	nfds &= ~SOCKET_FD_MASK;
	n = lv2_fd_from_posix(&lv2_read, readfds, nfds);
	if (n < 0)
		goto too_large;
	if (n > lv2_nfds)
		lv2_nfds = n;

	n = lv2_fd_from_posix(&lv2_write, writefds, nfds);
	if (n < 0)
		goto too_large;
	if (n > lv2_nfds)
		lv2_nfds = n;

	n = lv2_fd_from_posix(&lv2_except, exceptfds, nfds);
	if (n < 0)
		goto too_large;
	if (n > lv2_nfds)
		lv2_nfds = n;

	/* NULL means block indefinitely; only a supplied timeout is
	 * converted. */
	if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000) {
            errno = EINVAL; return -1;
        }
		lv2_timeout.tv_sec = (s64)timeout->tv_sec;
		lv2_timeout.tv_usec = (s64)timeout->tv_usec;
		lv2_timeoutp = &lv2_timeout;
	}

	ret = sysNetSelect(lv2_nfds, &lv2_read, &lv2_write, &lv2_except,
			   lv2_timeoutp);
	if (ret < 0)
		return net_result(ret);

	lv2_fd_to_posix(readfds, &lv2_read, nfds);
	lv2_fd_to_posix(writefds, &lv2_write, nfds);
	lv2_fd_to_posix(exceptfds, &lv2_except, nfds);

	return ret;

too_large:
	/* A descriptor outside Lv-2's 1024-bit set: EINVAL is what POSIX
	 * specifies for an out-of-range nfds, and silently ignoring the
	 * descriptor would be worse. */
	errno = EINVAL;
	return -1;
}

int __attribute__((weak)) socketselect(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
{ return select(nfds, r, w, e, t); }


LV2_SYSCALL sysNetGetSockOpt(int s, int level, int option, void *value, socklen_t *len)
{
    lv2syscall5(705, s, level, option, (u64)value, (u64)len);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysNetSetSockOpt(int s, int level, int option, const void *value, socklen_t len)
{
    lv2syscall5(711, s, level, option, (u64)value, len);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysNetPoll(struct pollfd *fds, nfds_t n, int timeout)
{
    lv2syscall3(715, (u64)fds, n, timeout);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysNetSendMsg(int s, const struct net_wire_msghdr *msg, int flags)
{
    lv2syscall3(709, s, (u64)msg, flags);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysNetRecvMsg(int s, struct net_wire_msghdr *msg, int flags)
{
    lv2syscall3(708, s, (u64)msg, flags);
    return_to_user_prog(s32);
}
static int timeout_option(int level, int option)
{ return level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO); }
static int valid_timeout(const struct timeval *t)
{ return t->tv_sec >= 0 && t->tv_usec >= 0 && t->tv_usec < 1000000; }

int __attribute__((weak)) setsockopt(int s, int level, int option, const void *value, socklen_t len)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
    lv2_timeval wire;
    if (!value) { errno = EFAULT; return -1; }
    if (timeout_option(level, option)) {
        struct timeval native = {0};
        if (len != sizeof(native)) { errno = EINVAL; return -1; }
        memcpy(&native, value, sizeof(native));
        if (!valid_timeout(&native)) { errno = EINVAL; return -1; }
        wire.tv_sec = native.tv_sec; wire.tv_usec = native.tv_usec;
        value = &wire; len = sizeof(wire);
    }
    return net_result(sysNetSetSockOpt(FD(s), level, option, value, len));
}

int __attribute__((weak)) getsockopt(int s, int level, int option, void *value, socklen_t *len)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
    lv2_timeval wire = {0, 0};
    socklen_t wire_len;
    int error = 0, ret;
    void *out = value;
    if (!value || !len) { errno = EFAULT; return -1; }
    wire_len = *len;
    if (timeout_option(level, option)) {
        if (*len < sizeof(struct timeval)) { errno = EINVAL; return -1; }
        out = &wire; wire_len = sizeof(wire);
    } else if (level == SOL_SOCKET && option == SO_ERROR) {
        if (*len < sizeof(error)) { errno = EINVAL; return -1; }
        out = &error; wire_len = sizeof(error);
    }
    ret = net_result(sysNetGetSockOpt(FD(s), level, option, out, &wire_len));
    if (ret < 0) return -1;
    if (timeout_option(level, option)) {
        struct timeval native = {0};
        native.tv_sec = wire.tv_sec; native.tv_usec = wire.tv_usec;
        memcpy(value, &native, sizeof(native));
        *len = sizeof(native);
    } else if (out == &error) {
        error = net_error(error);
        memcpy(value, &error, sizeof(error)); *len = sizeof(error);
    } else *len = wire_len;
    return ret;
}

int __attribute__((weak)) poll(struct pollfd *fds, nfds_t n, int timeout)
{
    struct pollfd *wire;
    unsigned i;
    int ret, invalid = 0;
    if (n > 1024) { errno = EINVAL; return -1; }
    if (n && !fds) { errno = EFAULT; return -1; }
    wire = n ? malloc(n * sizeof(*wire)) : NULL;
    if (n && !wire) { errno = ENOMEM; return -1; }
    for (i = 0; i < n; ++i) {
        wire[i] = fds[i]; wire[i].revents = 0;
        if (fds[i].fd < 0) wire[i].fd = -1;
        else if (!(fds[i].fd & SOCKET_FD_MASK)) { wire[i].fd = -1; ++invalid; }
        else wire[i].fd = FD(fds[i].fd);
    }
    ret = net_result(sysNetPoll(wire, n, invalid ? 0 : timeout));
    if (ret >= 0) {
        for (i = 0; i < n; ++i)
            fds[i].revents = fds[i].fd < 0 ? 0 :
                !(fds[i].fd & SOCKET_FD_MASK) ? POLLNVAL : wire[i].revents;
        ret += invalid;
    }
    free(wire);
    return ret;
}
int __attribute__((weak)) socketpoll(struct pollfd *fds, nfds_t n, int timeout)
{ return poll(fds, n, timeout); }

static int pointer_fits_ea(const void *p)
{ return (uintptr_t)p <= UINT32_MAX; }

static int message_to_wire(const struct msghdr *msg, struct net_wire_msghdr *wire,
                           struct net_wire_iovec **vectors)
{
    int i;
    *vectors = NULL;
    if (!msg) { errno = EFAULT; return -1; }
    if (msg->msg_iovlen < 0 || msg->msg_iovlen > 1024) { errno = EMSGSIZE; return -1; }
    if ((msg->msg_iovlen && !msg->msg_iov) ||
        !pointer_fits_ea(msg->msg_name) || !pointer_fits_ea(msg->msg_control)) {
        errno = EFAULT; return -1;
    }
    memset(wire, 0, sizeof(*wire));
    if (msg->msg_iovlen) {
        *vectors = calloc((unsigned)msg->msg_iovlen, sizeof(**vectors));
        if (!*vectors) { errno = ENOMEM; return -1; }
        if (!pointer_fits_ea(*vectors)) goto bad_address;
    }
    for (i = 0; i < msg->msg_iovlen; ++i) {
        if (!pointer_fits_ea(msg->msg_iov[i].iov_base)) goto bad_address;
        (*vectors)[i].base = (uint32_t)(uintptr_t)msg->msg_iov[i].iov_base;
        (*vectors)[i].len = msg->msg_iov[i].iov_len;
    }
    wire->name = (uint32_t)(uintptr_t)msg->msg_name;
    wire->namelen = msg->msg_namelen;
    wire->iov = (uint32_t)(uintptr_t)*vectors; wire->iovlen = msg->msg_iovlen;
    wire->control = (uint32_t)(uintptr_t)msg->msg_control;
    wire->controllen = msg->msg_controllen;
    return 0;
bad_address:
    free(*vectors); *vectors = NULL; errno = EFAULT; return -1;
}
ssize_t __attribute__((weak)) sendmsg(int s, const struct msghdr *msg, int flags)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
    struct net_wire_msghdr wire;
    struct net_wire_iovec *vectors;
    int ret;
    if (message_to_wire(msg, &wire, &vectors) < 0) return -1;
    ret = net_result(sysNetSendMsg(FD(s), &wire, flags));
    free(vectors); return ret;
}
ssize_t __attribute__((weak)) recvmsg(int s, struct msghdr *msg, int flags)
{
    if (!valid_socket(s)) { errno = EBADF; return -1; }
    struct net_wire_msghdr wire;
    struct net_wire_iovec *vectors;
    int ret;
    if (message_to_wire(msg, &wire, &vectors) < 0) return -1;
    ret = net_result(sysNetRecvMsg(FD(s), &wire, flags));
    free(vectors);
    if (ret >= 0) {
        msg->msg_namelen = wire.namelen; msg->msg_controllen = wire.controllen;
        msg->msg_flags = wire.flags;
    }
    return ret;
}

ssize_t __librt_recv_r(struct _reent *r, int s, void *buf, size_t len)
{
    if (!valid_socket(s)) { r->_errno = EBADF; return -1; }
    if (len > INT_MAX) { r->_errno = EMSGSIZE; return -1; }
    return net_result_r(r, sysNetRecvfrom(FD(s), buf, len, 0, NULL, NULL));
}
ssize_t __librt_send_r(struct _reent *r, int s, const void *buf, size_t len)
{
    if (!valid_socket(s)) { r->_errno = EBADF; return -1; }
    if (len > INT_MAX) { r->_errno = EMSGSIZE; return -1; }
    return net_result_r(r, sysNetSendto(FD(s), buf, len, 0, NULL, 0));
}
