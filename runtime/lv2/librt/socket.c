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

/*
 * Lv-2's select() takes its own descriptor set, and it is NOT newlib's
 * fd_set.  Lv-2 uses 32 big-endian 32-bit words (1024 bits), indexed as
 * word (s >> 5) & 31, bit (s & 31).  newlib's fd_set is an array of
 * `unsigned long`, which is 32-bit under our default ILP32 hybrid but
 * 64-bit under the lp64 multilib -- so a cast would happen to work on one
 * ABI and silently address the wrong bits on the other.
 *
 * The descriptors differ too: a POSIX socket fd here carries
 * SOCKET_FD_MASK, which Lv-2 knows nothing about.
 *
 * Both reasons mean the sets have to be rebuilt rather than passed through.
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

/*
 * getpeername/getsockname are declared in <sys/socket.h>, but librt.a alone
 * defined neither: a program linking -lrt without -lnet got an undefined
 * reference.  libnet.a has always defined them, through PSL1GHT's
 * netGetPeerName/netGetSockName imports, but only once netInitialize() has
 * run -- before that they return ENOSYS.
 *
 * These go straight to Lv-2 syscalls 703/704, so they work with neither
 * libnet on the link line nor netInitialize() called.
 *
 * On link order: both libraries define these weakly, and the linker takes
 * the first weak definition it meets.  cmake/ps3-ppu-toolchain.cmake calls
 * link_libraries(rt), which puts -lrt ahead of the user's libraries for a
 * separate constructor-ordering reason -- so in every CMake-built program
 * it is THESE that get bound, not libnet's, whether or not -lnet is on the
 * line.
 *
 * addr_len is in/out, so it takes the same bounce-buffer treatment as
 * accept(): Lv-2 writes through the pointer and we only publish the result
 * once the call has succeeded.
 */
int __attribute__((weak))
getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
	s32 ret;
	socklen_t len;

	if (!name || !namelen) {
		errno = EINVAL;
		return -1;
	}

	len = *namelen;
	ret = sysNetGetPeerName(FD(s), name, &len);
	if (ret < 0)
		return lv2errno(ret);

	*namelen = len;
	return 0;
}

int __attribute__((weak))
getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
	s32 ret;
	socklen_t len;

	if (!name || !namelen) {
		errno = EINVAL;
		return -1;
	}

	len = *namelen;
	ret = sysNetGetSockName(FD(s), name, &len);
	if (ret < 0)
		return lv2errno(ret);

	*namelen = len;
	return 0;
}

/*
 * This replaces a stub that returned ENOSYS.  That stub was not merely
 * dormant: libnet.a defines select() too -- weakly, routed through netSelect
 * -- but our toolchain file links -lrt ahead of the user's libraries, so the
 * linker met librt's weak stub FIRST and bound it.  select() therefore
 * returned ENOSYS in every CMake-built program even when -lnet was on the
 * line and libnet was initialised.  Lv-2 syscall 716 is the implementation.
 *
 * The descriptor sets are rebuilt rather than cast -- see the lv2_fd_set
 * notes at the top of this file for why a cast is wrong on the lp64
 * multilib and wrong for masked socket descriptors on both.
 */
int __attribute__((weak))
select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
       struct timeval *timeout)
{
	lv2_fd_set lv2_read, lv2_write, lv2_except;
	lv2_timeval lv2_timeout, *lv2_timeoutp = NULL;
	int lv2_nfds = 0, n;
	s32 ret;

	if (nfds < 0) {
		errno = EINVAL;
		return -1;
	}

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
		lv2_timeout.tv_sec = (s64)timeout->tv_sec;
		lv2_timeout.tv_usec = (s64)timeout->tv_usec;
		lv2_timeoutp = &lv2_timeout;
	}

	ret = sysNetSelect(lv2_nfds, &lv2_read, &lv2_write, &lv2_except,
			   lv2_timeoutp);
	if (ret < 0)
		return lv2errno(ret);

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
