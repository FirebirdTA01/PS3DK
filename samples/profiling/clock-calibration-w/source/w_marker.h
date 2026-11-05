/* W/W0 checkpoint markers (design r4 2afa7bf3 + review 19:47): the CLKW1 sidecar.  Included by main.c only.
 *
 * One file per ATTEMPTED marker, <W_DIR>/clockcal-<run>-<NN>.mk, created O_EXCL and written once:
 *   CLKW1 run=<16hex> variant=<W|W0> build=<16hex> seq=<NN> stage=<name> result=<R> tb=<16hex>
 *     prev=<none|NN:IO:16hex> unres=<s>,<r> counts=<n>,<m>[ stage fields] len=<prefix bytes>\n
 * tb is read when the record is formatted (before its own open).  prev names the previous ATTEMPTED marker, its
 * first I/O error (or ok) and the timebase read right after its LAST attempted call returned.  No record attests its
 * own I/O.  IO and error results: ok | error:<open|write|fsync|close>:<8hex errno> | error:write:zero |
 * error:format:<record|path> (no errno is manufactured for the last two).
 *
 * Per marker: open; write until done (short writes continue; EINTR on write is retried; another failure or a zero
 * return stops writing); fsync only after a complete write; close exactly once after a successful open.  fsync and
 * close failures are reported, never retried.  The FIRST error is kept.
 *
 * A logging failure (any error above) ends normal markers for the run: exactly one stage-99 record is then
 * attempted (prev = the failed marker), never retried; w_logging_failed() tells the probe to stop issuing
 * measurement requests.  No time bound is claimed for any file call. */
#ifndef CLOCKCAL_W_MARKER_H
#define CLOCKCAL_W_MARKER_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef W_BUILD_ID
#error "W_BUILD_ID must be defined by the build (16 lowercase hex digits)"
#endif
_Static_assert(sizeof(W_BUILD_ID) == 17, "W_BUILD_ID must be exactly 16 hex digits");

/* the longest record (stage 16 or 12 with every field at its maximum) fits with margin; tested at the bound */
#define W_REC_MAX 400

typedef struct {
    char text[40]; /* the first error as a result/IO text ("error:..."); empty = none */
} w_err;

static uint64_t w_run;
static const char *w_variant;
static unsigned w_prev_seq;
static char w_prev_io[40];
static uint64_t w_prev_done;
static int w_have_prev, w_log_failed, w_99_done;
static char w_unres[2] = {'n', 'n'}; /* SPU, RSX: n none | u submitted, no trusted completion | q outcome unknown */
static unsigned w_count[2];

static void w_fail_text(w_err *e, const char *call, const char *code)
{
    if (e->text[0] == '\0')
        snprintf(e->text, sizeof e->text, "error:%s:%s", call, code);
}

static void w_fail_errno(w_err *e, const char *call, int err)
{
    char code[12];
    snprintf(code, sizeof code, "%08x", (unsigned)(uint32_t)err);
    w_fail_text(e, call, code);
}

static int w_logging_failed(void) { return w_log_failed; }

/* writes one record; 0 on success, else *e holds the first error and *done the time after the last call */
static int w_emit(unsigned seq, const char *stage, const char *result, const char *fields, w_err *e, uint64_t *done)
{
    char path[512], rec[W_REC_MAX], prev[64];
    int n, m, fd;
    size_t off = 0, len;
    uint64_t tb = W_TB();

    memset(e, 0, sizeof *e);
    if (w_have_prev)
        snprintf(prev, sizeof prev, "%02u:%s:%016llx", w_prev_seq, w_prev_io, (unsigned long long)w_prev_done);
    else
        snprintf(prev, sizeof prev, "none");
    n = snprintf(rec, sizeof rec,
                 "CLKW1 run=%016llx variant=%s build=%s seq=%02u stage=%s result=%s tb=%016llx prev=%s unres=%c,%c "
                 "counts=%u,%u%s%s",
                 (unsigned long long)w_run, w_variant, W_BUILD_ID, seq, stage, result, (unsigned long long)tb, prev,
                 w_unres[0], w_unres[1], w_count[0], w_count[1], fields[0] ? " " : "", fields);
    m = n < 0 || (size_t)n >= sizeof rec ? -1 : snprintf(rec + n, sizeof rec - (size_t)n, " len=%d\n", n);
    if (m < 0 || (size_t)m >= sizeof rec - (size_t)n) {
        w_fail_text(e, "format", "record");
        *done = W_TB();
        return -1;
    }
    len = (size_t)n + (size_t)m;
    n = snprintf(path, sizeof path, W_DIR "/clockcal-%016llx-%02u.mk", (unsigned long long)w_run, seq);
    if (n < 0 || (size_t)n >= sizeof path) {
        w_fail_text(e, "format", "path");
        *done = W_TB();
        return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        w_fail_errno(e, "open", errno);
        *done = W_TB();
        return -1;
    }
    while (off < len) {
        ssize_t w = write(fd, rec + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            w_fail_errno(e, "write", errno);
            break;
        }
        if (w == 0) {
            w_fail_text(e, "write", "zero");
            break;
        }
        off += (size_t)w;
    }
    if (off == len && fsync(fd) != 0)
        w_fail_errno(e, "fsync", errno);
    if (close(fd) != 0)
        w_fail_errno(e, "close", errno);
    *done = W_TB(); /* after the LAST attempted call */
    return e->text[0] ? -1 : 0;
}

/* one milestone marker; after a logging failure nothing is written (the 99 was attempted once) */
static void w_mark(unsigned seq, const char *stage, const char *result, const char *fields)
{
    w_err e;
    uint64_t done = 0;
    if (w_log_failed)
        return;
    int rc = w_emit(seq, stage, result, fields, &e, &done);
    w_have_prev = 1;
    w_prev_seq = seq;
    w_prev_done = done;
    snprintf(w_prev_io, sizeof w_prev_io, "%s", rc == 0 ? "ok" : e.text);
    if (rc != 0) {
        w_log_failed = 1;
        if (!w_99_done) {
            w_99_done = 1;
            (void)w_emit(99, "logging-failed", "failed", "", &e, &done); /* its own outcome: UNOBSERVED */
        }
    }
}

#endif
