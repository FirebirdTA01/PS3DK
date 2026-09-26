/*
 * unlink.c — POSIX unlink() wrapper for Lv-2 filesystem.
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
#include "librt_path.h"

int
__librt_unlink_r(struct _reent *r, const char *path)
{
	char path_buf[PATH_MAX];

	path = __librt_resolve_path(r, path, path_buf);
	if (!path)
		return -1;
	return lv2errno_r(r, sysLv2FsUnlink(path));
}
