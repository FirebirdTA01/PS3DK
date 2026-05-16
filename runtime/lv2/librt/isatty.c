/*
 * isatty.c — POSIX isatty() for Lv-2 file descriptors.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <sys/reent.h>

int
__librt_isatty_r(struct _reent *r, int fd)
{
	if (fd >= 0 && fd <= 2)
		return 1;

	r->_errno = ENOTTY;
	return 0;
}
