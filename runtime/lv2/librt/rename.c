/*
 * rename.c — POSIX rename() wrapper for Lv-2 filesystem.
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
__librt_rename_r(struct _reent *r, const char *old, const char *new)
{
	char old_buf[PATH_MAX];
	char new_buf[PATH_MAX];

	old = __librt_resolve_path(r, old, old_buf);
	if (!old)
		return -1;
	new = __librt_resolve_path(r, new, new_buf);
	if (!new)
		return -1;
	return lv2errno_r(r, sysLv2FsRename(old, new));
}
