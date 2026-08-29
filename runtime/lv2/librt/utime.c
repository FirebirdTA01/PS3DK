/*
 * utime.c — POSIX utime() wrapper for Lv-2 filesystem.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <stddef.h>
#include <time.h>
#include <sys/reent.h>
#include <utime.h>
#include <sys/lv2errno.h>
#include <sys/file.h>

_Static_assert(sizeof(((sysFSUtimbuf *)0)->actime) ==
               sizeof(((struct utimbuf *)0)->actime),
               "sysFSUtimbuf.actime must match struct utimbuf.actime");
_Static_assert(sizeof(((sysFSUtimbuf *)0)->modtime) ==
               sizeof(((struct utimbuf *)0)->modtime),
               "sysFSUtimbuf.modtime must match struct utimbuf.modtime");

int
__librt_utime_r(struct _reent *r, const char *path,
                const struct utimbuf *times)
{
	sysFSUtimbuf lv2times;

	if (times) {
		lv2times.actime = times->actime;
		lv2times.modtime = times->modtime;
	} else {
		time_t now = time(NULL);
		lv2times.actime = now;
		lv2times.modtime = now;
	}

	return lv2errno_r(r, sysLv2FsUtime(path, &lv2times));
}
