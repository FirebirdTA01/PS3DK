/* Launch-marker probe L (localization probe design r2, variant L): the smallest program on the clock probe's launch
 * route.  main writes marker 01, then marker 17, and (only if 17 fails) one marker 99, then returns: no SPU, no GCM,
 * no decrementer write, no CSV.  UNVALIDATED ON TARGET: only host behaviour is tested.
 *
 * What a run can show: a VALID marker 01 shows execution reached main and completed that record's write.  Only a
 * VALID marker 17 with result=ok shows that marker 01's fsync and close also returned success (a valid 17 carrying
 * an error shows the failing call instead).  A valid 99 shows marker 17's own failure.  An absent or invalid marker
 * proves nothing (filesystem and power-loss behaviour are unverified); missing files are unknown, not failures.  L
 * does not show the clock probe's loader route and makes no durability claim; no wall-time bound is claimed for any
 * file call.
 *
 * Record (one file per marker, created O_EXCL, written once): <LM_DIR>/clockcal-<run>-<NN>.mk holding
 *   "CLKM1 run=<run> variant=L build=<build> seq=<NN> stage=<stage> result=<result> counts=0,0 len=<n>\n"
 * <run> = the timebase at main entry (16 hex digits); <build> = LM_BUILD_ID, exactly 16 hex digits (a source-hash
 * prefix: a marker tag, not the build identity; the artifact manifest is that); <n> = the byte length of everything
 * before " len=".  Stages: 01 main-entered (result ok); 17 marker-closed (ok, or marker 01's FIRST error); 99
 * logging-failed (marker 17's FIRST error; never ok).  Error results: error:<open|write|fsync|close>:<errno as 8 hex
 * digits> (LV2 errno values are 0x8001xxxx), error:write:zero (a write returned 0), error:format:<record|path>.
 *
 * Per marker: open; write until done (a short write continues with the rest; EINTR on write is retried; any other
 * failure or a zero return stops writing); fsync only after a complete write; close exactly once after a successful
 * open, whatever happened before.  fsync and close failures are reported, never retried.  The FIRST error is kept. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef LM_HOST_TEST /* host tests only: a fixed "timebase", LM_DIR of the test's choosing, a syscall seam */
#include "lm_seam.h"
#define LM_TB() ((uint64_t)(LM_HOST_TEST))
#else
#include <sys/process.h>
SYS_PROCESS_PARAM(1001, 0x100000); /* the clock probe's priority and stack */
#define LM_TB() __builtin_ppc_get_timebase()
#define LM_DIR "/dev_hdd0/tmp"
#endif

#ifndef LM_BUILD_ID
#error "LM_BUILD_ID must be defined by the build (16 lowercase hex digits)"
#endif
_Static_assert(sizeof(LM_BUILD_ID) == 17, "LM_BUILD_ID must be exactly 16 hex digits");

/* the longest record: 126 bytes before " len=" (stage logging-failed, a 20-byte error result), then " len=126\n" */
#define LM_REC_MAX 160

typedef struct {
    char text[32]; /* the first error as its result text ("error:..."); empty = none */
} lm_err;

static void lm_fail(lm_err *e, const char *call, const char *code)
{
    if (e->text[0] == '\0')
        snprintf(e->text, sizeof e->text, "error:%s:%s", call, code);
}

static void lm_fail_errno(lm_err *e, const char *call, int err)
{
    char code[12];
    snprintf(code, sizeof code, "%08x", (unsigned)(uint32_t)err);
    lm_fail(e, call, code);
}

/* write one marker; 0 on success, else the first error is in *e */
static int lm_marker(uint64_t run, unsigned seq, const char *stage, const char *result, lm_err *e)
{
    char path[512], rec[LM_REC_MAX];
    int n, m, fd;
    size_t off = 0, len;

    memset(e, 0, sizeof *e);
    n = snprintf(rec, sizeof rec, "CLKM1 run=%016llx variant=L build=%s seq=%02u stage=%s result=%s counts=0,0",
                 (unsigned long long)run, LM_BUILD_ID, seq, stage, result);
    m = n < 0 || (size_t)n >= sizeof rec ? -1 : snprintf(rec + n, sizeof rec - (size_t)n, " len=%d\n", n);
    if (m < 0 || (size_t)m >= sizeof rec - (size_t)n) {
        lm_fail(e, "format", "record");
        return -1;
    }
    len = (size_t)n + (size_t)m;
    n = snprintf(path, sizeof path, LM_DIR "/clockcal-%016llx-%02u.mk", (unsigned long long)run, seq);
    if (n < 0 || (size_t)n >= sizeof path) {
        lm_fail(e, "format", "path");
        return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        lm_fail_errno(e, "open", errno);
        return -1;
    }
    while (off < len) {
        ssize_t w = write(fd, rec + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            lm_fail_errno(e, "write", errno);
            break;
        }
        if (w == 0) {
            lm_fail(e, "write", "zero");
            break;
        }
        off += (size_t)w;
    }
    if (off == len && fsync(fd) != 0)
        lm_fail_errno(e, "fsync", errno);
    if (close(fd) != 0)
        lm_fail_errno(e, "close", errno);
    return e->text[0] ? -1 : 0;
}

int main(void)
{
    uint64_t run = LM_TB();
    lm_err e1, e2, e3;
    int r1, r2;

    r1 = lm_marker(run, 1, "main-entered", "ok", &e1);
    r2 = lm_marker(run, 17, "marker-closed", r1 == 0 ? "ok" : e1.text, &e2);
    if (r2 != 0)
        (void)lm_marker(run, 99, "logging-failed", e2.text, &e3);
    return (r1 == 0 && r2 == 0) ? 0 : 1;
}
