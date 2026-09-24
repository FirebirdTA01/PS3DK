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

/* Called by CRT constructor 106, after the heap, syscall table and
 * newlib recursive-lock setup. User atexit registrations follow this one,
 * so normal exit runs them before the legacy .fini work. */
void
__librt_register_fini(void)
{
	if (atexit(_fini) != 0)
		sysProcessExit(1);
}

void
__librt_exit(int rc)
{
	/* This is newlib's low-level _exit hook, also reached by abort.
	 * Only exit() runs atexit callbacks and flushes stdio. */
	sysProcessExit(rc);
}
