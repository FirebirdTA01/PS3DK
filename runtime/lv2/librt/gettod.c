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
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/lv2errno.h>
#include <sys/systime.h>

int
__librt_gettod_r(struct _reent *r, struct timeval *ptimeval, void *ptimezone)
{
	u64 sec, nsec;
	s32 ret;

	(void)ptimezone;

	if (!ptimeval) {
		r->_errno = EFAULT;
		return -1;
	}

	ret = sysGetCurrentTime(&sec, &nsec);
	if (ret)
		return lv2errno_r(r, ret);

	ptimeval->tv_sec = (time_t)sec;
	ptimeval->tv_usec = (suseconds_t)(nsec / 1000);
	return 0;
}
