/* psgl_sample_tmpfile.h - private temporary files for the PSGL samples
 * (included as "../../psgl-common/psgl_sample_tmpfile.h").
 *
 * Some PSGL entry points take a path (psglLoadShaderLibrary,
 * cgCreateProgramFromFile), so a sample writes embedded data to a file
 * first.  It must never overwrite a file it did not create:
 *
 *   - names are <stem>-NN.<ext> under /dev_hdd0/tmp, NN = 00..99;
 *   - a name is used only when stat confirms it is absent (ENOENT); an
 *     existing name is skipped, and any other stat result fails;
 *   - the file is created with O_CREAT|O_EXCL, so a file that appears in
 *     between is not opened;
 *   - every write and the close are checked; on failure the file this call
 *     created is removed and nothing else is touched;
 *   - psgl_sample_tmpfile_remove() removes only a file this run created.
 *
 * The file operations go through a table so a check can inject failures;
 * samples pass &psgl_sample_lv2_fs.
 */
#ifndef PSGL_SAMPLE_TMPFILE_H
#define PSGL_SAMPLE_TMPFILE_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/file.h>

typedef struct psgl_sample_fs_ops {
    int32_t (*stat)(const char *path);   /* 0 = exists, else an lv2 error code */
    int32_t (*open_excl)(const char *path, int32_t *fd);
    int32_t (*write)(int32_t fd, const void *data, uint64_t size, uint64_t *written);
    int32_t (*close)(int32_t fd);
    int32_t (*unlink)(const char *path);
} psgl_sample_fs_ops;

static int32_t psgl_sample_lv2_stat(const char *path)
{
    sysFSStat st;
    return sysLv2FsStat(path, &st);
}

static int32_t psgl_sample_lv2_open_excl(const char *path, int32_t *fd)
{
    return sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_EXCL, fd, 0644, NULL, 0);
}

static const psgl_sample_fs_ops psgl_sample_lv2_fs = {
    psgl_sample_lv2_stat,
    psgl_sample_lv2_open_excl,
    sysLv2FsWrite,
    sysLv2FsClose,
    sysLv2FsUnlink,
};

/* Writes size bytes to a new private file and returns 1 with its name in
   path; returns 0 (and prints why) when no file was created. */
static inline int psgl_sample_tmpfile_create(const psgl_sample_fs_ops *ops,
                                             const char *stem, const char *ext,
                                             const void *data, uint32_t size,
                                             char *path, size_t cap)
{
    for (unsigned n = 0; n < 100u; n++) {
        int len = snprintf(path, cap, "/dev_hdd0/tmp/%s-%02u.%s", stem, n, ext);
        if (len < 0 || (size_t)len >= cap) break;
        int32_t rc = ops->stat(path);
        if (rc == 0) continue;                       /* exists: never reused */
        if (rc != (int32_t)ENOENT) {
            printf("%s: stat %s = %#x, not ENOENT: refused\n", stem, path, (unsigned)rc);
            path[0] = 0;
            return 0;
        }
        int32_t fd = -1;
        rc = ops->open_excl(path, &fd);
        if (rc != 0 || fd < 0) {
            printf("%s: exclusive create of %s failed %#x\n", stem, path, (unsigned)rc);
            path[0] = 0;
            return 0;
        }
        uint64_t written = 0;
        int ok = ops->write(fd, data, size, &written) == 0 && written == size;
        if (ops->close(fd) != 0) ok = 0;
        if (!ok) {
            printf("%s: writing %s failed\n", stem, path);
            ops->unlink(path);                       /* created by this call */
            path[0] = 0;
            return 0;
        }
        return 1;
    }
    printf("%s: no free temporary name\n", stem);
    path[0] = 0;
    return 0;
}

/* Removes a file psgl_sample_tmpfile_create made; an empty path is a no-op. */
static inline int psgl_sample_tmpfile_remove(const psgl_sample_fs_ops *ops, char *path)
{
    if (!path[0]) return 1;
    int ok = ops->unlink(path) == 0;
    path[0] = 0;
    return ok;
}

#endif
