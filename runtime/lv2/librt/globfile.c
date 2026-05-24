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

static char __cwd[PATH_MAX];

int
__librt_chdir_r(struct _reent *r, const char *dirname)
{
	strncpy(__cwd, dirname, PATH_MAX);
	__cwd[PATH_MAX - 1] = '\0';
	return 0;
}

char *
__librt_getcwd_r(struct _reent *r, char *buf, size_t size)
{
	if (!buf) {
		r->_errno = EINVAL;
		return NULL;
	}

	strncpy(buf, __cwd, size);
	buf[size - 1] = '\0';
	return buf;
}
