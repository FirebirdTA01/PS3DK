/*
 * pathconf.c -- pathname limits for the Lv-2 filesystem interface.
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */
#include <errno.h>
#include <limits.h>
#include <string.h>
#ifdef __NEWLIB__
#include <sys/syslimits.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

long
pathconf(const char *path, int name)
{
    long limit;
    struct stat st;
    char resolved[PATH_MAX];

    /* Newlib's limits match CELL_FS_MAX_FS_{PATH,FILE_NAME}_LENGTH.
     * Do not infer values for other selectors from unrelated host limits.
     */
    switch (name) {
    case _PC_PATH_MAX:
        limit = PATH_MAX;
        break;
    case _PC_NAME_MAX:
        limit = NAME_MAX;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    if (!*path) {
        errno = ENOENT;
        return -1;
    }
    /* Lv-2 has no process cwd; librt maintains it for getcwd/chdir.
     * Preserve components for the filesystem lookup (do not collapse ..
     * across a component which might not exist). */
    if (*path != '/') {
        size_t used, separator, len = strlen(path);
        if (!getcwd(resolved, sizeof resolved))
            return -1;
        used = strlen(resolved);
        separator = used && resolved[used - 1] != '/';
        if (len >= sizeof resolved - used - separator) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (separator)
            resolved[used++] = '/';
        memcpy(resolved + used, path, len + 1);
        path = resolved;
    }

    /* Preserve the pathname lookup error, even though these two limits
     * are constant across the supported Lv-2 filesystem interface.
     */
    if (stat(path, &st) != 0)
        return -1;
    return limit;
}
