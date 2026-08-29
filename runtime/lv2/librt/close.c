/*
 * close.c — POSIX close() wrapper for Lv-2 file descriptors.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <sys/reent.h>
#include <sys/lv2errno.h>
#include <sys/file.h>
#include <sys/socket.h>

int __librt_socketclose_r(struct _reent *r, int s);

int
__librt_close_r(struct _reent *r, int fd)
{
	if (fd & SOCKET_FD_MASK)
		return __librt_socketclose_r(r, fd);

	return lv2errno_r(r, sysLv2FsClose(fd));
}
