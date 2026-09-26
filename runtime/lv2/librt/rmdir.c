/*
 * rmdir.c — POSIX rmdir() wrapper for Lv-2 filesystem.
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
__librt_rmdir_r(struct _reent *r, const char *dirname)
{
	char dirname_buf[PATH_MAX];

	dirname = __librt_resolve_path(r, dirname, dirname_buf);
	if (!dirname)
		return -1;
	return lv2errno_r(r, sysLv2FsRmdir(dirname));
}
