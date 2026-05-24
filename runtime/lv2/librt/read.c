/*
 * read.c — POSIX read() wrapper for Lv-2 file descriptors.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <_ansi.h>
#include <errno.h>
#include <sys/reent.h>
#include <sys/types.h>
#include <sys/lv2errno.h>
#include <sys/tty.h>
#include <sys/file.h>

_ssize_t
__librt_read_r(struct _reent *r, int fd, void *ptr, size_t len)
{
	u32 nread;
	s32 ret;

	if (fd == 0) {
		ret = sysTtyRead(fd, ptr, (u32)len, &nread);
	} else {
		u64 nread64;
		ret = sysLv2FsRead(fd, ptr, len, &nread64);
		nread = (u32)nread64;
	}

	return lv2errno_r(r, ret) < 0 ? (_ssize_t)-1 : (_ssize_t)nread;
}
