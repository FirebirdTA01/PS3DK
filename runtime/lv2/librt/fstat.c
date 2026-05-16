/*
 * fstat.c — POSIX stat() / fstat() wrappers for Lv-2 filesystem.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <string.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/lv2errno.h>
#include <sys/file.h>

#undef st_atime
#undef st_mtime
#undef st_ctime

static void
convert_lv2stat(struct stat *dst, sysFSStat *src)
{
	memset(dst, 0, sizeof(struct stat));
	dst->st_mode        = src->st_mode;
	dst->st_uid         = src->st_uid;
	dst->st_gid         = src->st_gid;
	dst->st_atim.tv_sec = src->st_atime;
	dst->st_mtim.tv_sec = src->st_mtime;
	dst->st_ctim.tv_sec = src->st_ctime;
	dst->st_size        = src->st_size;
	dst->st_blksize     = src->st_blksize;
}

int
__librt_fstat_r(struct _reent *r, int fd, struct stat *st)
{
	s32 ret;
	sysFSStat stat;

	ret = sysLv2FsFStat(fd, &stat);
	if (!ret && st)
		convert_lv2stat(st, &stat);

	return lv2errno_r(r, ret);
}

int
__librt_fstat64_r(struct _reent *r, int fd, struct stat *st)
{
	return __librt_fstat_r(r, fd, st);
}

int
__librt_stat_r(struct _reent *r, const char *path, struct stat *st)
{
	s32 ret;
	sysFSStat stat;

	ret = sysLv2FsStat(path, &stat);
	if (!ret && st)
		convert_lv2stat(st, &stat);

	return lv2errno_r(r, ret);
}

int
__librt_stat64_r(struct _reent *r, const char *path, struct stat *st)
{
	return __librt_stat_r(r, path, st);
}
