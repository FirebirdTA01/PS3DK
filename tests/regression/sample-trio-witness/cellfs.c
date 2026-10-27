/*
 * hello-ppu-cellfs-async-stream witness. Every cellFs call the sample
 * makes goes to an in-memory fake here; no real file is touched.
 *
 *   case 0 control:        everything succeeds; must pass, closing both
 *                          fds, finishing AIO and removing its file.
 *   case 1 scratch-exists: the scratch name already exists; must fail
 *                          without opening, writing or removing it, and
 *                          the existing contents stay byte-identical.
 *   case 2 stat-eio:       the admission stat fails with EIO; must fail
 *                          without opening or removing anything.
 *   case 3 req0-timeout:   the first AIO read never completes; must fail
 *                          without a second request, streaming, closing
 *                          the read fd, finishing AIO or removing the
 *                          file. A late callback for it afterwards must
 *                          not count as any other request's completion.
 *   case 4 cancel-stuck:   the cancelled request never completes; must
 *                          fail with the same resources left alone.
 *   case 5 close-fail:     closing the read fd fails; must fail, and the
 *                          file is still removed.
 *   case 6 finish-fail:    cellFsAioFinish fails; must fail, and the file
 *                          is still removed.
 */
#include <stdint.h>
#include <string.h>
#include <cell/cell_fs.h>

#include "witness.h"

#define FAKE_SIZE (1 * 1024 * 1024)
#define WFD 3
#define RFD 4

static uint8_t  file_data[FAKE_SIZE];
static uint64_t file_size;
static int      file_exists;
static char     file_path[128];

static int n_open, n_write, n_unlink, n_close_w, n_close_r, n_close_other;
static int n_aio_init, n_aio_finish, n_aio_read, n_aio_cancel;
static int n_st_init, n_st_finish;
static uint64_t st_cursor, st_end;

static CellFsAio *pending_aio[2];
static void (*pending_func[2])(CellFsAio *, CellFsErrno, int, uint64_t);

static CellFsErrno fake_stat(const char *path, CellFsStat *sb)
{
    if (WITNESS_CASE == 2)
        return CELL_FS_EIO;
    if (WITNESS_CASE == 1 && !file_path[0]) {
        /* Pretend the name the sample picked already holds someone's data. */
        snprintf(file_path, sizeof(file_path), "%s", path);
        file_exists = 1;
    }
    if (!file_exists || strcmp(path, file_path) != 0)
        return CELL_FS_ENOENT;
    memset(sb, 0, sizeof(*sb));
    sb->st_size = file_size;
    return CELL_FS_OK;
}

static CellFsErrno fake_open(const char *path, int flags, int *fd,
                             const void *arg, uint64_t size)
{
    (void)arg; (void)size;
    n_open++;
    if (flags & CELL_FS_O_CREAT) {
        if (!(flags & CELL_FS_O_EXCL) || (flags & CELL_FS_O_TRUNC))
            return CELL_FS_EINVAL;      /* the sample must only create */
        if (file_exists && strcmp(path, file_path) == 0)
            return CELL_FS_EEXIST;
        snprintf(file_path, sizeof(file_path), "%s", path);
        file_exists = 1;
        file_size = 0;
        *fd = WFD;
        return CELL_FS_OK;
    }
    if (!file_exists || strcmp(path, file_path) != 0)
        return CELL_FS_ENOENT;
    *fd = RFD;
    return CELL_FS_OK;
}

static CellFsErrno fake_write(int fd, const void *buf, uint64_t n, uint64_t *done)
{
    n_write++;
    if (fd != WFD || n > FAKE_SIZE)
        return CELL_FS_EBADF;
    memcpy(file_data, buf, n);
    file_size = n;
    *done = n;
    return CELL_FS_OK;
}

static CellFsErrno fake_close(int fd)
{
    if (fd == WFD) { n_close_w++; return CELL_FS_OK; }
    if (fd == RFD) { n_close_r++; return WITNESS_CASE == 5 ? CELL_FS_EIO : CELL_FS_OK; }
    n_close_other++;
    return CELL_FS_EBADF;
}

static CellFsErrno fake_unlink(const char *path)
{
    n_unlink++;
    if (!file_exists || strcmp(path, file_path) != 0)
        return CELL_FS_ENOENT;
    file_exists = 0;
    return CELL_FS_OK;
}

static CellFsErrno fake_aio_init(const char *mount)
{
    (void)mount;
    n_aio_init++;
    return CELL_FS_OK;
}

static CellFsErrno fake_aio_finish(const char *mount)
{
    (void)mount;
    n_aio_finish++;
    return WITNESS_CASE == 6 ? CELL_FS_EIO : CELL_FS_OK;
}

static CellFsErrno fake_aio_read(CellFsAio *aio, int *id,
                                 void (*func)(CellFsAio *, CellFsErrno, int, uint64_t))
{
    int n = n_aio_read++;
    if (n >= 2 || aio->fd != RFD)
        return CELL_FS_EINVAL;
    *id = 100 + n;
    pending_aio[n] = aio;
    pending_func[n] = func;
    if ((WITNESS_CASE == 3 && n == 0) || (WITNESS_CASE == 4 && n == 1))
        return CELL_FS_OK;              /* never completes */
    uint64_t len = aio->size;
    if (aio->offset + len > file_size)
        len = aio->offset < file_size ? file_size - aio->offset : 0;
    memcpy(aio->buf, file_data + aio->offset, len);
    func(aio, CELL_FS_OK, *id, len);
    return CELL_FS_OK;
}

static CellFsErrno fake_aio_cancel(int id)
{
    (void)id;
    n_aio_cancel++;
    return CELL_FS_OK;
}

static CellFsErrno fake_st_init(int fd, const CellFsRingBuffer *r)
{
    (void)r;
    n_st_init++;
    return fd == RFD ? CELL_FS_OK : CELL_FS_EBADF;
}
static CellFsErrno fake_st_finish(int fd) { (void)fd; n_st_finish++; return CELL_FS_OK; }
static CellFsErrno fake_st_start(int fd, uint64_t off, uint64_t size)
{
    (void)fd;
    st_cursor = off;
    st_end = off + size;
    return CELL_FS_OK;
}
static CellFsErrno fake_st_stop(int fd) { (void)fd; return CELL_FS_OK; }
static CellFsErrno fake_st_status(int fd, uint64_t *s) { (void)fd; *s = 0; return CELL_FS_OK; }
static CellFsErrno fake_st_wait(int fd, uint64_t size) { (void)fd; (void)size; return CELL_FS_OK; }
static CellFsErrno fake_st_read(int fd, char *buf, uint64_t size, uint64_t *got)
{
    (void)fd;
    if (st_cursor + size > st_end)
        size = st_end - st_cursor;
    memcpy(buf, file_data + st_cursor, size);
    st_cursor += size;
    *got = size;
    return CELL_FS_OK;
}

#define cellFsStat            fake_stat
#define cellFsOpen            fake_open
#define cellFsWrite           fake_write
#define cellFsClose           fake_close
#define cellFsUnlink          fake_unlink
#define cellFsAioInit         fake_aio_init
#define cellFsAioFinish       fake_aio_finish
#define cellFsAioRead         fake_aio_read
#define cellFsAioCancel       fake_aio_cancel
#define cellFsStReadInit      fake_st_init
#define cellFsStReadFinish    fake_st_finish
#define cellFsStReadStart     fake_st_start
#define cellFsStReadStop      fake_st_stop
#define cellFsStReadGetStatus fake_st_status
#define cellFsStReadWait      fake_st_wait
#define cellFsStRead          fake_st_read
#define main sample_main
#include "lv2/hello-ppu-cellfs-async-stream/source/main.c"
#undef main

static uint32_t sum_bytes(void)
{
    uint32_t s = 0;
    for (uint64_t i = 0; i < FAKE_SIZE; i++)
        s = s * 31u + file_data[i];
    return s;
}

int main(void)
{
    /* The "someone else's" contents for case 1. */
    for (uint64_t i = 0; i < FAKE_SIZE; i++)
        file_data[i] = (uint8_t)(i * 7u + 3u);
    file_size = FAKE_SIZE;
    uint32_t before = sum_bytes();

    int rc = sample_main(0, NULL);
    printf("witness %s: sample exit status %d, open=%d write=%d close w/r=%d/%d "
           "unlink=%d aio init/read/cancel/finish=%d/%d/%d/%d stream init=%d\n",
           WITNESS_NAME, rc, n_open, n_write, n_close_w, n_close_r, n_unlink,
           n_aio_init, n_aio_read, n_aio_cancel, n_aio_finish, n_st_init);

    WITNESS_REQUIRE(n_close_other == 0);
    WITNESS_REQUIRE(strncmp(file_path, "/dev_hdd0/tmp/", 14) == 0 || WITNESS_CASE == 2);
    switch (WITNESS_CASE) {
    case 0:
        WITNESS_REQUIRE(rc == 0);
        WITNESS_REQUIRE(n_close_w == 1 && n_close_r == 1);
        WITNESS_REQUIRE(n_aio_read == 2 && n_aio_finish == 1);
        WITNESS_REQUIRE(n_st_init == 1 && n_st_finish == 1);
        WITNESS_REQUIRE(n_unlink == 1 && !file_exists);
        break;
    case 1:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_open == 0 && n_write == 0 && n_unlink == 0);
        WITNESS_REQUIRE(file_exists && file_size == FAKE_SIZE);
        WITNESS_REQUIRE(sum_bytes() == before);
        break;
    case 2:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_open == 0 && n_write == 0 && n_unlink == 0);
        break;
    case 3:
    case 4:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_aio_read == (WITNESS_CASE == 3 ? 1 : 2));
        WITNESS_REQUIRE(n_close_w == 1 && n_close_r == 0);
        WITNESS_REQUIRE(n_aio_finish == 0 && n_st_init == 0);
        WITNESS_REQUIRE(n_unlink == 0 && file_exists);
        {
            /* The stuck request completes late: it must land in its own
             * record and leave every other request's record untouched. */
            int n = WITNESS_CASE == 3 ? 0 : 1;
            int other = 1 - n;
            int other_done = __atomic_load_n(&req[other].done, __ATOMIC_ACQUIRE);
            WITNESS_REQUIRE(!__atomic_load_n(&req[n].done, __ATOMIC_ACQUIRE));
            pending_func[n](pending_aio[n], CELL_FS_OK, 100 + n, 0);
            WITNESS_REQUIRE(__atomic_load_n(&req[n].done, __ATOMIC_ACQUIRE));
            WITNESS_REQUIRE(__atomic_load_n(&req[other].done, __ATOMIC_ACQUIRE) == other_done);
            WITNESS_REQUIRE(stray_callbacks == 0);
        }
        break;
    case 5:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_close_r == 1 && n_aio_finish == 1);
        WITNESS_REQUIRE(n_unlink == 1 && !file_exists);
        break;
    case 6:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_aio_finish == 1 && n_close_r == 1);
        WITNESS_REQUIRE(n_unlink == 1 && !file_exists);
        break;
    default:
        WITNESS_REQUIRE(!"unknown case");
    }
    return witness_finish();
}
