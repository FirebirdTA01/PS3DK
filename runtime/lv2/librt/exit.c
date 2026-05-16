/*
 * exit.c — Process exit wrapper for Lv-2.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <stdlib.h>
#include <sys/process.h>

extern void _fini(void);

void
__librt_exit(int rc)
{
	_fini();
	sysProcessExit(rc);
}
