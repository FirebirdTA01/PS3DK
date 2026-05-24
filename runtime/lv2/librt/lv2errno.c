/*
 * lv2errno.c — LV2 error-code to POSIX errno mapping.
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

/* LV2 error codes are 0x8001XXXX; every non-negative return is success. */
s32
lv2error(s32 error)
{
	switch (error) {
	case 0x00000000: return 0;
	case 0x80010009: return EPERM;          /* Not super-user */
	case 0x80010006: return ENOENT;         /* No such file or directory */
	case 0x80010005: return ESRCH;          /* No such process */
	case 0x8001001F: return EINTR;          /* Interrupted system call */
	case 0x8001002B: return EIO;            /* I/O error */
	case 0x8001002F: return ENXIO;          /* No such device or address */
	case 0x80010007: return ENOEXEC;        /* Exec format error */
	case 0x8001002A: return EBADF;          /* Bad file number */
	case 0x80010001: return EAGAIN;         /* No more processes */
	case 0x80010004: return ENOMEM;         /* Not enough core */
	case 0x80010029: return EACCES;         /* Permission denied */
	case 0x8001000D: return EFAULT;         /* Bad address */
	case 0x8001000A: return EBUSY;          /* Mount device busy */
	case 0x80010014: return EEXIST;         /* File exists */
	case 0x80010030: return EXDEV;          /* Cross-device link */
	case 0x8001002D: return ENODEV;         /* No such device */
	case 0x8001002E: return ENOTDIR;        /* Not a directory */
	case 0x80010012: return EISDIR;         /* Is a directory */
	case 0x80010002: return EINVAL;         /* Invalid argument */
	case 0x80010022: return ENFILE;         /* Too many open files in system */
	case 0x8001002C: return EMFILE;         /* Too many open files */
	case 0x80010024: return ENOTTY;         /* Not a typewriter */
	case 0x80010020: return EFBIG;          /* File too large */
	case 0x80010023: return ENOSPC;         /* No space left on device */
	case 0x80010027: return ESPIPE;         /* Illegal seek */
	case 0x80010026: return EROFS;          /* Read only file system */
	case 0x80010021: return EMLINK;         /* Too many links */
	case 0x80010025: return EPIPE;          /* Broken pipe */
	case 0x8001001B: return EDOM;           /* Math arg out of domain of func */
	case 0x8001001C: return ERANGE;         /* Math result not representable */
	case 0x80010008: return EDEADLK;        /* Deadlock condition */
	case 0x80010035: return ENOLCK;         /* No record locks available */
	case 0x80010031: return EBADMSG;        /* Trying to read unreadable msg */
	case 0x80010003: return ENOSYS;         /* Function not implemented */
	case 0x80010036: return ENOTEMPTY;      /* Directory not empty */
	case 0x80010034: return ENAMETOOLONG;   /* File or path name too long */
	case 0x8001000B: return ETIMEDOUT;      /* Connection timed out */
	case 0x80010032: return EINPROGRESS;    /* Connection already in progress */
	case 0x80010033: return EMSGSIZE;       /* Message too long */
	case 0x80010015: return EISCONN;        /* Socket is already connected */
	case 0x80010016: return ENOTCONN;       /* Socket is not connected */
	case 0x80010037: return ENOTSUP;        /* Not supported */
	case 0x8001001D: return EILSEQ;         /* Illegal byte sequence */
	case 0x80010039: return EOVERFLOW;      /* Value too large for data type */
	case 0x80010013: return ECANCELED;      /* Operation canceled */
	default:         return EINVAL;
	}
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
