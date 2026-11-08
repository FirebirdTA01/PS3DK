/* Host-test syscall seam: each call consumes the next action of its own queue from LM_FAULTS, then the queue default
 * is the real call.  LM_FAULTS = "open=A,A..;write=..;fsync=..;close=.." with actions
 *   ok          the real call
 *   err:<v>     no real call (close: the real close FIRST, then the failure), errno = (int)v, return -1
 *   part:<k>    write only: the real write of min(k, n) bytes, returning that count
 *   eintr       write only: errno = EINTR, return -1, nothing written
 *   zero        write only: return 0, nothing written
 * Every call is appended to LM_TRACE as one line "<call> <args> -> <rc> <errno>".  An unparsable LM_FAULTS aborts. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lm_seam.h"

#undef open
#undef write
#undef fsync
#undef close

enum { Q_OPEN, Q_WRITE, Q_FSYNC, Q_CLOSE, Q_N };
static const char *qname[Q_N] = {"open", "write", "fsync", "close"};
static char act[Q_N][16][24];
static int nact[Q_N], pos[Q_N], loaded;

static void load(void)
{
    const char *f = getenv("LM_FAULTS");
    char buf[1024], *grp, *sg;

    loaded = 1;
    if (!f || !*f)
        return;
    if (strlen(f) >= sizeof buf)
        abort();
    strcpy(buf, f);
    for (grp = strtok_r(buf, ";", &sg); grp; grp = strtok_r(NULL, ";", &sg)) {
        char *eq = strchr(grp, '='), *a, *sa;
        int q;
        if (!eq)
            abort();
        *eq = '\0';
        for (q = 0; q < Q_N && strcmp(grp, qname[q]) != 0; q++)
            ;
        if (q == Q_N)
            abort();
        for (a = strtok_r(eq + 1, ",", &sa); a; a = strtok_r(NULL, ",", &sa)) {
            if (nact[q] == 16 || strlen(a) >= sizeof act[q][0])
                abort();
            strcpy(act[q][nact[q]++], a);
        }
    }
}

static const char *next(int q)
{
    if (!loaded)
        load();
    return pos[q] < nact[q] ? act[q][pos[q]++] : "ok";
}

static int err_of(const char *a)
{
    char *end;
    unsigned long v = strtoul(a + 4, &end, 0);
    if (strncmp(a, "err:", 4) != 0 || *end != '\0')
        abort();
    return (int)(unsigned)v;
}

static void trace(const char *what, long rc, int e)
{
    const char *t = getenv("LM_TRACE");
    FILE *f;
    if (!t)
        return;
    f = fopen(t, "a");
    if (!f)
        abort();
    fprintf(f, "%s -> %ld %d\n", what, rc, rc < 0 ? e : 0);
    fclose(f);
}

int lm_seam_open(const char *path, int flags, int mode)
{
    const char *a = next(Q_OPEN);
    int fd, e = 0;
    if (strcmp(a, "ok") == 0) {
        fd = open(path, flags, mode);
        e = errno;
    } else {
        fd = -1;
        e = err_of(a);
    }
    char w[32];
    const char *seq = strrchr(path, '-');
    snprintf(w, sizeof w, "open %.2s", seq ? seq + 1 : "??"); /* the marker number */
    trace(w, fd, e);
    errno = e;
    return fd;
}

ssize_t lm_seam_write(int fd, const void *buf, size_t n)
{
    const char *a = next(Q_WRITE);
    ssize_t r;
    int e = 0;
    if (strcmp(a, "ok") == 0) {
        r = write(fd, buf, n);
        e = errno;
    } else if (strncmp(a, "part:", 5) == 0) {
        size_t k = strtoul(a + 5, NULL, 10);
        r = write(fd, buf, k < n ? k : n);
        e = errno;
    } else if (strcmp(a, "eintr") == 0) {
        r = -1;
        e = EINTR;
    } else if (strcmp(a, "zero") == 0) {
        r = 0;
    } else {
        r = -1;
        e = err_of(a);
    }
    char w[32];
    snprintf(w, sizeof w, "write %zu", n);
    trace(w, (long)r, e);
    errno = e;
    return r;
}

int lm_seam_fsync(int fd)
{
    const char *a = next(Q_FSYNC);
    int r, e = 0;
    if (strcmp(a, "ok") == 0) {
        r = fsync(fd);
        e = errno;
    } else {
        r = -1;
        e = err_of(a);
    }
    trace("fsync", r, e);
    errno = e;
    return r;
}

int lm_seam_close(int fd)
{
    const char *a = next(Q_CLOSE);
    int r = close(fd), e = errno; /* the descriptor is always released */
    if (strcmp(a, "ok") != 0) {
        r = -1;
        e = err_of(a);
    }
    trace("close", r, e);
    errno = e;
    return r;
}
