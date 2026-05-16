/*
 * sleep.c — POSIX usleep() / sleep() wrappers via Lv-2 timer syscalls.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <sys/reent.h>
#include <sys/lv2errno.h>
#include <sys/systime.h>

int
__librt_usleep_r(struct _reent *r, useconds_t usec)
{
	return lv2errno_r(r, sysUsleep(usec));
}

unsigned int
__librt_sleep_r(struct _reent *r, unsigned int seconds)
{
	return lv2errno_r(r, sysSleep(seconds));
}
