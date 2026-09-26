/*
 * mkdir.c — POSIX mkdir() wrapper for Lv-2 filesystem.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <sys/reent.h>
#include <sys/types.h>
#include <sys/lv2errno.h>
#include <sys/file.h>
#include "librt_path.h"

extern mode_t g_umask;

int
__librt_mkdir_r(struct _reent *r, const char *path, mode_t mode)
{
	char path_buf[PATH_MAX];

	path = __librt_resolve_path(r, path, path_buf);
	if (!path)
		return -1;
	return lv2errno_r(r, sysLv2FsMkdir(path, mode & ~g_umask));
}
