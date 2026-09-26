/*
 * librt_path.h -- relative path resolution for the path-taking wrappers.
 *
 * Lv-2 filesystem calls take absolute paths only; the process working
 * directory exists only in librt (chdir/getcwd in globfile.c).  Every
 * wrapper that passes a path to the kernel resolves it here first.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */
#ifndef __LIBRT_PATH_H__
#define __LIBRT_PATH_H__

#include <sys/reent.h>
#include <sys/syslimits.h>

/* A relative path becomes the working directory, '/', and the path, joined
 * verbatim into buf: "." and ".." are left for the kernel, so a relative
 * path behaves exactly as its absolute spelling (a missing directory before
 * "..", or a trailing "/" after a regular file, still fails).  An absolute
 * or empty path is returned unchanged.  Returns the path to hand to the
 * kernel, or NULL with r->_errno set (EFAULT for a NULL path,
 * ENAMETOOLONG). */
const char *__librt_resolve_path(struct _reent *r, const char *path,
                                 char buf[PATH_MAX]);

#endif /* __LIBRT_PATH_H__ */
