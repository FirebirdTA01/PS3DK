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
#include <stdint.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/lv2errno.h>
#include <sys/file.h>
#include "librt_path.h"

#undef st_atime
#undef st_mtime
#undef st_ctime

/*
 * Largest value representable in a signed off_t, derived from off_t itself:
 * (2^(sizeof(off_t)*8 - 1)) - 1.  So INT32_MAX when off_t is 32-bit (the PPU
 * ILP32 ABI) and INT64_MAX when it is 64-bit.  LV2's sysFSStat.st_size is
 * uint64_t, so a file whose size exceeds this bound cannot fit st_size.
 * POSIX requires stat()/fstat() to fail with EOVERFLOW in that case rather
 * than truncating silently.
 */
#define ST_SIZE_MAX \
	((off_t)((((uint64_t)1) << (sizeof(off_t) * 8 - 1)) - 1))

/* Returns 0 on success, 1 if st_size overflowed off_t. */
static int
convert_lv2stat(struct stat *dst, sysFSStat *src)
{
	if (src->st_size > (uint64_t)ST_SIZE_MAX)
		return 1;

	memset(dst, 0, sizeof(struct stat));
	dst->st_mode        = src->st_mode;
	dst->st_uid         = src->st_uid;
	dst->st_gid         = src->st_gid;
	dst->st_atim.tv_sec = src->st_atime;
	dst->st_mtim.tv_sec = src->st_mtime;
	dst->st_ctim.tv_sec = src->st_ctime;
	dst->st_size        = (off_t)src->st_size;
	dst->st_blksize     = src->st_blksize;
	return 0;
}

int
__librt_fstat_r(struct _reent *r, int fd, struct stat *st)
{
	s32 ret;
	sysFSStat stat;

	ret = sysLv2FsFStat(fd, &stat);
	if (!ret && st) {
		ret = convert_lv2stat(st, &stat);
		if (ret) {
			r->_errno = EOVERFLOW;
			return -1;
		}
	}

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
	char path_buf[PATH_MAX];

	path = __librt_resolve_path(r, path, path_buf);
	if (!path)
		return -1;
	ret = sysLv2FsStat(path, &stat);
	if (!ret && st) {
		ret = convert_lv2stat(st, &stat);
		if (ret) {
			r->_errno = EOVERFLOW;
			return -1;
		}
	}

	return lv2errno_r(r, ret);
}

int
__librt_stat64_r(struct _reent *r, const char *path, struct stat *st)
{
	return __librt_stat_r(r, path, st);
}
