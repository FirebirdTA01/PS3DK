/*
 * dirent.c — POSIX opendir/readdir/closedir wrappers for Lv-2 filesystem.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/dirent.h>
#include <sys/lv2errno.h>
#include <sys/file.h>
#include <unistd.h>

struct librt_dir {
	DIR dir;
	char path[PATH_MAX];
};

static void
convert_lv2dirent(struct dirent *result, sysFSDirent *source, DIR *dirp)
{
	result->d_reclen  = sizeof(struct dirent);
	result->d_seekoff = dirp->dd_seek;
	result->d_namlen  = source->d_namlen;
	result->d_type    = source->d_type;
	strncpy(result->d_name, source->d_name, MAXPATHLEN + 1);
}

static s32
readdir_i(DIR *dirp, struct dirent *entry, struct dirent **result)
{
	s32 ret;
	u64 read = 0;
	sysFSDirent lv2dir;

	*result = NULL;
	ret = sysLv2FsReadDir(dirp->dd_fd, &lv2dir, &read);
	if (ret < 0)
		return ret;

	if (read) {
		convert_lv2dirent(entry + dirp->dd_loc, &lv2dir, dirp);
		dirp->dd_seek++;
		*result = entry;
	}

	return ret;
}

static int
store_dir_path(struct _reent *r, struct librt_dir *owned, const char *path)
{
	size_t cwd_len;
	size_t path_len;

	if (!path || !*path) {
		r->_errno = EINVAL;
		return -1;
	}

	if (path[0] == '/') {
		if (strlen(path) >= PATH_MAX) {
			r->_errno = ENAMETOOLONG;
			return -1;
		}
		strcpy(owned->path, path);
		return 0;
	}

	if (!getcwd(owned->path, sizeof(owned->path)))
		return -1;

	cwd_len = strlen(owned->path);
	path_len = strlen(path);
	if (cwd_len + (cwd_len > 1 ? 1 : 0) + path_len >= PATH_MAX) {
		r->_errno = ENAMETOOLONG;
		return -1;
	}

	if (cwd_len > 1)
		strcat(owned->path, "/");
	strcat(owned->path, path);
	return 0;
}

DIR *
__librt_opendir_r(struct _reent *r, const char *path)
{
	s32 fd, ret;
	struct librt_dir *owned = (struct librt_dir *)malloc(sizeof(*owned));
	DIR *dirp = owned ? &owned->dir : NULL;
	struct dirent *buffer = (struct dirent *)malloc(sizeof(struct dirent));

	if (!owned || !buffer) {
		free(owned);
		free(buffer);
		r->_errno = ENOMEM;
		return NULL;
	}
	if (store_dir_path(r, owned, path) < 0) {
		free(owned);
		free(buffer);
		return NULL;
	}

	memset(dirp, 0, sizeof(DIR));
	memset(buffer, 0, sizeof(struct dirent));

	dirp->dd_buf = buffer;
	dirp->dd_len = sizeof(struct dirent);

	ret = sysLv2FsOpenDir(owned->path, &fd);
	if (!ret) {
		dirp->dd_fd = fd;
		return dirp;
	}

	free(buffer);
	free(owned);
	lv2errno_r(r, ret);

	return NULL;
}

struct dirent *
__librt_readdir_r(struct _reent *r, DIR *dirp)
{
	s32 ret;
	struct dirent *out = NULL;

	ret = readdir_i(dirp, (struct dirent *)dirp->dd_buf, &out);
	if (ret < 0)
		lv2errno_r(r, ret);

	return out;
}

int
__librt_readdir_r_r(struct _reent *r, DIR *dirp, struct dirent *entry,
                    struct dirent **result)
{
	s32 ret;

	ret = readdir_i(dirp, entry, result);
	return lv2errno_r(r, ret);
}

int
__librt_closedir_r(struct _reent *r, DIR *dirp)
{
	s32 ret;
	struct librt_dir *owned = (struct librt_dir *)dirp;

	ret = sysLv2FsCloseDir(dirp->dd_fd);

	free(dirp->dd_buf);
	free(owned);

	return lv2errno_r(r, ret);
}

long int
__librt_telldir_r(struct _reent *r, DIR *dirp)
{
	return dirp ? dirp->dd_seek : 0;
}

static int
reopen_dir(struct _reent *r, DIR *dirp)
{
	struct librt_dir *owned = (struct librt_dir *)dirp;
	s32 new_fd;
	s32 ret;

	ret = sysLv2FsOpenDir(owned->path, &new_fd);
	if (ret)
		return lv2errno_r(r, ret);

	sysLv2FsCloseDir(dirp->dd_fd);
	dirp->dd_fd = new_fd;
	dirp->dd_seek = 0;
	dirp->dd_loc = 0;
	return 0;
}

void
__librt_rewinddir_r(struct _reent *r, DIR *dirp)
{
	if (dirp)
		reopen_dir(r, dirp);
}

void
__librt_seekdir_r(struct _reent *r, DIR *dirp, long int loc)
{
	struct dirent discard;
	struct dirent *result;
	long int i;

	if (!dirp || loc < 0) {
		r->_errno = EINVAL;
		return;
	}
	if (reopen_dir(r, dirp) < 0)
		return;

	for (i = 0; i < loc; i++) {
		s32 ret = readdir_i(dirp, &discard, &result);
		if (ret < 0) {
			lv2errno_r(r, ret);
			return;
		}
		if (!result) {
			r->_errno = EINVAL;
			return;
		}
	}
}
