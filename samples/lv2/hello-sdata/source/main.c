/*
 * hello-sdata: developer SDATA files read through cellFsSdataOpen.
 *
 * Each embedded SDATA file (made by make_sdata at build time) is written
 * to /dev_hdd0/tmp, opened with cellFsSdataOpen and read in odd-sized
 * pieces; the data must equal the embedded payload, and reading past it
 * must return nothing.  Prints one line per file and a summary:
 *   HELLO_SDATA <name> OK | FAIL <step> ...
 *   HELLO_SDATA DONE passed=<n> of <n>
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/process.h>
#include <cell/cell_fs.h>

#include "payload_bin.h"
#include "data_v4_sdat.h"
#include "data_v3_sdat.h"
#include "data_v2_sdat.h"
#include "data_b1_sdat.h"
#include "data_z_sdat.h"

SYS_PROCESS_PARAM(1001, 0x100000);

static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    puts(line);
    fflush(stdout);
}

static const struct {
    const char *name;
    const unsigned char *data;
    const unsigned int *size;
} kFiles[] = {
    { "v4", data_v4_sdat, &data_v4_sdat_size },
    { "v3", data_v3_sdat, &data_v3_sdat_size },
    { "v2", data_v2_sdat, &data_v2_sdat_size },
    { "b1", data_b1_sdat, &data_b1_sdat_size },
    { "z", data_z_sdat, &data_z_sdat_size },
};

static unsigned char s_read[48 * 1024];

static int check(const char *name, const unsigned char *file, unsigned int fileSize)
{
    char path[64];
    int fd = -1;
    uint64_t n = 0, got = 0;
    CellFsErrno rc;

    snprintf(path, sizeof path, "/dev_hdd0/tmp/hello_sdata_%s.sdat", name);
    rc = cellFsOpen(path, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED) { say("HELLO_SDATA %s FAIL write-open rc=%#x", name, rc); return 0; }
    rc = cellFsWrite(fd, file, fileSize, &n);
    cellFsClose(fd);
    if (rc != CELL_FS_SUCCEEDED || n != fileSize) { say("HELLO_SDATA %s FAIL write rc=%#x n=%llu", name, rc, (unsigned long long)n); return 0; }

    rc = cellFsSdataOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (rc != CELL_FS_SUCCEEDED) { say("HELLO_SDATA %s FAIL sdata-open rc=%#x", name, rc); return 0; }
    /* odd-sized pieces cross every block boundary at a different offset */
    memset(s_read, 0xa5, sizeof s_read);
    for (;;) {
        uint64_t want = 1000;
        if (got + want > sizeof s_read) want = sizeof s_read - got;
        if (want == 0) break;
        rc = cellFsRead(fd, s_read + got, want, &n);
        if (rc != CELL_FS_SUCCEEDED) { say("HELLO_SDATA %s FAIL read rc=%#x at %llu", name, rc, (unsigned long long)got); cellFsClose(fd); return 0; }
        if (n == 0) break;
        got += n;
    }
    cellFsClose(fd);
    cellFsUnlink(path);

    if (got != payload_bin_size) { say("HELLO_SDATA %s FAIL length got=%llu want=%u", name, (unsigned long long)got, payload_bin_size); return 0; }
    for (uint64_t i = 0; i < got; ++i)
        if (s_read[i] != payload_bin[i]) {
            say("HELLO_SDATA %s FAIL data at %llu got=%#x want=%#x", name, (unsigned long long)i, s_read[i], payload_bin[i]);
            return 0;
        }
    say("HELLO_SDATA %s OK (%u byte file, %llu bytes of data)", name, fileSize, (unsigned long long)got);
    return 1;
}

int main(void)
{
    const unsigned total = sizeof kFiles / sizeof kFiles[0];
    unsigned passed = 0;
    say("hello-sdata: %u developer SDATA files, payload %u bytes", total, payload_bin_size);
    for (unsigned i = 0; i < total; ++i)
        passed += check(kFiles[i].name, kFiles[i].data, *kFiles[i].size);
    say("HELLO_SDATA DONE passed=%u of %u", passed, total);
    return passed == total ? 0 : 1;
}
