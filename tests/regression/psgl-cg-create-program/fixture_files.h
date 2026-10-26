/* Fixture files that a test writes at fixed paths, with the previous state
   of each path preserved: snapshots of every path are taken before any
   mutation, only a confirmed ENOENT counts as absent, and a path is
   restored only from a completed snapshot.  All I/O goes through an ops
   table so the helpers can be checked against injected faults. */
#ifndef PSGL_CG_FIXTURE_FILES_H
#define PSGL_CG_FIXTURE_FILES_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct FixtureOps {
    void *ctx;
    /* 0 when the path exists, otherwise the errno of the failed stat. */
    int (*probe)(void *ctx, const char *path);
    void *(*open)(void *ctx, const char *path, int write);
    size_t (*read)(void *ctx, void *handle, void *data, size_t size);
    int (*read_error)(void *ctx, void *handle);
    size_t (*write)(void *ctx, void *handle, const void *data, size_t size);
    int (*close)(void *ctx, void *handle);
    int (*remove)(void *ctx, const char *path);
    void *(*grow)(void *ctx, void *block, size_t size);
} FixtureOps;

typedef struct FixtureSnapshot {
    const char *path;
    unsigned char *bytes;
    size_t size;
    int present;
    int complete;
} FixtureSnapshot;

static void fixture_snapshot_free(const FixtureOps *ops, FixtureSnapshot *s)
{
    if (s->bytes) ops->grow(ops->ctx, s->bytes, 0);
    s->bytes = NULL;
    s->size = 0;
    s->complete = 0;
}

/* Returns 1 with a complete snapshot (present bytes, or confirmed absent);
   0 on any failure, with nothing kept. */
static int fixture_snapshot_take(const FixtureOps *ops, FixtureSnapshot *s,
                                 const char *path)
{
    s->path = path; s->bytes = NULL; s->size = 0; s->present = 0; s->complete = 0;
    int probed = ops->probe(ops->ctx, path);
    if (probed == ENOENT) { s->complete = 1; return 1; }
    if (probed != 0) return 0;
    void *h = ops->open(ops->ctx, path, 0);
    if (!h) return 0;
    size_t capacity = 0;
    int ok = 1;
    for (;;) {
        if (s->size == capacity) {
            size_t next = capacity ? capacity * 2u : 256u;
            unsigned char *grown = (unsigned char *)ops->grow(ops->ctx, s->bytes, next);
            if (!grown) { ok = 0; break; }
            s->bytes = grown; capacity = next;
        }
        size_t got = ops->read(ops->ctx, h, s->bytes + s->size, capacity - s->size);
        s->size += got;
        if (got == 0) { if (ops->read_error(ops->ctx, h)) ok = 0; break; }
    }
    if (ops->close(ops->ctx, h) != 0) ok = 0;
    if (!ok) { fixture_snapshot_free(ops, s); return 0; }
    s->present = 1;
    s->complete = 1;
    return 1;
}

/* Snapshots of both paths, or neither: a failed second snapshot frees the
   first, so the caller must not mutate or restore either path. */
static int fixture_snapshot_pair(const FixtureOps *ops, FixtureSnapshot *a,
                                 const char *path_a, FixtureSnapshot *b,
                                 const char *path_b)
{
    b->bytes = NULL; b->complete = 0;
    if (!fixture_snapshot_take(ops, a, path_a)) return 0;
    if (!fixture_snapshot_take(ops, b, path_b)) {
        fixture_snapshot_free(ops, a);
        return 0;
    }
    return 1;
}

static int fixture_write(const FixtureOps *ops, const char *path,
                         const void *data, size_t size)
{
    void *h = ops->open(ops->ctx, path, 1);
    if (!h) return 0;
    int ok = ops->write(ops->ctx, h, data, size) == size;
    return ops->close(ops->ctx, h) == 0 && ok;
}

/* Puts the path back as the snapshot found it.  Refuses an incomplete
   snapshot; an absent path is removed only after a successful stat, and a
   stat error other than ENOENT fails. */
static int fixture_snapshot_restore(const FixtureOps *ops, FixtureSnapshot *s)
{
    int ok;
    if (!s->complete) return 0;
    if (s->present) {
        ok = fixture_write(ops, s->path, s->bytes, s->size);
    } else {
        int probed = ops->probe(ops->ctx, s->path);
        ok = probed == ENOENT || (probed == 0 && ops->remove(ops->ctx, s->path) == 0);
    }
    fixture_snapshot_free(ops, s);
    return ok;
}

/* The real file system. */
static int fixture_real_probe(void *ctx, const char *path)
{
    struct stat st;
    (void)ctx;
    errno = 0;
    if (stat(path, &st) == 0) return 0;
    return errno ? errno : EIO;
}
static void *fixture_real_open(void *ctx, const char *path, int write)
{ (void)ctx; return fopen(path, write ? "wb" : "rb"); }
static size_t fixture_real_read(void *ctx, void *h, void *data, size_t size)
{ (void)ctx; return fread(data, 1, size, (FILE *)h); }
static int fixture_real_read_error(void *ctx, void *h)
{ (void)ctx; return ferror((FILE *)h) != 0; }
static size_t fixture_real_write(void *ctx, void *h, const void *data, size_t size)
{ (void)ctx; return fwrite(data, 1, size, (FILE *)h); }
static int fixture_real_close(void *ctx, void *h)
{ (void)ctx; return fclose((FILE *)h); }
static int fixture_real_remove(void *ctx, const char *path)
{ (void)ctx; return remove(path); }
static void *fixture_real_grow(void *ctx, void *block, size_t size)
{
    (void)ctx;
    if (!size) { free(block); return NULL; }
    return realloc(block, size);
}
static const FixtureOps fixture_real_ops = {
    NULL, fixture_real_probe, fixture_real_open, fixture_real_read,
    fixture_real_read_error, fixture_real_write, fixture_real_close,
    fixture_real_remove, fixture_real_grow
};

#endif
