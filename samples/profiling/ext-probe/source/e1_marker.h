/* E1 checkpoint markers: the EXTP1 sidecar (design p3.3 E1 r2 section 1 + r3 section D + r4), derived from the W
 * probe's CLKW1 writer (clock-calibration-w/source/w_marker.h).  Included by main.c only.
 *
 * One file per ATTEMPTED marker, <run dir>/extp-<run>-<NN>.mk, created O_EXCL and written once:
 *   EXTP1 run=<16hex> build=<16hex> seq=<NN> stage=<name> result=<R> tb=<16hex> prev=<none|NN:IO:16hex>
 *     unres=<s>,<r> counts=<n>,<m>[ fields] len=<prefix bytes>\n
 * tb is read when the record is formatted (before its own open).  prev names the previous ATTEMPTED marker, its
 * FIRST I/O error (or ok) and the timebase read right after its LAST attempted call.  No record attests its own
 * I/O; an absent marker proves nothing.  IO: ok | error:<open|write|fsync|close>:<8hex errno> | error:write:zero |
 * error:format:<record|path>.  counts = <SPU events the trusted reports stated>,<GPU events retired>.
 *
 * Per marker: open; write until done (EINTR retried; another failure or a zero return stops writing); fsync only
 * after a complete write; close exactly once after a successful open.  fsync and close failures are reported, never
 * retried.  The FIRST error is kept.  A logging failure ends normal markers: exactly one stage-99 record is then
 * attempted (prev = the failed marker), never retried; e1_logging_failed() tells the probe to submit nothing new.
 * No time bound is claimed for any file call.  The file calls go through E1_OPEN / E1_WRITE / E1_FSYNC / E1_CLOSE
 * (the libc calls on the target; a fault-injecting seam in the host tests). */
#ifndef E1_MARKER_H
#define E1_MARKER_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef E1_BUILD_ID
#error "E1_BUILD_ID must be defined by the build (16 lowercase hex digits)"
#endif
_Static_assert(sizeof(E1_BUILD_ID) == 17, "E1_BUILD_ID must be exactly 16 hex digits");

#define E1_REC_MAX 400

typedef struct {
    char text[40]; /* the first error as an IO text ("error:..."); empty = none */
} e1_err;

static uint64_t e1_run;
static char e1_dir[256];
static unsigned e1_prev_seq;
static char e1_prev_io[40];
static uint64_t e1_prev_done;
static int e1_have_prev, e1_log_failed, e1_99_done;
static char e1_unres[2] = {'n', 'n'}; /* SPU n|u|q, RSX n|u: sticky, historical */
static unsigned e1_count[2];

static void e1_fail_text(e1_err *e, const char *call, const char *code)
{
    if (e->text[0] == '\0')
        snprintf(e->text, sizeof e->text, "error:%s:%s", call, code);
}

static void e1_fail_errno(e1_err *e, const char *call, int err)
{
    char code[12];
    snprintf(code, sizeof code, "%08x", (unsigned)(uint32_t)err);
    e1_fail_text(e, call, code);
}

static int e1_logging_failed(void) { return e1_log_failed; }

static int e1_emit(unsigned seq, const char *stage, const char *result, const char *fields, e1_err *e,
                   uint64_t *done)
{
    char path[512], rec[E1_REC_MAX], prev[64];
    int n, m, fd;
    size_t off = 0, len;
    uint64_t tb = E1_TB();

    memset(e, 0, sizeof *e);
    if (e1_have_prev)
        snprintf(prev, sizeof prev, "%02u:%s:%016llx", e1_prev_seq, e1_prev_io, (unsigned long long)e1_prev_done);
    else
        snprintf(prev, sizeof prev, "none");
    n = snprintf(rec, sizeof rec,
                 "EXTP1 run=%016llx build=%s seq=%02u stage=%s result=%s tb=%016llx prev=%s unres=%c,%c "
                 "counts=%u,%u%s%s",
                 (unsigned long long)e1_run, E1_BUILD_ID, seq, stage, result, (unsigned long long)tb, prev,
                 e1_unres[0], e1_unres[1], e1_count[0], e1_count[1], fields[0] ? " " : "", fields);
    m = n < 0 || (size_t)n >= sizeof rec ? -1 : snprintf(rec + n, sizeof rec - (size_t)n, " len=%d\n", n);
    if (m < 0 || (size_t)m >= sizeof rec - (size_t)n) {
        e1_fail_text(e, "format", "record");
        *done = E1_TB();
        return -1;
    }
    len = (size_t)n + (size_t)m;
    n = snprintf(path, sizeof path, "%s/extp-%016llx-%02u.mk", e1_dir, (unsigned long long)e1_run, seq);
    if (n < 0 || (size_t)n >= sizeof path) {
        e1_fail_text(e, "format", "path");
        *done = E1_TB();
        return -1;
    }
    fd = E1_OPEN(path, seq);
    if (fd < 0) {
        e1_fail_errno(e, "open", errno);
        *done = E1_TB();
        return -1;
    }
    while (off < len) {
        long w = E1_WRITE(fd, rec + off, len - off, seq);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            e1_fail_errno(e, "write", errno);
            break;
        }
        if (w == 0) {
            e1_fail_text(e, "write", "zero");
            break;
        }
        off += (size_t)w;
    }
    if (off == len && E1_FSYNC(fd, seq) != 0)
        e1_fail_errno(e, "fsync", errno);
    if (E1_CLOSE(fd, seq) != 0)
        e1_fail_errno(e, "close", errno);
    *done = E1_TB(); /* after the LAST attempted call */
    return e->text[0] ? -1 : 0;
}

/* one milestone marker; after a logging failure nothing is written (the 99 was attempted once) */
static void e1_mark(unsigned seq, const char *stage, const char *result, const char *fields)
{
    e1_err e;
    uint64_t done = 0;
    int rc;
    if (e1_log_failed)
        return;
    rc = e1_emit(seq, stage, result, fields, &e, &done);
    e1_have_prev = 1;
    e1_prev_seq = seq;
    e1_prev_done = done;
    snprintf(e1_prev_io, sizeof e1_prev_io, "%s", rc == 0 ? "ok" : e.text);
    if (rc != 0) {
        e1_log_failed = 1;
        if (!e1_99_done) {
            e1_99_done = 1;
            (void)e1_emit(99, "logging-failed", "failed", "", &e, &done); /* its own outcome: UNOBSERVED */
        }
    }
}

#endif
