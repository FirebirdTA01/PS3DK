/*
 * socket.c — Weak-alias socket stubs that return ENOSYS.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <sys/reent.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>

#define STUB(name, rettype, ...) \
	rettype __attribute__((weak)) name(__VA_ARGS__) \
	{ errno = ENOSYS; return (rettype)-1; }

STUB(accept,      int, int s, struct sockaddr *a, socklen_t *l)
STUB(bind,        int, int s, const struct sockaddr *a, socklen_t l)
STUB(connect,     int, int s, const struct sockaddr *a, socklen_t l)
STUB(listen,      int, int s, int backlog)
STUB(socket,      int, int d, int t, int p)
STUB(send,        ssize_t, int s, const void *b, size_t l, int f)
STUB(sendto,      ssize_t, int s, const void *b, size_t l, int f,
     const struct sockaddr *d, socklen_t dl)
STUB(recv,        ssize_t, int s, void *b, size_t l, int f)
STUB(recvfrom,    ssize_t, int s, void *b, size_t l, int f,
     struct sockaddr *fr, socklen_t *fl)
STUB(shutdown,    int, int s, int how)
STUB(closesocket, int, int s)
STUB(inet_aton,   int, const char *cp, struct in_addr *inp)
STUB(inet_pton,   int, int af, const char *src, void *dst)
STUB(select,      int, int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
