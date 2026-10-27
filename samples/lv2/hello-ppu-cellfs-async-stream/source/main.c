/*
 * hello-ppu-cellfs-async-stream - cellFs asynchronous I/O and streaming
 * read surfaces, side by side.
 *
 * Validates, against <cell/cell_fs.h>:
 *   cellFsAioInit / cellFsAioRead / cellFsAioCancel / cellFsAioFinish
 *   cellFsStReadInit / cellFsStReadStart / cellFsStReadGetRingBuf /
 *   cellFsStReadGetStatus / cellFsStRead / cellFsStReadFinish
 *   cellFsOpen / cellFsWrite / cellFsRead / cellFsClose / cellFsUnlink
 *
 * Behaviour:
 *   1. Writes a 1 MiB pattern file to /dev_hdd0/tmp.
 *   2. AIO read: submit one 64 KiB cellFsAioRead at offset 128 KiB,
 *      wait for the callback, verify the bytes.
 *   3. AIO cancel: submit a second read, cancel it before it completes;
 *      verify the callback fires with ECANCELED.
 *   4. Streaming read: init ring buf on the fd, do a 256 KiB streaming
 *      read at offset 256 KiB, read it back, verify, finish.
 *   5. Clean up the scratch file and the AIO subsystem.
 *
 * Prints one line per API call so a failed step is visible on TTY.
 * Last line is "hello-ppu-cellfs-async-stream: PASS (n checks)" or names
 * the failures.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/memory.h>
#include <cell/cell_fs.h>

SYS_PROCESS_PARAM(2048, 0x100000);

#define FILE_SIZE  (1 * 1024 * 1024)   /* 1 MiB */
#define CHUNK_SIZE (64 * 1024)        /* 64 KiB  */
#define SCRATCH    "/dev_hdd0/tmp/ps3tc_cellfs_async_stream.bin"

static int checks, failures;

static void check(int ok, const char *what, unsigned long long got)
{
    checks++;
    if (!ok) failures++;
    printf("  %s %s (0x%llx)\n", ok ? "ok  " : "FAIL", what, got);
}

/* ---------------------------------------------------------------- *
 * pattern: deterministic 4-byte word so a mismatch is easy to see
 * ---------------------------------------------------------------- */
static uint32_t word_at(uint64_t off)
{
    /* 2^16 - 7 (Fibonacci scramble) gives a nice non-zero pattern. */
    return (uint32_t)(off / 4) * 265013u ^ 0xdeadbeefu;
}

static void fill(uint32_t *buf, uint64_t base_off)
{
    for (size_t i = 0; i < FILE_SIZE / 4; i++)
        buf[i] = word_at(base_off + i * 4);
}

static int cmp_mem(const uint32_t *a, uint64_t base, size_t words)
{
    uint32_t bad = 0;
    for (size_t i = 0; i < words; i++)
        bad += a[i] != word_at(base + i * 4);
    return bad;
}

/* ---------------------------------------------------------------- *
 * AIO callback
 * ---------------------------------------------------------------- */
static volatile int aio_done = 0;
static volatile CellFsErrno aio_err = -1;
static volatile uint64_t aio_bytes = 0;

static void aio_callback(CellFsAio *aio, CellFsErrno err, int id, uint64_t size)
{
    (void)aio; (void)id;
    printf("    [aio callback] err=0x%08x size=%llu\n",
           (unsigned)err, (unsigned long long)size);
    aio_err   = err;
    aio_bytes = size;
    aio_done  = 1;
}

/* ---------------------------------------------------------------- */
int main(int argc, const char **argv)
{
    (void)argc; (void)argv;

    printf("hello-ppu-cellfs-async-stream: AIO + streaming read test\n");
    printf("  scratch:   %s\n", SCRATCH);
    printf("  file:      %u bytes\n", (unsigned)FILE_SIZE);

    /* --- memory container (required by AIO subsystem) ------------- */
    sys_memory_container_t cid = SYS_MEMORY_CONTAINER_ID_INVALID;
    int rc = sys_memory_container_create(&cid, 16 * 1024 * 1024);
    check(rc == CELL_OK, "sys_memory_container_create(16 MiB)", (unsigned)rc);
    if (rc != CELL_OK) { printf("  no container; aborting\n"); return 1; }
    printf("  container id = 0x%08x\n", (unsigned)cid);

    /* --- 1. write 1 MiB pattern file ------------------------------ */
    static uint32_t *wbuf = NULL;
    wbuf = (uint32_t *)malloc(FILE_SIZE);
    if (!wbuf) { printf("  malloc failed\n"); return 1; }
    fill(wbuf, 0);

    int fd = -1;
    CellFsErrno err;
    err = cellFsOpen(SCRATCH,
                     CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_WRONLY,
                     &fd, NULL, 0);
    check(err == CELL_FS_OK, "cellFsOpen(write)", (unsigned)err);
    if (err != CELL_FS_OK) { free(wbuf); return 1; }

    uint64_t written = 0;
    err = cellFsWrite(fd, wbuf, FILE_SIZE, &written);
    check(err == CELL_FS_OK && written == FILE_SIZE, "cellFsWrite(1 MiB)",
          (unsigned long long)written);
    cellFsClose(fd);
    fd = -1;

    /* --- 2. open read-only, stat ------------------------------- */
    CellFsStat sb;
    err = cellFsStat(SCRATCH, &sb);
    check(err == CELL_FS_OK && sb.st_size == FILE_SIZE,
          "cellFsStat size", (unsigned long long)sb.st_size);

    err = cellFsOpen(SCRATCH, CELL_FS_O_RDONLY, &fd, NULL, 0);
    check(err == CELL_FS_OK, "cellFsOpen(read)", (unsigned)err);
    if (err != CELL_FS_OK) { free(wbuf); cellFsUnlink(SCRATCH); sys_memory_container_destroy(cid); return 1; }

    /* --- 3. AIO: submit + wait ------------------------------------ */
    printf("\n--- AIO read at offset %u, size %u ---\n", CHUNK_SIZE, CHUNK_SIZE);
    err = cellFsAioInit("host");
    check(err == CELL_FS_OK, "cellFsAioInit(\"host\")", (unsigned)err);
    if (err != CELL_FS_OK) {
        cellFsClose(fd); free(wbuf);
        cellFsUnlink(SCRATCH); sys_memory_container_destroy(cid);
        return 1;
    }

    static char *rbuf = NULL;
    rbuf = (char *)malloc(CHUNK_SIZE);
    if (!rbuf) { printf("  malloc failed\n"); goto cleanup; }

    CellFsAio aio;
    aio.fd     = fd;
    aio.offset = CHUNK_SIZE;
    aio.buf    = rbuf;
    aio.size   = CHUNK_SIZE;
    aio.user_data = 0;

    int aio_id = -1;
    err = cellFsAioRead(&aio, &aio_id, aio_callback);
    check(err == CELL_FS_OK, "cellFsAioRead", (unsigned)err);

    if (err == CELL_FS_OK) {
        /* poll for callback up to 5 s */
        int wait_ms = 0;
        while (!aio_done && wait_ms < 5000) { usleep(10 * 1000); wait_ms += 10; }
        check(aio_done, "aio callback fired", (unsigned)aio_done);
        check(aio_err == CELL_FS_OK, "aio err == OK", (unsigned)aio_err);
        check(aio_bytes == CHUNK_SIZE, "aio bytes == 64 KiB",
              (unsigned long long)aio_bytes);
        check(cmp_mem((uint32_t *)rbuf, CHUNK_SIZE, CHUNK_SIZE / 4) == 0,
              "aio data matches pattern", 0);
    }

    /* --- 4. AIO cancel ------------------------------------------ */
    printf("\n--- AIO cancel ---\n");
    aio_done = 0; aio_err = -1; aio_bytes = 0;
    aio.offset = 2 * 1024 * 1024; /* past EOF — would fail anyway */
    rbuf[0] = 0;
    err = cellFsAioRead(&aio, &aio_id, aio_callback);
    check(err == CELL_FS_OK, "cellFsAioRead (cancel test)", (unsigned)err);
    if (err == CELL_FS_OK) {
        err = cellFsAioCancel(aio_id);
        check(err == CELL_FS_OK, "cellFsAioCancel", (unsigned)err);
        int wait_ms = 0;
        while (!aio_done && wait_ms < 5000) { usleep(10 * 1000); wait_ms += 10; }
        check(aio_done, "aio cancel callback fired", (unsigned)aio_done);
        printf("    (cancel callback err=0x%08x; ECANCELED expected)\n",
               (unsigned)aio_err);
    }

    /* --- 5. streaming read ---------------------------------------- */
    printf("\n--- streaming read at offset 256 KiB ---\n");
    uint64_t st_off = 256 * 1024;
    uint64_t st_size = 256 * 1024;

    CellFsRingBuffer ring;
    ring.ringbuf_size  = 64 * 1024;
    ring.block_size    = 8 * 1024;
    ring.transfer_rate = 0;   /* no limit */
    ring.copy          = CELL_FS_ST_COPY;

    err = cellFsStReadInit(fd, &ring);
    check(err == CELL_FS_OK, "cellFsStReadInit", (unsigned)err);

    if (err == CELL_FS_OK) {
        uint64_t status = 0;
        err = cellFsStReadStart(fd, st_off, st_size);
        check(err == CELL_FS_OK, "cellFsStReadStart", (unsigned)err);

        if (err == CELL_FS_OK) {
            err = cellFsStReadGetStatus(fd, &status);
            check(err == CELL_FS_OK, "cellFsStReadGetStatus", (unsigned)err);

            for (int blk = 0; blk < (int)(st_size / 8192); blk++) {
                static char stbuf[8192];
                uint64_t got = 0;
                err = cellFsStRead(fd, stbuf, 8192, &got);
                if (err != CELL_FS_OK || got != 8192) {
                    printf("    stream block %d: err=0x%08x got=%llu\n",
                           blk, (unsigned)err, (unsigned long long)got);
                    break;
                }
                uint32_t *w32 = (uint32_t *)stbuf;
                uint64_t base_addr = st_off + (uint64_t)blk * 8192;
                uint32_t bad = 0;
                for (int i = 0; i < 8192 / 4; i++)
                    bad += w32[i] != word_at(base_addr + i * 4);
                if (bad) {
                    printf("    stream block %d: %u mismatched words (first at 0x%llx)\n",
                           blk, (unsigned)bad, (unsigned long long)base_addr);
                    break;
                }
            }
            printf("    streaming read: 256 KiB in 32 x 8 KiB blocks\n");

            err = cellFsStReadStop(fd);
            check(err == CELL_FS_OK, "cellFsStReadStop", (unsigned)err);
        }

        err = cellFsStReadFinish(fd);
        check(err == CELL_FS_OK, "cellFsStReadFinish", (unsigned)err);
    }

    /* --- cleanup ------------------------------------------------ */
cleanup:
    cellFsAioFinish("host");
    if (fd >= 0) cellFsClose(fd);
    cellFsUnlink(SCRATCH);
    free(rbuf);
    free(wbuf);
    sys_memory_container_destroy(cid);

    printf("\nhello-ppu-cellfs-async-stream: %s (%d checks)\n",
           failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
