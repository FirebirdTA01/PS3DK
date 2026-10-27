/*
 * hello-ppu-cellfs-async-stream - cellFs asynchronous I/O and streaming
 * read surfaces, side by side.
 *
 * Uses, against <cell/cell_fs.h>:
 *   cellFsAioInit / cellFsAioRead / cellFsAioCancel / cellFsAioFinish
 *   cellFsStReadInit / cellFsStReadStart / cellFsStReadGetStatus /
 *   cellFsStReadWait / cellFsStRead / cellFsStReadStop /
 *   cellFsStReadFinish
 *   cellFsStat / cellFsOpen / cellFsWrite / cellFsClose / cellFsUnlink
 *
 * Behaviour:
 *   1. Picks a scratch name in /dev_hdd0/tmp that includes the process id
 *      and the system time, and goes on only if cellFsStat reports that
 *      name does not exist (ENOENT). It then creates the file with
 *      O_CREAT | O_EXCL, so an existing file is never opened, truncated or
 *      removed; only a file this run created is unlinked at the end.
 *   2. Writes 1 MiB of a position-dependent pattern and closes it.
 *   3. AIO read: initialises AIO on /dev_hdd0, submits one 64 KiB
 *      cellFsAioRead at offset 64 KiB and waits for its callback.
 *   4. AIO cancel: submits a second 64 KiB read, into its own buffer, and
 *      cancels it at once. Whether the cancel wins is printed, not judged,
 *      but the request must still report completion.
 *   5. Streaming read: a 64 KiB ring of 8 KiB blocks on the same fd;
 *      256 KiB from offset 256 KiB as 32 blocks, each waited for, read
 *      and checked word by word.
 *   6. Finishes AIO, closes the fd and removes the scratch file.
 *
 * Each AIO request has its own CellFsAio, buffer and completion record.
 * The callback finds its record through user_data, stores the result and
 * then publishes completion with a release store; the main thread reads
 * the result only after an acquire load sees it. If a request has not
 * completed within AIO_TIMEOUT_MS, the sample cannot tell whether it is
 * still running, so it stops there with FAIL and leaves the fd, the
 * request buffers and the scratch file alone (static storage, released at
 * process exit); the file name is printed. cellFsStReadWait blocks until
 * the data arrives and is not bounded by this sample.
 *
 * Every checked call prints one line. The last line is
 * "hello-ppu-cellfs-async-stream: PASS (n of m checks)" or FAIL with the
 * same counts, and the exit status is 0 or 1.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/sys_time.h>
#include <cell/cell_fs.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define FILE_SIZE   (1 * 1024 * 1024)   /* 1 MiB  */
#define CHUNK_SIZE  (64 * 1024)         /* 64 KiB */
#define ST_BLOCK    (8 * 1024)          /* 8 KiB  */
#define ST_OFFSET   (256 * 1024)
#define ST_SIZE     (256 * 1024)
#define AIO_MOUNT   "/dev_hdd0"
#ifndef AIO_TIMEOUT_MS
#define AIO_TIMEOUT_MS 5000
#endif

static int checks, failures;

static int check(int ok, const char *what, unsigned long long got)
{
    checks++;
    if (!ok) failures++;
    printf("  %s %s (0x%llx)\n", ok ? "ok  " : "FAIL", what, got);
    return ok;
}

static int verdict(void)
{
    printf("\nhello-ppu-cellfs-async-stream: %s (%d of %d checks)\n",
           failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}

/* Deterministic 4-byte word per file offset, so a misplaced block is
 * visible as well as a corrupted one. */
static uint32_t word_at(uint64_t off)
{
    return (uint32_t)(off / 4) * 265013u ^ 0xdeadbeefu;
}

static uint32_t count_bad(const uint32_t *w, uint64_t base, size_t words)
{
    uint32_t bad = 0;
    for (size_t i = 0; i < words; i++)
        bad += w[i] != word_at(base + i * 4);
    return bad;
}

/* ---- AIO requests ------------------------------------------------- */

typedef struct {
    CellFsAio   aio;
    CellFsErrno err;      /* written by the callback before done */
    uint64_t    bytes;    /* written by the callback before done */
    int         done;     /* release-stored 1 by the callback */
} aio_request;

enum { NREQ = 2 };
static aio_request req[NREQ];
static uint32_t    req_buf[NREQ][CHUNK_SIZE / 4];
static int         stray_callbacks;

static void aio_callback(CellFsAio *aio, CellFsErrno err, int id, uint64_t size)
{
    (void)id;
    uint64_t n = aio ? aio->user_data : NREQ;
    if (n >= NREQ || __atomic_load_n(&req[n].done, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&stray_callbacks, 1, __ATOMIC_RELAXED);
        return;
    }
    req[n].err   = err;
    req[n].bytes = size;
    __atomic_store_n(&req[n].done, 1, __ATOMIC_RELEASE);
}

static void request_init(int n, int fd, uint64_t offset)
{
    memset(&req[n], 0, sizeof(req[n]));
    req[n].aio.fd        = fd;
    req[n].aio.offset    = offset;
    req[n].aio.buf       = req_buf[n];
    req[n].aio.size      = CHUNK_SIZE;
    req[n].aio.user_data = (uint64_t)n;
}

/* 1 once request n has completed (acquire), 0 after the timeout. */
static int request_wait(int n)
{
    for (int ms = 0; ms <= AIO_TIMEOUT_MS; ms += 10) {
        if (__atomic_load_n(&req[n].done, __ATOMIC_ACQUIRE))
            return 1;
        usleep(10 * 1000);
    }
    return __atomic_load_n(&req[n].done, __ATOMIC_ACQUIRE);
}

static int abandon(int n, const char *path)
{
    printf("  request %d has not completed after %d ms: it may still be\n"
           "  using the fd and its buffer, so the run stops here and leaves\n"
           "  them, AIO and %s in place\n", n, AIO_TIMEOUT_MS, path);
    check(0, "AIO request completed", (unsigned)n);
    return verdict();
}

/* ---- streaming read ----------------------------------------------- */

static void stream_test(int fd)
{
    printf("\n--- streaming read: %u bytes at offset %u ---\n",
           (unsigned)ST_SIZE, (unsigned)ST_OFFSET);

    CellFsRingBuffer ring;
    memset(&ring, 0, sizeof(ring));
    ring.ringbuf_size  = 64 * 1024;
    ring.block_size    = ST_BLOCK;
    ring.transfer_rate = 0;            /* no rate limit */
    ring.copy          = CELL_FS_ST_COPY;

    CellFsErrno err = cellFsStReadInit(fd, &ring);
    if (!check(err == CELL_FS_OK, "cellFsStReadInit", (unsigned)err))
        return;

    err = cellFsStReadStart(fd, ST_OFFSET, ST_SIZE);
    if (check(err == CELL_FS_OK, "cellFsStReadStart", (unsigned)err)) {
        uint64_t status = 0;
        err = cellFsStReadGetStatus(fd, &status);
        check(err == CELL_FS_OK, "cellFsStReadGetStatus", status);

        static uint32_t stbuf[ST_BLOCK / 4];
        int blocks = 0;
        for (int blk = 0; blk < ST_SIZE / ST_BLOCK; blk++) {
            uint64_t got = 0;
            err = cellFsStReadWait(fd, ST_BLOCK);
            if (err == CELL_FS_OK)
                err = cellFsStRead(fd, (char *)stbuf, ST_BLOCK, &got);
            if (err != CELL_FS_OK || got != ST_BLOCK) {
                printf("    stream block %d: err=0x%08x got=%llu\n",
                       blk, (unsigned)err, (unsigned long long)got);
                break;
            }
            uint64_t base = ST_OFFSET + (uint64_t)blk * ST_BLOCK;
            uint32_t bad = count_bad(stbuf, base, ST_BLOCK / 4);
            if (bad) {
                printf("    stream block %d: %u mismatched words at 0x%llx\n",
                       blk, (unsigned)bad, (unsigned long long)base);
                break;
            }
            blocks++;
        }
        check(blocks == ST_SIZE / ST_BLOCK,
              "all 32 stream blocks read and match", (unsigned)blocks);

        err = cellFsStReadStop(fd);
        check(err == CELL_FS_OK, "cellFsStReadStop", (unsigned)err);
    }

    err = cellFsStReadFinish(fd);
    check(err == CELL_FS_OK, "cellFsStReadFinish", (unsigned)err);
}

/* ---- main --------------------------------------------------------- */

static uint32_t wbuf[FILE_SIZE / 4];

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;

    char path[96];
    snprintf(path, sizeof(path), "/dev_hdd0/tmp/ps3tc_aio_stream_%08x_%016llx.bin",
             (unsigned)sys_process_getpid(),
             (unsigned long long)sys_time_get_system_time());

    printf("hello-ppu-cellfs-async-stream: AIO + streaming read test\n");
    printf("  scratch:   %s\n", path);
    printf("  file:      %u bytes\n", (unsigned)FILE_SIZE);

    /* --- 1. admission: the name must not exist ------------------- */
    CellFsStat sb;
    memset(&sb, 0, sizeof(sb));
    CellFsErrno err = cellFsStat(path, &sb);
    if (!check(err == CELL_FS_ENOENT, "scratch name is unused (stat = ENOENT)",
               (unsigned)err)) {
        printf("  refusing to touch an existing or unreadable path\n");
        return verdict();
    }

    int fd = -1;
    err = cellFsOpen(path, CELL_FS_O_CREAT | CELL_FS_O_EXCL | CELL_FS_O_WRONLY,
                     &fd, NULL, 0);
    if (!check(err == CELL_FS_OK, "cellFsOpen(create, exclusive)", (unsigned)err))
        return verdict();

    /* From here on the file is ours and is removed on every exit path
     * except an unfinished AIO request. */

    /* --- 2. write the pattern ------------------------------------ */
    for (size_t i = 0; i < FILE_SIZE / 4; i++)
        wbuf[i] = word_at(i * 4);
    uint64_t written = 0;
    err = cellFsWrite(fd, wbuf, FILE_SIZE, &written);
    int ok = check(err == CELL_FS_OK && written == FILE_SIZE,
                   "cellFsWrite(1 MiB)", (unsigned long long)written);
    err = cellFsClose(fd);
    ok &= check(err == CELL_FS_OK, "cellFsClose(write fd)", (unsigned)err);
    fd = -1;
    if (ok) {
        memset(&sb, 0, sizeof(sb));
        err = cellFsStat(path, &sb);
        ok = check(err == CELL_FS_OK && sb.st_size == FILE_SIZE,
                   "cellFsStat size", (unsigned long long)sb.st_size);
    }
    if (ok) {
        err = cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0);
        ok = check(err == CELL_FS_OK, "cellFsOpen(read)", (unsigned)err);
        if (!ok) fd = -1;
    }

    int aio_inited = 0;
    if (ok) {
        printf("\n--- AIO read at offset %u, size %u ---\n",
               (unsigned)CHUNK_SIZE, (unsigned)CHUNK_SIZE);
        err = cellFsAioInit(AIO_MOUNT);
        aio_inited = check(err == CELL_FS_OK, "cellFsAioInit(" AIO_MOUNT ")",
                           (unsigned)err);
        ok = aio_inited;
    }

    /* --- 3. AIO read --------------------------------------------- */
    if (ok) {
        int id = -1;
        request_init(0, fd, CHUNK_SIZE);
        err = cellFsAioRead(&req[0].aio, &id, aio_callback);
        ok = check(err == CELL_FS_OK, "cellFsAioRead", (unsigned)err);
        if (ok) {
            if (!request_wait(0))
                return abandon(0, path);
            check(req[0].err == CELL_FS_OK, "request 0 err == OK",
                  (unsigned)req[0].err);
            check(req[0].bytes == CHUNK_SIZE, "request 0 bytes == 64 KiB",
                  (unsigned long long)req[0].bytes);
            check(count_bad(req_buf[0], CHUNK_SIZE, CHUNK_SIZE / 4) == 0,
                  "request 0 data matches pattern", 0);
        }
    }

    /* --- 4. AIO cancel ------------------------------------------- */
    if (ok) {
        printf("\n--- AIO cancel ---\n");
        int id = -1;
        request_init(1, fd, 0);
        err = cellFsAioRead(&req[1].aio, &id, aio_callback);
        ok = check(err == CELL_FS_OK, "cellFsAioRead (cancel test)", (unsigned)err);
        if (ok) {
            CellFsErrno cerr = cellFsAioCancel(id);
            printf("    cellFsAioCancel(%d) -> 0x%08x\n", id, (unsigned)cerr);
            if (!request_wait(1))
                return abandon(1, path);
            printf("    request 1 completed: err=0x%08x size=%llu\n",
                   (unsigned)req[1].err, (unsigned long long)req[1].bytes);
        }
    }

    /* --- 5. streaming read --------------------------------------- */
    if (ok)
        stream_test(fd);

    /* --- 6. cleanup: no request is in flight here ---------------- */
    if (aio_inited) {
        err = cellFsAioFinish(AIO_MOUNT);
        check(err == CELL_FS_OK, "cellFsAioFinish", (unsigned)err);
    }
    if (fd >= 0) {
        err = cellFsClose(fd);
        check(err == CELL_FS_OK, "cellFsClose(read fd)", (unsigned)err);
    }
    err = cellFsUnlink(path);
    check(err == CELL_FS_OK, "cellFsUnlink(own scratch file)", (unsigned)err);
    if (stray_callbacks)
        check(0, "no unexpected AIO callbacks", (unsigned)stray_callbacks);

    return verdict();
}
