/*
 * settod.c — POSIX settimeofday() wrapper via Lv-2 time syscalls.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <sys/reent.h>
#include <sys/time.h>
#include <sys/lv2errno.h>
#include <sys/systime.h>

int
__librt_settod_r(struct _reent *r, const struct timeval *ptimeval,
                 const struct timezone *ptimezone)
{
	return lv2errno_r(r, sysSetCurrentTime(ptimeval, ptimezone));
}
