/*
 * lseek.c — POSIX lseek() / lseek64() wrappers for Lv-2 file descriptors.
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

_off_t
__librt_lseek_r(struct _reent *r, int fd, _off_t pos, int dir)
{
	u64 result;
	s32 ret = sysLv2FsLSeek64(fd, (u64)pos, dir, &result);
	return lv2errno_r(r, ret) < 0 ? (_off_t)-1 : (_off_t)result;
}

_off64_t
__librt_lseek64_r(struct _reent *r, int fd, _off64_t pos, int dir)
{
	u64 result;
	s32 ret = sysLv2FsLSeek64(fd, (u64)pos, dir, &result);
	return lv2errno_r(r, ret) < 0 ? (_off64_t)-1 : (_off64_t)result;
}
