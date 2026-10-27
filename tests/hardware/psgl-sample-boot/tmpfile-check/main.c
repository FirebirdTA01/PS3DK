/* Checks samples/toolchain/psgl-common/psgl_sample_tmpfile.h.
 * Fault cases run against an in-memory file system through the helper's
 * operation table: an existing name is skipped and kept, a stat error other
 * than ENOENT refuses with nothing opened or written, a failed exclusive
 * create, write or close leaves no file of ours behind and never touches a
 * prior file, and remove deletes only what was created.  A last case uses
 * the real lv2 file system in /dev_hdd0/tmp.
 * Prints PSGL_TMPFILE_CHECK_OK checks=N failures=0, or ..._FAIL lines. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>

#include "../../../../samples/toolchain/psgl-common/psgl_sample_tmpfile.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define MAXF 8

typedef struct FakeFile {
    char path[64];
    char data[32];
    uint32_t size;
    int used;
} FakeFile;

static FakeFile g_files[MAXF];
static int32_t g_stat_error;       /* returned for g_stat_error_path */
static char g_stat_error_path[64];
static int g_fail_open, g_fail_write, g_fail_close;
static int g_opens, g_writes, g_unlinks;
static int g_checks, g_failures;

static FakeFile *find(const char *path)
{
    for (int i = 0; i < MAXF; i++)
        if (g_files[i].used && strcmp(g_files[i].path, path) == 0) return &g_files[i];
    return NULL;
}

static int32_t fake_stat(const char *path)
{
    if (g_stat_error && strcmp(path, g_stat_error_path) == 0) return g_stat_error;
    return find(path) ? 0 : (int32_t)ENOENT;
}

static int32_t fake_open_excl(const char *path, int32_t *fd)
{
    g_opens++;
    if (g_fail_open) return (int32_t)EIO;
    if (find(path)) return (int32_t)EEXIST;
    for (int i = 0; i < MAXF; i++) {
        if (!g_files[i].used) {
            memset(&g_files[i], 0, sizeof g_files[i]);
            g_files[i].used = 1;
            snprintf(g_files[i].path, sizeof g_files[i].path, "%s", path);
            *fd = i;
            return 0;
        }
    }
    return (int32_t)ENOSPC;
}

static int32_t fake_write(int32_t fd, const void *data, uint64_t size, uint64_t *written)
{
    g_writes++;
    if (g_fail_write) { *written = size / 2u; return 0; }   /* short write */
    if (size > sizeof g_files[fd].data) return (int32_t)EIO;
    memcpy(g_files[fd].data, data, (size_t)size);
    g_files[fd].size = (uint32_t)size;
    *written = size;
    return 0;
}

static int32_t fake_close(int32_t fd)
{
    (void)fd;
    return g_fail_close ? (int32_t)EIO : 0;
}

static int32_t fake_unlink(const char *path)
{
    FakeFile *f = find(path);
    g_unlinks++;
    if (!f) return (int32_t)ENOENT;
    f->used = 0;
    return 0;
}

static const psgl_sample_fs_ops k_fake = {
    fake_stat, fake_open_excl, fake_write, fake_close, fake_unlink
};

static void reset(void)
{
    memset(g_files, 0, sizeof g_files);
    g_stat_error = 0;
    g_stat_error_path[0] = 0;
    g_fail_open = g_fail_write = g_fail_close = 0;
    g_opens = g_writes = g_unlinks = 0;
}

static void add_prior(const char *path, const char *data)
{
    for (int i = 0; i < MAXF; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            snprintf(g_files[i].path, sizeof g_files[i].path, "%s", path);
            snprintf(g_files[i].data, sizeof g_files[i].data, "%s", data);
            g_files[i].size = (uint32_t)strlen(data);
            return;
        }
    }
}

static int count_files(void)
{
    int n = 0;
    for (int i = 0; i < MAXF; i++) n += g_files[i].used;
    return n;
}

static int prior_intact(const char *path, const char *data)
{
    FakeFile *f = find(path);
    return f && f->size == strlen(data) && memcmp(f->data, data, f->size) == 0;
}

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("PSGL_TMPFILE_CHECK_FAIL %s\n", what);
    }
}

#define P0 "/dev_hdd0/tmp/t-00.bin"
#define P1 "/dev_hdd0/tmp/t-01.bin"

int main(void)
{
    static const char payload[] = "payload";
    char path[64];
    int ok;

    /* Existing name: skipped and kept; the next name is created. */
    reset();
    add_prior(P0, "prior");
    ok = psgl_sample_tmpfile_create(&k_fake, "t", "bin", payload, 7u, path, sizeof path);
    check(ok && strcmp(path, P1) == 0, "existing name skipped, next name created");
    check(prior_intact(P0, "prior"), "existing file unchanged");
    check(find(P1) && find(P1)->size == 7u && memcmp(find(P1)->data, payload, 7) == 0,
          "new file holds the payload");
    check(psgl_sample_tmpfile_remove(&k_fake, path) && !find(P1) && path[0] == 0,
          "remove deletes the created file");
    check(prior_intact(P0, "prior"), "remove leaves the prior file");
    check(psgl_sample_tmpfile_remove(&k_fake, path) && g_unlinks == 1,
          "remove of an empty path is a no-op");

    /* stat error other than ENOENT: refused, nothing opened or written. */
    reset();
    g_stat_error = (int32_t)EIO;
    snprintf(g_stat_error_path, sizeof g_stat_error_path, "%s", P0);
    ok = psgl_sample_tmpfile_create(&k_fake, "t", "bin", payload, 7u, path, sizeof path);
    check(!ok && path[0] == 0, "non-ENOENT stat error refused");
    check(g_opens == 0 && g_writes == 0 && g_unlinks == 0 && count_files() == 0,
          "stat error: no open, write or remove");

    /* A stat error on a later name, with an existing first name. */
    reset();
    add_prior(P0, "prior");
    g_stat_error = (int32_t)EACCES;
    snprintf(g_stat_error_path, sizeof g_stat_error_path, "%s", P1);
    ok = psgl_sample_tmpfile_create(&k_fake, "t", "bin", payload, 7u, path, sizeof path);
    check(!ok && g_opens == 0 && prior_intact(P0, "prior") && count_files() == 1,
          "stat error after an existing name: refused, prior kept");

    /* Exclusive create fails: refused, nothing written or removed. */
    reset();
    add_prior(P0, "prior");
    g_fail_open = 1;
    ok = psgl_sample_tmpfile_create(&k_fake, "t", "bin", payload, 7u, path, sizeof path);
    check(!ok && g_writes == 0 && g_unlinks == 0 && prior_intact(P0, "prior") &&
          count_files() == 1, "failed exclusive create: refused, prior kept");

    /* Short write: the created file is removed, the prior one kept. */
    reset();
    add_prior(P0, "prior");
    g_fail_write = 1;
    ok = psgl_sample_tmpfile_create(&k_fake, "t", "bin", payload, 7u, path, sizeof path);
    check(!ok && path[0] == 0 && !find(P1) && prior_intact(P0, "prior") && g_unlinks == 1,
          "short write: created file removed, prior kept");

    /* Failed close: the created file is removed. */
    reset();
    g_fail_close = 1;
    ok = psgl_sample_tmpfile_create(&k_fake, "t", "bin", payload, 7u, path, sizeof path);
    check(!ok && count_files() == 0 && g_unlinks == 1, "failed close: created file removed");

    /* Real lv2 file system: two files get two names, removal is exact. */
    {
        char a[64], b[64];
        sysFSStat st;
        int ok_a = psgl_sample_tmpfile_create(&psgl_sample_lv2_fs, "psgl-tmpfile-check", "bin",
                                              payload, 7u, a, sizeof a);
        int ok_b = psgl_sample_tmpfile_create(&psgl_sample_lv2_fs, "psgl-tmpfile-check", "bin",
                                              "second", 6u, b, sizeof b);
        check(ok_a && ok_b && strcmp(a, b) != 0, "lv2: two files, two names");
        check(ok_a && sysLv2FsStat(a, &st) == 0 && st.st_size == 7u, "lv2: first file kept its size");
        int removed = psgl_sample_tmpfile_remove(&psgl_sample_lv2_fs, a);
        removed &= psgl_sample_tmpfile_remove(&psgl_sample_lv2_fs, b);
        check(removed, "lv2: both removed");
    }

    if (g_failures == 0)
        printf("PSGL_TMPFILE_CHECK_OK checks=%d failures=0\n", g_checks);
    else
        printf("PSGL_TMPFILE_CHECK_FAIL checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
