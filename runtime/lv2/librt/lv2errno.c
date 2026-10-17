/*
 * lv2errno.c — LV2 error code to errno (identity on this target).
 *
 * Replaces the libsysbase errno-mapping previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <sys/reent.h>
#include <ppu-types.h>
#include <sys/lv2errno.h>

/* LV2 error codes are 0x8001XXXX; every non-negative return is success.
   On this target the E* errno names are the LV2 codes themselves (newlib's
   <sys/_lv2_errno.h>), so an LV2 status maps to itself: errno holds exactly
   what the system call returned, including codes without a POSIX name
   (EABORT, ENOTMOUNTED, ...).  Anything outside the LV2 range is EINVAL.  */
s32
lv2error(s32 error)
{
	if (error == 0)
		return 0;
	if (((u32)error & 0xFFFF0000u) == 0x80010000u)
		return error;
	return EINVAL;
}

s32
lv2errno(s32 error)
{
	if (error >= 0)
		return error;

	errno = lv2error(error);
	return -1;
}

s32
lv2errno_r(struct _reent *r, s32 error)
{
	if (error >= 0)
		return error;

	r->_errno = lv2error(error);
	return -1;
}
