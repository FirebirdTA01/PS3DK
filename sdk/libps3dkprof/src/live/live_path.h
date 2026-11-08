/* The measured-device path rule of the live link, as a header-only helper
 * so the host tests check exactly what the LV2 shim enforces.
 *
 * live_path_under(path, prefix) is 1 only when path is prefix (which ends in
 * '/') followed by one or more components, each nonempty and neither "." nor
 * "..". Nothing is resolved or normalized: a path that could leave the
 * measured device ("/dev_hdd0/../dev_usb000/x"), or that needs normalizing to
 * say where it is ("//", a trailing '/'), is refused. */
#ifndef PS3DK_PROF_LIVE_PATH_H
#define PS3DK_PROF_LIVE_PATH_H
#include <string.h>

static inline int live_path_under(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    if (!path || n == 0 || prefix[n - 1] != '/' || strncmp(path, prefix, n) != 0)
        return 0;
    const char *c = path + n;
    if (*c == 0)
        return 0; /* the device itself, no file */
    for (;;) {
        const char *e = strchr(c, '/');
        size_t len = e ? (size_t)(e - c) : strlen(c);
        if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
            return 0;
        if (!e)
            return 1;
        c = e + 1;
    }
}

#endif
