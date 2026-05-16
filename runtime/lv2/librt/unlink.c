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

int
__librt_unlink_r(struct _reent *r, const char *path)
{
	return lv2errno_r(r, sysLv2FsUnlink(path));
}
