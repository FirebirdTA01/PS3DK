/*
 * times.c — POSIX times() via PPU Time Base register (hardware, not LV2).
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <sys/reent.h>
#include <sys/times.h>
#include <sys/lv2errno.h>
#include <ppu-asm.h>

clock_t
__librt_times_r(struct _reent *r, struct tms *buf)
{
	return __gettime();
}
