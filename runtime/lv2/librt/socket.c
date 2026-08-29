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
#include <sys/reent.h>
#include <sys/lv2_syscall.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <sys/lv2errno.h>

#define FD(socket) ((socket) & ~SOCKET_FD_MASK)

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

int __attribute__((weak))
accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
	s32 ret;
	socklen_t len;
	socklen_t *lenp = (addr && addrlen) ? &len : NULL;

	if (lenp)
		len = *addrlen;

	ret = sysNetAccept(FD(s), addr, lenp);
	if (ret < 0)
		return lv2errno(ret);

	if (lenp)
		*addrlen = len;

	return ret | SOCKET_FD_MASK;
}

int __attribute__((weak))
bind(int s, const struct sockaddr *addr, socklen_t addrlen)
{
	return lv2errno(sysNetBind(FD(s), addr, addrlen));
}

int __attribute__((weak))
connect(int s, const struct sockaddr *addr, socklen_t addrlen)
{
	return lv2errno(sysNetConnect(FD(s), addr, addrlen));
}

int __attribute__((weak))
listen(int s, int backlog)
{
	return lv2errno(sysNetListen(FD(s), backlog));
}

int __attribute__((weak))
socket(int domain, int type, int protocol)
{
	s32 ret = sysNetSocket(domain, type, protocol);
	if (ret < 0)
		return lv2errno(ret);

	return ret | SOCKET_FD_MASK;
}

ssize_t __attribute__((weak))
send(int s, const void *buf, size_t len, int flags)
{
	return (ssize_t)lv2errno(sysNetSendto(FD(s), buf, len, flags, NULL, 0));
}

ssize_t __attribute__((weak))
sendto(int s, const void *buf, size_t len, int flags,
       const struct sockaddr *addr, socklen_t addrlen)
{
	return (ssize_t)lv2errno(sysNetSendto(FD(s), buf, len, flags, addr,
	                                      addrlen));
}

ssize_t __attribute__((weak))
recv(int s, void *buf, size_t len, int flags)
{
	return (ssize_t)lv2errno(sysNetRecvfrom(FD(s), buf, len, flags, NULL,
	                                        NULL));
}

ssize_t __attribute__((weak))
recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from,
         socklen_t *fromlen)
{
	s32 ret;
	socklen_t len_out;
	socklen_t *lenp = NULL;

	if (from && fromlen) {
		len_out = *fromlen;
		lenp = &len_out;
	}

	ret = sysNetRecvfrom(FD(s), buf, len, flags, from, lenp);
	if (ret < 0)
		return (ssize_t)lv2errno(ret);

	if (lenp)
		*fromlen = len_out;

	return (ssize_t)ret;
}

int __attribute__((weak))
shutdown(int s, int how)
{
	return lv2errno(sysNetShutdown(FD(s), how));
}

int __attribute__((weak))
socketclose(int s)
{
	return lv2errno(sysNetClose(FD(s)));
}

int __attribute__((weak))
closesocket(int s)
{
	return socketclose(s);
}

int
__librt_socketclose_r(struct _reent *r, int s)
{
	return lv2errno_r(r, sysNetClose(FD(s)));
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

int __attribute__((weak))
select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
       struct timeval *timeout)
{
	(void)nfds;
	(void)readfds;
	(void)writefds;
	(void)exceptfds;
	(void)timeout;
	errno = ENOSYS;
	return -1;
}
