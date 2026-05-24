/*
 * gettod.c — POSIX gettimeofday() wrapper via Lv-2 time syscalls.
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
__librt_gettod_r(struct _reent *r, struct timeval *ptimeval, void *ptimezone)
{
	return lv2errno_r(r, sysGetCurrentTime(ptimeval, ptimezone));
}
