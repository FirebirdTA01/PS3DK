/*
 * truncate.c — POSIX truncate() / ftruncate() wrappers for Lv-2.
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

int
__librt_truncate_r(struct _reent *r, const char *path, off_t len)
{
	return lv2errno_r(r, sysLv2FsTruncate(path, (u64)len));
}

int
__librt_ftruncate_r(struct _reent *r, int fd, off_t len)
{
	return lv2errno_r(r, sysLv2FsFtruncate(fd, (u64)len));
}
