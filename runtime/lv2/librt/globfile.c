/*
 * globfile.c — POSIX chdir() / getcwd() for Lv-2 processes.
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
#include <sys/syslimits.h>
#include <sys/lv2errno.h>
#include <sys/file.h>

static char __cwd[PATH_MAX] = "/";

static int
append_component(char *path, const char *component, size_t len)
{
	size_t used = strlen(path);

	if (used > 1) {
		if (used + 1 >= PATH_MAX)
			return -1;
		path[used++] = '/';
		path[used] = '\0';
	}

	if (used + len >= PATH_MAX)
		return -1;
	memcpy(path + used, component, len);
	path[used + len] = '\0';
	return 0;
}

static void
pop_component(char *path)
{
	char *slash;

	if (strcmp(path, "/") == 0)
		return;

	slash = strrchr(path, '/');
	if (!slash || slash == path) {
		strcpy(path, "/");
		return;
	}

	*slash = '\0';
}

static int
normalize_path(const char *dirname, char *resolved)
{
	const char *cursor;

	if (!dirname || !*dirname)
		return EINVAL;

	if (dirname[0] == '/') {
		strcpy(resolved, "/");
		cursor = dirname + 1;
	} else {
		strncpy(resolved, __cwd, PATH_MAX);
		resolved[PATH_MAX - 1] = '\0';
		cursor = dirname;
	}

	while (*cursor) {
		const char *next = strchr(cursor, '/');
		size_t len = next ? (size_t)(next - cursor) : strlen(cursor);

		if (len == 0 || (len == 1 && cursor[0] == '.')) {
			/* Skip repeated separators and ".". */
		} else if (len == 2 && cursor[0] == '.' && cursor[1] == '.') {
			pop_component(resolved);
		} else if (append_component(resolved, cursor, len) < 0) {
			return ENAMETOOLONG;
		}

		if (!next)
			break;
		cursor = next + 1;
	}

	return 0;
}

int
__librt_chdir_r(struct _reent *r, const char *dirname)
{
	char resolved[PATH_MAX];
	s32 fd;
	int err = normalize_path(dirname, resolved);
	s32 ret;

	if (err) {
		r->_errno = err;
		return -1;
	}

	ret = sysLv2FsOpenDir(resolved, &fd);
	if (ret)
		return lv2errno_r(r, ret);

	sysLv2FsCloseDir(fd);
	strcpy(__cwd, resolved);
	return 0;
}

char *
__librt_getcwd_r(struct _reent *r, char *buf, size_t size)
{
	size_t len;

	if (!buf) {
		r->_errno = EINVAL;
		return NULL;
	}
	if (size == 0) {
		r->_errno = EINVAL;
		return NULL;
	}

	len = strlen(__cwd);
	if (size <= len) {
		r->_errno = ERANGE;
		return NULL;
	}

	memcpy(buf, __cwd, len + 1);
	return buf;
}
