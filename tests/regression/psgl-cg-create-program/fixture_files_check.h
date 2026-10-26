/* Fault-injection check of fixture_files.h against an in-memory file
   system: no real file is touched.  Each injected fault (stat error other
   than ENOENT, snapshot read error, snapshot close error, allocation
   failure, a failed second snapshot) must refuse the snapshot, leave the
   original bytes unchanged and perform no write or remove; restoration of
   a present and of an absent path must put the path back. */
#ifndef PSGL_CG_FIXTURE_FILES_CHECK_H
#define PSGL_CG_FIXTURE_FILES_CHECK_H

#include "fixture_files.h"

#define FAKE_CAP 64u

typedef struct FakeFile {
    const char *name;
    unsigned char data[FAKE_CAP];
    size_t size;
    int present;
    int probe_errno;   /* injected stat failure; 0 = none */
} FakeFile;

typedef struct FakeFs {
    FakeFile files[2];
    int fail_read, fail_close_read, fail_grow;
    unsigned write_opens, writes, removes;
    struct { int idx; int write; size_t pos; int err; int open; } h;
} FakeFs;

static FakeFile *fake_find(FakeFs *fs, const char *path)
{
    for (int i = 0; i < 2; ++i)
        if (!strcmp(fs->files[i].name, path)) return &fs->files[i];
    return NULL;
}
static int fake_probe(void *ctx, const char *path)
{
    FakeFile *f = fake_find((FakeFs *)ctx, path);
    if (!f) return ENOENT;
    if (f->probe_errno) return f->probe_errno;
    return f->present ? 0 : ENOENT;
}
static void *fake_open(void *ctx, const char *path, int write)
{
    FakeFs *fs = (FakeFs *)ctx;
    FakeFile *f = fake_find(fs, path);
    if (!f || fs->h.open || (!write && !f->present)) return NULL;
    fs->h.idx = (int)(f - fs->files); fs->h.write = write; fs->h.pos = 0;
    fs->h.err = 0; fs->h.open = 1;
    if (write) { ++fs->write_opens; f->present = 1; f->size = 0; }
    return &fs->h;
}
static size_t fake_read(void *ctx, void *h, void *data, size_t size)
{
    FakeFs *fs = (FakeFs *)ctx;
    FakeFile *f = &fs->files[fs->h.idx];
    (void)h;
    if (fs->fail_read) { fs->h.err = 1; return 0; }
    size_t n = f->size - fs->h.pos;
    if (n > size) n = size;
    memcpy(data, f->data + fs->h.pos, n);
    fs->h.pos += n;
    return n;
}
static int fake_read_error(void *ctx, void *h) { (void)h; return ((FakeFs *)ctx)->h.err; }
static size_t fake_write(void *ctx, void *h, const void *data, size_t size)
{
    FakeFs *fs = (FakeFs *)ctx;
    FakeFile *f = &fs->files[fs->h.idx];
    (void)h;
    ++fs->writes;
    size_t n = FAKE_CAP - f->size;
    if (n > size) n = size;
    memcpy(f->data + f->size, data, n);
    f->size += n;
    return n;
}
static int fake_close(void *ctx, void *h)
{
    FakeFs *fs = (FakeFs *)ctx;
    (void)h;
    fs->h.open = 0;
    return (!fs->h.write && fs->fail_close_read) ? EOF : 0;
}
static int fake_remove(void *ctx, const char *path)
{
    FakeFs *fs = (FakeFs *)ctx;
    FakeFile *f = fake_find(fs, path);
    ++fs->removes;
    if (!f || !f->present) return -1;
    f->present = 0; f->size = 0;
    return 0;
}
static void *fake_grow(void *ctx, void *block, size_t size)
{
    if (!size) { free(block); return NULL; }
    if (((FakeFs *)ctx)->fail_grow) return NULL;
    return realloc(block, size);
}

static const char fake_original[] = "original fixture bytes";

static void fake_reset(FakeFs *fs, FixtureOps *ops)
{
    memset(fs, 0, sizeof(*fs));
    fs->files[0].name = "/fake/present";
    fs->files[0].present = 1;
    fs->files[0].size = sizeof(fake_original);
    memcpy(fs->files[0].data, fake_original, sizeof(fake_original));
    fs->files[1].name = "/fake/absent";
    ops->ctx = fs; ops->probe = fake_probe; ops->open = fake_open;
    ops->read = fake_read; ops->read_error = fake_read_error;
    ops->write = fake_write; ops->close = fake_close;
    ops->remove = fake_remove; ops->grow = fake_grow;
}

/* The original is byte-for-byte unchanged and nothing was written or
   removed. */
static int fake_untouched(const FakeFs *fs)
{
    const FakeFile *f = &fs->files[0];
    return f->present && f->size == sizeof(fake_original) &&
           !memcmp(f->data, fake_original, sizeof(fake_original)) &&
           !fs->files[1].present && !fs->write_opens && !fs->writes &&
           !fs->removes;
}

static void fixture_files_selfcheck(void (*check)(int ok, const char *name))
{
    FakeFs fs;
    FixtureOps ops;
    FixtureSnapshot a, b;

    fake_reset(&fs, &ops); fs.fail_read = 1;
    check(!fixture_snapshot_take(&ops, &a, "/fake/present") && !a.complete && !a.bytes,
          "fault read refuses snapshot");
    check(fake_untouched(&fs), "fault read leaves original, no write or remove");

    fake_reset(&fs, &ops); fs.fail_close_read = 1;
    check(!fixture_snapshot_take(&ops, &a, "/fake/present") && !a.complete && !a.bytes,
          "fault close refuses snapshot");
    check(fake_untouched(&fs), "fault close leaves original, no write or remove");

    fake_reset(&fs, &ops); fs.fail_grow = 1;
    check(!fixture_snapshot_take(&ops, &a, "/fake/present") && !a.complete && !a.bytes,
          "fault allocation refuses snapshot");
    check(fake_untouched(&fs), "fault allocation leaves original, no write or remove");

    fake_reset(&fs, &ops); fs.files[0].probe_errno = EACCES;
    check(!fixture_snapshot_take(&ops, &a, "/fake/present") && !a.complete,
          "fault stat EACCES is not absence");
    check(fake_untouched(&fs), "fault stat leaves original, no write or remove");

    fake_reset(&fs, &ops); fs.files[1].probe_errno = EIO;
    check(!fixture_snapshot_pair(&ops, &a, "/fake/present", &b, "/fake/absent") &&
          !a.complete && !a.bytes && !b.complete,
          "failed second snapshot frees the first");
    check(!fixture_snapshot_restore(&ops, &a) && !fixture_snapshot_restore(&ops, &b) &&
          fake_untouched(&fs),
          "incomplete snapshots are not restored");

    fake_reset(&fs, &ops);
    int taken = fixture_snapshot_pair(&ops, &a, "/fake/present", &b, "/fake/absent");
    int mutated = fixture_write(&ops, "/fake/present", "mutated", 7) &&
                  fixture_write(&ops, "/fake/absent", "created", 7);
    int restored = fixture_snapshot_restore(&ops, &a);
    check(taken && mutated && restored && fs.files[0].size == sizeof(fake_original) &&
          !memcmp(fs.files[0].data, fake_original, sizeof(fake_original)),
          "present path restored to original bytes");
    check(fixture_snapshot_restore(&ops, &b) && !fs.files[1].present && fs.removes == 1,
          "absent path removed again");

    fake_reset(&fs, &ops);
    taken = fixture_snapshot_take(&ops, &b, "/fake/absent");
    mutated = fixture_write(&ops, "/fake/absent", "created", 7);
    fs.files[1].probe_errno = EIO;
    check(taken && mutated && !fixture_snapshot_restore(&ops, &b) && !fs.removes,
          "absent restore with stat error fails closed");
}

#endif
