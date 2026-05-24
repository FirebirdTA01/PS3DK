/*
 * write.c — POSIX write() wrapper for Lv-2 file descriptors.
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
__librt_write_r(struct _reent *r, int fd, const void *ptr, size_t len)
{
	u32 nwritten;
	s32 ret;

	if (fd == 1 || fd == 2) {
		ret = sysTtyWrite(fd, ptr, (u32)len, &nwritten);
	} else {
		u64 nwritten64;
		ret = sysLv2FsWrite(fd, ptr, len, &nwritten64);
		nwritten = (u32)nwritten64;
	}

	return lv2errno_r(r, ret) < 0 ? (_ssize_t)-1 : (_ssize_t)nwritten;
}
