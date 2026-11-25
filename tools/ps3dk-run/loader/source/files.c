/*
 * ps3dk-loader: file service on an authenticated session.
 *
 * One root (PRN_ROOT_TMP = /dev_hdd0/tmp).  OFF on cold start; ON only from the loader screen (CIRCLE); the host can
 * only turn it OFF.  Every OFF bumps files_gen, which cancels outstanding file work: a PUT publishes, and a DELETE
 * deletes, only under files_lock with the service ON and files_gen unchanged since the request started.
 * Paths: relative, printable ASCII, no "." / ".." / empty component, no backslash or colon; each component is stat'ed
 * while walking (directories, then the expected final type), and the walk is repeated right before a mutation.
 */
#include "loader.h"
#include "../../common/prn_net.h"
#include "../../common/prn_sha256.h"
#include "../../common/ps3dk_run_proto.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int files_on;
volatile unsigned files_gen;

#define CHUNK (64u * 1024u)
#define MAX_XFER PRN_MAX_FILE_SIZE

void loader_set_files(int on, const char *who)
{
    pthread_mutex_lock(&files_lock);
    if (!on) files_gen++;
    files_on = on;
    if (on) { FILE *f = fopen(PRN_FILES_ON, "wb"); if (f) fclose(f); }
    else unlink(PRN_FILES_ON);
    pthread_mutex_unlock(&files_lock);
    loader_status("file service %s (%s)", on ? "ON" : "OFF", who);
}

enum want { W_DIR, W_FILE, W_NEW };

static int rel_ok(const char *p, int allow_empty)
{
    if (!*p) return allow_empty;
    if (*p == '/') return 0;
    for (const char *c = p; *c; c++) if (*c < 0x21 || *c > 0x7e || *c == '\\' || *c == ':') return 0;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n + (e ? 1 : 0);
        if (e && !*s) return 0;
    }
    return 1;
}

/* Builds root/rel in full and walks it with stat.  *exists reports the final component for W_NEW.  0 / -1. */
static int resolve(uint8_t root, const char *rel, char *full, size_t cap, enum want w, int *exists)
{
    struct stat st;
    char *p;
    if (root != PRN_ROOT_TMP || !rel_ok(rel, w == W_DIR)) return -1;
    if ((size_t)snprintf(full, cap, "%s%s%s", PRN_TMP_ROOT, *rel ? "/" : "", rel) >= cap) return -1;
    p = full + strlen(PRN_TMP_ROOT);
    for (;;) {                                   /* every proper prefix must be a directory */
        char *slash = strchr(p + 1, '/');
        char c;
        if (!slash) break;
        c = *slash; *slash = 0;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) { *slash = c; return -1; }
        *slash = c; p = slash;
    }
    if (stat(PRN_TMP_ROOT, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    switch (w) {
    case W_DIR:  return stat(full, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    case W_FILE: return stat(full, &st) == 0 && S_ISREG(st.st_mode) ? 0 : -1;
    case W_NEW:
        if (stat(full, &st) != 0) { *exists = 0; return errno == ENOENT ? 0 : -1; }
        *exists = 1;
        return S_ISREG(st.st_mode) ? 0 : -1;
    }
    return -1;
}

static int reply_err(prn_chan *c, uint32_t req, const char *why)
{
    uint8_t b[160];
    size_t n = strlen(why);
    if (n > sizeof b - 4) n = sizeof b - 4;
    prn_put32(b, req);
    memcpy(b + 4, why, n);
    return prn_chan_send(c, PRN_FILE_ERR, b, (uint32_t)(4 + n));
}

static int reply_ok(prn_chan *c, uint32_t req)
{
    uint8_t b[4];
    prn_put32(b, req);
    return prn_chan_send(c, PRN_FILE_OK, b, 4);
}

/* u32 req, u8 root, u16 len, path -> path copied to out (NUL-terminated); returns bytes consumed or -1 */
static int parse_path(const uint8_t *p, uint32_t n, uint32_t *req, uint8_t *root, char *out)
{
    uint16_t len;
    if (n < 7) return -1;
    *req = prn_get32(p); *root = p[4]; len = prn_get16(p + 5);
    if (len > PRN_MAX_PATH || 7u + len > n || memchr(p + 7, 0, len)) return -1;
    memcpy(out, p + 7, len); out[len] = 0;
    return 7 + len;
}

static uint8_t big[PRN_MAX_FRAME];

static int do_list(prn_chan *c, uint8_t *p, uint32_t n)
{
    uint32_t req; uint8_t root; char rel[PRN_MAX_PATH + 1], full[512], ent[1024];
    DIR *d; struct dirent *e; uint32_t cnt = 0, at = 10; int truncated = 0, unstable = 0;
    if (parse_path(p, n, &req, &root, rel) != (int)n) return reply_err(c, 0, "malformed LIST");
    if (!files_on) return reply_err(c, req, "file service is OFF");
    if (resolve(root, rel, full, sizeof full, W_DIR, NULL) != 0) return reply_err(c, req, "path not allowed or not a directory");
    if ((d = opendir(full)) == NULL) return reply_err(c, req, "cannot open directory");
    prn_put32(big, req);
    while ((e = readdir(d)) != NULL) {
        struct stat st; size_t nl = strlen(e->d_name);
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (cnt == PRN_MAX_LIST || at + 11 + nl > sizeof big) { truncated = 1; break; }
        snprintf(ent, sizeof ent, "%s/%s", full, e->d_name);
        if (stat(ent, &st) != 0) { unstable = 1; continue; }
        big[at] = S_ISDIR(st.st_mode) ? 'd' : S_ISREG(st.st_mode) ? 'f' : '?';
        prn_put64(big + at + 1, (uint64_t)st.st_size);
        prn_put16(big + at + 9, (uint16_t)nl);
        memcpy(big + at + 11, e->d_name, nl);
        at += 11 + (uint32_t)nl; cnt++;
    }
    closedir(d);
    big[4] = (uint8_t)truncated; big[5] = (uint8_t)unstable; prn_put32(big + 6, cnt);
    return prn_chan_send(c, PRN_LIST_RESULT, big, at);
}

static int do_get(prn_chan *c, uint8_t *p, uint32_t n)
{
    uint32_t req; uint8_t root; char rel[PRN_MAX_PATH + 1], full[512];
    struct stat st0, st1; FILE *f; uint64_t total = 0; prn_sha256 s; uint8_t end[4 + 8 + 32];
    unsigned gen = files_gen;
    if (parse_path(p, n, &req, &root, rel) != (int)n) return reply_err(c, 0, "malformed GET");
    if (!files_on) return reply_err(c, req, "file service is OFF");
    if (resolve(root, rel, full, sizeof full, W_FILE, NULL) != 0) return reply_err(c, req, "path not allowed or not a regular file");
    if (stat(full, &st0) != 0 || (uint64_t)st0.st_size > MAX_XFER) return reply_err(c, req, "file too large or gone");
    if ((f = fopen(full, "rb")) == NULL) return reply_err(c, req, "cannot open file");
    prn_sha256_init(&s);
    prn_put32(big, req);
    for (;;) {
        size_t k = fread(big + 4, 1, CHUNK, f);
        if (k == 0) break;
        if (!files_on || files_gen != gen) { fclose(f); return reply_err(c, req, "file service turned off"); }
        prn_sha256_update(&s, big + 4, k);
        total += k;
        if (total > (uint64_t)st0.st_size || prn_chan_send(c, PRN_GET_DATA, big, (uint32_t)(4 + k)) != 0) { fclose(f); return total > (uint64_t)st0.st_size ? reply_err(c, req, "file grew while reading") : -1; }
    }
    if (ferror(f)) { fclose(f); return reply_err(c, req, "read error"); }
    fclose(f);
    if (total != (uint64_t)st0.st_size || stat(full, &st1) != 0 || st1.st_size != st0.st_size || st1.st_mtim.tv_sec != st0.st_mtim.tv_sec || st1.st_mtim.tv_nsec != st0.st_mtim.tv_nsec)
        return reply_err(c, req, "short read or file changed while reading");
    prn_put32(end, req); prn_put64(end + 4, total); prn_sha256_final(&s, end + 12);
    return prn_chan_send(c, PRN_GET_END, end, sizeof end);
}

static int do_put(prn_chan *c, uint8_t *p, uint32_t n)
{
    uint32_t req; uint8_t root, policy, want_sha[32], got_sha[32]; char rel[PRN_MAX_PATH + 1], full[512], stage[160];
    uint64_t size, got = 0; int used, exists, rc = 0; FILE *f; prn_sha256 s; unsigned gen = files_gen;
    uint8_t type; uint32_t m;
    used = parse_path(p, n, &req, &root, rel);
    if (used < 0 || (uint32_t)used + 8 + 32 + 1 != n) return reply_err(c, 0, "malformed PUT");
    size = prn_get64(p + used); memcpy(want_sha, p + used + 8, 32); policy = p[used + 40];
    if (!files_on) return reply_err(c, req, "file service is OFF");
    if (size > MAX_XFER || policy > 1) return reply_err(c, req, "size or policy not allowed");
    if (resolve(root, rel, full, sizeof full, W_NEW, &exists) != 0) return reply_err(c, req, "path not allowed");
    if (exists && policy == 0) return reply_err(c, req, "destination exists (policy NEW)");
    {   uint8_t r[8]; loader_random(r, sizeof r);
        snprintf(stage, sizeof stage, "%s/put-%u-%02x%02x%02x%02x%02x%02x%02x%02x.partial", PRN_STATE_DIR, (unsigned)req,
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]); }
    if ((f = fopen(stage, "wb")) == NULL) return reply_err(c, req, "cannot create staging file");
    prn_sha256_init(&s);
    for (;;) {
        if (prn_chan_recv(c, &type, big, sizeof big, &m) != 0) { fclose(f); unlink(stage); return -1; }
        if (type == PRN_FILES_OFF) { loader_set_files(0, "host, during a PUT"); if (reply_ok(c, m >= 4 ? prn_get32(big) : 0) != 0) { fclose(f); unlink(stage); return -1; } continue; }
        if (m < 4 || prn_get32(big) != req) { fclose(f); unlink(stage); return reply_err(c, req, "unexpected frame during PUT"); }
        if (type == PRN_PUT_COMMIT) break;
        if (type != PRN_PUT_DATA) { fclose(f); unlink(stage); return reply_err(c, req, "unexpected frame during PUT"); }
        if (got + (m - 4) > size) { fclose(f); unlink(stage); return reply_err(c, req, "more data than declared"); }
        if (fwrite(big + 4, 1, m - 4, f) != m - 4) { fclose(f); unlink(stage); return reply_err(c, req, "write error"); }
        prn_sha256_update(&s, big + 4, m - 4);
        got += m - 4;
    }
    if (fclose(f) != 0) { unlink(stage); return reply_err(c, req, "write error"); }
    prn_sha256_final(&s, got_sha);
    if (got != size || memcmp(got_sha, want_sha, 32) != 0) { unlink(stage); return reply_err(c, req, "size or sha256 mismatch"); }
    pthread_mutex_lock(&files_lock);
    if (!files_on || files_gen != gen) rc = 1;
    else if (resolve(root, rel, full, sizeof full, W_NEW, &exists) != 0 || (exists && policy == 0)) rc = 2;
    else {
        /* REPLACE never loses the old file: move it aside, put the staged file in place, then delete the moved copy;
           if the second rename fails the old file is moved back (unlink-then-rename could lose it) */
        char aside[176];
        snprintf(aside, sizeof aside, "%s.old", stage);
        if (exists && rename(full, aside) != 0) rc = 3;
        else if (rename(stage, full) != 0) rc = (exists && rename(aside, full) != 0) ? 4 : 3;
        else if (exists) unlink(aside);
    }
    pthread_mutex_unlock(&files_lock);
    if (rc) {
        unlink(stage);
        return reply_err(c, req, rc == 1 ? "file service turned off: not published" : rc == 2 ? "destination changed: not published"
                                 : rc == 4 ? "publish failed; the previous file could not be put back (kept in the loader's state dir)"
                                           : "publish failed; destination unchanged");
    }
    return reply_ok(c, req);
}

static int do_delete(prn_chan *c, uint8_t *p, uint32_t n)
{
    uint32_t req; uint8_t root; char rel[PRN_MAX_PATH + 1], full[512]; int rc = 0;
    if (parse_path(p, n, &req, &root, rel) != (int)n) return reply_err(c, 0, "malformed DELETE");
    pthread_mutex_lock(&files_lock);
    if (!files_on) rc = 1;
    else if (resolve(root, rel, full, sizeof full, W_FILE, NULL) != 0) rc = 2;
    else if (unlink(full) != 0) rc = 3;
    pthread_mutex_unlock(&files_lock);
    if (rc) return reply_err(c, req, rc == 1 ? "file service is OFF" : rc == 2 ? "path not allowed or not a regular file" : "delete failed");
    return reply_ok(c, req);
}

int loader_file_request(void *chan, uint8_t type, uint8_t *payload, uint32_t n)
{
    prn_chan *c = chan;
    switch (type) {
    case PRN_LIST:   return do_list(c, payload, n);
    case PRN_GET:    return do_get(c, payload, n);
    case PRN_PUT:    return do_put(c, payload, n);
    case PRN_DELETE: return do_delete(c, payload, n);
    case PRN_FILES_OFF:
        loader_set_files(0, "host");
        return reply_ok(c, n >= 4 ? prn_get32(payload) : 0);
    }
    return -1;
}
