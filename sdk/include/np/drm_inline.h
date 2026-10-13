/* sdk/include/np/drm_inline.h
 *
 * <np/drm_inline.h>: an inline shim over <cell/np/drm.h>.
 *
 * sceNpDrmOpen does the three-step dance to open a DRM-protected file:
 *   1. cellFsStat the path; if the file does not exist, return that ENOENT.
 *      Any other stat result is ignored (the stat is only a fast-path check).
 *   2. sceNpDrmIsAvailable; a non-OK result is returned as-is.
 *   3. cellFsOpen with a SceNpDrmOpenArg carrying SCE_NP_DRM_OPEN_FLAG.
 *
 * The last two parameters exist for call compatibility and are not read.
 */
#ifndef PS3TC_NP_DRM_INLINE_H
#define PS3TC_NP_DRM_INLINE_H

#include <cell/cell_fs.h>
#include <cell/np/drm.h>
#include <cell/error.h>    /* CELL_OK */

#ifdef __cplusplus
extern "C" {
#endif

static inline int sceNpDrmOpen(const SceNpDrmKey *k_licensee, const char *path,
                               int flags, int *fd, void *arg, uint64_t size)
{
    (void)arg;
    (void)size;
    CellFsStat st;
    int rc = (int)cellFsStat(path, &st);
    if (rc == (int)CELL_FS_ERROR_ENOENT)
        return rc;
    rc = sceNpDrmIsAvailable(k_licensee, path);
    if (rc != CELL_OK)
        return rc;
    SceNpDrmOpenArg a;
    a.flag = SCE_NP_DRM_OPEN_FLAG;
    return (int)cellFsOpen(path, flags, fd, &a, sizeof a);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_NP_DRM_INLINE_H */
