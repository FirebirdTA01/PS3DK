/*
 * hello-ppu-cellfs — file-system surface smoke test.
 *
 * Round-trips a small payload through cellFs:
 *
 *   1. cellFsOpen   /dev_hdd0/tmp/ps3tc_cellfs.txt  (CREAT|TRUNC|WRONLY)
 *   2. cellFsWrite  payload                          (writes N bytes)
 *   3. cellFsClose
 *   4. cellFsStat   verifies file size matches payload
 *   5. cellFsOpen   same path                        (RDONLY)
 *   6. cellFsRead   reads back into a buffer
 *   7. cellFsClose
 *   8. memcmp       payload == read-back
 *   9. cellFsUnlink cleans up the scratch file
 *
 * Each step prints its CellFsErrno return so a failed step is visible
 * on the RPCS3 TTY console (or PS3 dev-tty).  A summary line at the
 * end declares overall pass/fail.
 *
 * Validates: <cell/cell_fs.h> type / errno / signature surface, and
 * libfs_stub.a NID linkage to the sys_fs SPRX module.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/process.h>
#include <cell/cell_fs.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define SCRATCH_PATH  "/dev_hdd0/tmp/ps3tc_cellfs.txt"

static const char k_payload[] =
    "PS3 Custom Toolchain — cellFs smoke test\n"
    "If you can read this, libfs_stub linked, the SPRX resolved every NID,\n"
    "and CellFsStat / CellFsErrno marshalled across the SPRX boundary cleanly.\n";

#define PAYLOAD_LEN  (sizeof(k_payload) - 1u)  /* exclude trailing NUL */

int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    printf("hello-ppu-cellfs: cellFs surface smoke test\n");
    printf("  scratch:   %s\n", SCRATCH_PATH);
    printf("  payload:   %u bytes\n", (unsigned)PAYLOAD_LEN);

    int          ok    = 1;
    CellFsErrno  err;
    int          fd_w  = -1;
    int          fd_r  = -1;
    uint64_t     wrote = 0;
    uint64_t     readn = 0;
    CellFsStat   sb;
    char         readback[256];

    /* 1. open for write (create + truncate). */
    err = cellFsOpen(SCRATCH_PATH,
                     CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_WRONLY,
                     &fd_w, NULL, 0);
    printf("  cellFsOpen(WRONLY|CREAT|TRUNC) -> err=0x%08x fd=%d\n",
           (unsigned)err, fd_w);
    if (err != CELL_FS_OK) { ok = 0; goto done; }

    /* 2. write payload. */
    err = cellFsWrite(fd_w, k_payload, (uint64_t)PAYLOAD_LEN, &wrote);
    printf("  cellFsWrite                    -> err=0x%08x wrote=%llu\n",
           (unsigned)err, (unsigned long long)wrote);
    if (err != CELL_FS_OK || wrote != PAYLOAD_LEN) { ok = 0; }

    /* 3. close write fd. */
    err = cellFsClose(fd_w);
    printf("  cellFsClose(write fd)          -> err=0x%08x\n", (unsigned)err);
    fd_w = -1;
    if (err != CELL_FS_OK) { ok = 0; }

    /* 4. stat. */
    memset(&sb, 0, sizeof(sb));
    err = cellFsStat(SCRATCH_PATH, &sb);
    printf("  cellFsStat                     -> err=0x%08x size=%llu mode=0%o\n",
           (unsigned)err, (unsigned long long)sb.st_size,
           (unsigned)sb.st_mode);
    if (err != CELL_FS_OK || sb.st_size != PAYLOAD_LEN) { ok = 0; }

    /* 5. open for read. */
    err = cellFsOpen(SCRATCH_PATH, CELL_FS_O_RDONLY,
                     &fd_r, NULL, 0);
    printf("  cellFsOpen(RDONLY)             -> err=0x%08x fd=%d\n",
           (unsigned)err, fd_r);
    if (err != CELL_FS_OK) { ok = 0; goto unlink; }

    /* 6. read back. */
    memset(readback, 0, sizeof(readback));
    err = cellFsRead(fd_r, readback, (uint64_t)sizeof(readback) - 1u, &readn);
    printf("  cellFsRead                     -> err=0x%08x readn=%llu\n",
           (unsigned)err, (unsigned long long)readn);
    if (err != CELL_FS_OK || readn != PAYLOAD_LEN) { ok = 0; }

    /* 7. close read fd. */
    err = cellFsClose(fd_r);
    printf("  cellFsClose(read fd)           -> err=0x%08x\n", (unsigned)err);
    fd_r = -1;
    if (err != CELL_FS_OK) { ok = 0; }

    /* 8. compare payload. */
    if (memcmp(k_payload, readback, PAYLOAD_LEN) == 0) {
        printf("  payload memcmp                 -> match\n");
    } else {
        printf("  payload memcmp                 -> MISMATCH\n");
        ok = 0;
    }

unlink:
    /* 9. unlink scratch file. */
    err = cellFsUnlink(SCRATCH_PATH);
    printf("  cellFsUnlink                   -> err=0x%08x\n", (unsigned)err);
    if (err != CELL_FS_OK) { ok = 0; }

done:
    if (fd_w >= 0) cellFsClose(fd_w);
    if (fd_r >= 0) cellFsClose(fd_r);

    printf("hello-ppu-cellfs: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
