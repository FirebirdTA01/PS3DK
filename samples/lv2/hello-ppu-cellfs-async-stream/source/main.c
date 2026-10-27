/*
 * hello-ppu-cellfs-async-stream - cellFs asynchronous I/O and streaming
 * read surfaces, side by side.
 *
 * Validates, against <cell/cell_fs.h>:
 *   cellFsAioInit / cellFsAioRead / cellFsAioCancel / cellFsAioFinish
 *   cellFsStReadInit / cellFsStReadStart / cellFsStReadGetStatus /
 *   cellFsStReadWait / cellFsStRead / cellFsStReadStop /
 *   cellFsStReadFinish
 *   cellFsOpen / cellFsWrite / cellFsStat / cellFsClose / cellFsUnlink
 *
 * Behaviour:
 *   1. Writes a 1 MiB pattern file to /dev_hdd0/tmp.
 *   2. AIO read: initialises AIO on the /dev_hdd0 mount, submits one
 *      64 KiB cellFsAioRead at offset 64 KiB, waits for the callback and
 *      verifies the bytes.
 *   3. AIO cancel: submits a second read and cancels it at once. Whether
 *      the cancel wins the race, and the error the callback then gets,
 *      are printed but not judged.
 *   4. Streaming read: a 64 KiB ring of 8 KiB blocks on the same fd,
 *      256 KiB streamed from offset 256 KiB as 32 blocks, each waited
 *      for, read and checked word by word.
 *   5. Removes the scratch file and finishes AIO.
 *
 * Prints one line per checked call so a failed step is visible on TTY.
 * The last line is "hello-ppu-cellfs-async-stream: PASS (n of m checks)"
 * or FAIL with the same counts.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <cell/cell_fs.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define FILE_SIZE   (1 * 1024 * 1024)   /* 1 MiB  */
#define CHUNK_SIZE  (64 * 1024)         /* 64 KiB */
#define ST_BLOCK    (8 * 1024)          /* 8 KiB  */
#define ST_OFFSET   (256 * 1024)
#define ST_SIZE     (256 * 1024)
#define AIO_MOUNT   "/dev_hdd0"
#define SCRATCH     "/dev_hdd0/tmp/ps3tc_cellfs_async_stream.bin"

static int checks, failures;

static int check(int ok, const char *what, unsigned long long got)
{
    checks++;
    if (!ok) failures++;
    printf("  %s %s (0x%llx)\n", ok ? "ok  " : "FAIL", what, got);
    return ok;
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

static volatile int         aio_done;
static volatile CellFsErrno aio_err;
static volatile uint64_t    aio_bytes;

static void aio_callback(CellFsAio *aio, CellFsErrno err, int id, uint64_t size)
{
    (void)aio; (void)id;
    printf("    [aio callback] err=0x%08x size=%llu\n",
           (unsigned)err, (unsigned long long)size);
    aio_err   = err;
    aio_bytes = size;
    aio_done  = 1;
}

static void aio_wait(int max_ms)
{
    for (int ms = 0; !aio_done && ms < max_ms; ms += 10)
        usleep(10 * 1000);
}

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

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;

    printf("hello-ppu-cellfs-async-stream: AIO + streaming read test\n");
    printf("  scratch:   %s\n", SCRATCH);
    printf("  file:      %u bytes\n", (unsigned)FILE_SIZE);

    /* --- 1. write the 1 MiB pattern file ------------------------- */
    uint32_t *wbuf = (uint32_t *)malloc(FILE_SIZE);
    char *rbuf = (char *)malloc(CHUNK_SIZE);
    if (!wbuf || !rbuf) {
        printf("  malloc failed\n");
        free(wbuf); free(rbuf);
        return 1;
    }
    for (size_t i = 0; i < FILE_SIZE / 4; i++)
        wbuf[i] = word_at(i * 4);

    int fd = -1;
    int aio_inited = 0;
    CellFsErrno err = cellFsOpen(SCRATCH,
                                 CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_WRONLY,
                                 &fd, NULL, 0);
    if (!check(err == CELL_FS_OK, "cellFsOpen(write)", (unsigned)err)) {
        free(wbuf); free(rbuf);
        return 1;
    }
    uint64_t written = 0;
    err = cellFsWrite(fd, wbuf, FILE_SIZE, &written);
    check(err == CELL_FS_OK && written == FILE_SIZE, "cellFsWrite(1 MiB)",
          (unsigned long long)written);
    cellFsClose(fd);
    fd = -1;
    free(wbuf);

    CellFsStat sb;
    memset(&sb, 0, sizeof(sb));
    err = cellFsStat(SCRATCH, &sb);
    check(err == CELL_FS_OK && sb.st_size == FILE_SIZE,
          "cellFsStat size", (unsigned long long)sb.st_size);

    /* --- 2. AIO read + wait --------------------------------------- */
    err = cellFsOpen(SCRATCH, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (!check(err == CELL_FS_OK, "cellFsOpen(read)", (unsigned)err))
        goto out;

    printf("\n--- AIO read at offset %u, size %u ---\n",
           (unsigned)CHUNK_SIZE, (unsigned)CHUNK_SIZE);
    err = cellFsAioInit(AIO_MOUNT);
    aio_inited = check(err == CELL_FS_OK, "cellFsAioInit(" AIO_MOUNT ")",
                       (unsigned)err);
    if (aio_inited) {
        CellFsAio aio;
        memset(&aio, 0, sizeof(aio));
        aio.fd        = fd;
        aio.offset    = CHUNK_SIZE;
        aio.buf       = rbuf;
        aio.size      = CHUNK_SIZE;
        aio.user_data = 0;

        int aio_id = -1;
        aio_done = 0; aio_err = -1; aio_bytes = 0;
        err = cellFsAioRead(&aio, &aio_id, aio_callback);
        if (check(err == CELL_FS_OK, "cellFsAioRead", (unsigned)err)) {
            aio_wait(5000);
            check(aio_done, "aio callback fired", (unsigned)aio_done);
            check(aio_err == CELL_FS_OK, "aio err == OK", (unsigned)aio_err);
            check(aio_bytes == CHUNK_SIZE, "aio bytes == 64 KiB",
                  (unsigned long long)aio_bytes);
            check(aio_done && count_bad((const uint32_t *)rbuf, CHUNK_SIZE,
                                        CHUNK_SIZE / 4) == 0,
                  "aio data matches pattern", 0);
        }

        /* --- 3. AIO cancel (race reported, not judged) ------------- */
        printf("\n--- AIO cancel ---\n");
        aio_done = 0; aio_err = -1; aio_bytes = 0;
        aio.offset = 0;
        err = cellFsAioRead(&aio, &aio_id, aio_callback);
        if (check(err == CELL_FS_OK, "cellFsAioRead (cancel test)", (unsigned)err)) {
            CellFsErrno cerr = cellFsAioCancel(aio_id);
            printf("    cellFsAioCancel -> 0x%08x\n", (unsigned)cerr);
            /* Wait either way so the request is no longer in flight
             * when rbuf is freed. */
            aio_wait(2000);
            printf("    after cancel: callback %s, err=0x%08x size=%llu\n",
                   aio_done ? "fired" : "not fired", (unsigned)aio_err,
                   (unsigned long long)aio_bytes);
        }
    }

    /* --- 4. streaming read ---------------------------------------- */
    stream_test(fd);

out:
    /* --- 5. cleanup ----------------------------------------------- */
    if (fd >= 0) cellFsClose(fd);
    err = cellFsUnlink(SCRATCH);
    check(err == CELL_FS_OK, "cellFsUnlink(scratch)", (unsigned)err);
    if (aio_inited) {
        err = cellFsAioFinish(AIO_MOUNT);
        check(err == CELL_FS_OK, "cellFsAioFinish", (unsigned)err);
    }
    free(rbuf);

    printf("\nhello-ppu-cellfs-async-stream: %s (%d of %d checks)\n",
           failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
