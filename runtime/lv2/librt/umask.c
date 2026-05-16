/*
 * umask.c — POSIX umask() for Lv-2 processes.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <sys/reent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/lv2errno.h>

mode_t g_umask = S_IWGRP | S_IWOTH;

mode_t
__librt_umask_r(struct _reent *r, mode_t cmask)
{
	mode_t old = g_umask;
	g_umask = cmask;
	return old;
}
