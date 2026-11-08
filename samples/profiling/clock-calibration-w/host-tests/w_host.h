/* HOST-ONLY fakes for the W probe's control flow (W_HOST_TEST): every LV2 / GCM / sysutil call the probe makes, a
 * fake SPU (a synchronous "DMA" into payload/commit) and a fake RSX (the label written at flush), and a file-call
 * seam for the CSV (open, fdopen, fprintf, fflush, fsync, fclose) and the markers (open, write, fsync, close).
 * MODEL ONLY: nothing here describes LV2, the SPU, the RSX or the console's filesystem.  In particular the
 * W_MB_NO_ENQUEUE refusal code is a host-model guarantee, not an LV2 contract.
 *
 * Faults: env W_FAULTS, comma-separated key=value:
 *   setup.<sysutil-register|init|import|group-create|thread-init|group-start>=<hex>  that call returns <hex>
 *   gcm.<host-buffer|init>=<hex>   label=null                   GCM setup / the label address
 *   mb@<k>=<err:<hex>|timeout|identity>    the k-th mailbox write (1-based, every request counted)
 *   rsx@<k>=<timeout|identity>             the k-th RSX pair
 *   quit@<k>                               the k-th cellSysutilCheckCallback reports an exit request
 *   cleanup.<terminate|join|destroy|image-close>=<hex>
 *   csv.<open|fdopen|rawclose>=<hex errno>   csv.fprintf@<k>=fail   csv.<fflush|fsync>@<k>=<hex errno>   csv.fclose=<hex>
 *   mk<NN>.<open|fsync|close>=<hex errno>   mk<NN>.write=<zero|<hex errno>|part:<n>:<hex errno>>
 *   tbstep=<n>                             the fake timebase step (default 1000; 0 = equal readings)
 * Every call is appended to env W_TRACE as one line. */
#ifndef CLOCKCAL_W_HOST_H
#define CLOCKCAL_W_HOST_H

#define _POSIX_C_SOURCE 200809L /* fdopen, fileno (main.c includes this header first) */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef W_DIR
#error "host tests define W_DIR"
#endif
#define W_HOST_NO_ENQUEUE_RC 0x80010099u /* the model's guaranteed no-enqueue refusal */
#define W_MB_NO_ENQUEUE(rc) ((uint32_t)(rc) == W_HOST_NO_ENQUEUE_RC)
#define W_SYNC() __sync_synchronize()
#define W_SYNC_ISYNC() __sync_synchronize()
#define CELL_SYSUTIL_REQUEST_EXITGAME 0x0101u
#define CELL_GCM_CURRENT (&g_fake_ctx)
#define usleep(us) ((void)(us))

typedef uint32_t sys_addr_t;
typedef struct {
    int id;
} sysSpuImage;
typedef struct {
    const char *name;
    uint32_t nsize;
} sysSpuThreadGroupAttribute;
typedef struct {
    const char *name;
    uint32_t nsize;
} sysSpuThreadAttribute;
typedef struct {
    uint64_t arg0, arg1, arg2, arg3;
} sysSpuThreadArgument;
typedef struct {
    int unused;
} CellGcmContextData;
typedef void (*w_cb)(uint64_t, uint64_t, void *);
static const uint8_t clockcal_spu_bin[16];
static CellGcmContextData g_fake_ctx;

/* ---- the fault script and the trace ---- */
static const char *w_f(const char *key) /* a copy of key's value in W_FAULTS ("" for a bare key), or NULL */
{
    static char buf[2048], val[4][128];
    static unsigned r;
    const char *s = getenv("W_FAULTS");
    size_t kl = strlen(key);
    if (!s)
        return NULL;
    if (strlen(s) >= sizeof buf)
        abort();
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ","))
        if (!strncmp(t, key, kl) && (t[kl] == '=' || t[kl] == '\0')) {
            char *v = val[r++ & 3];
            snprintf(v, sizeof val[0], "%s", t[kl] == '=' ? t + kl + 1 : "");
            return v;
        }
    return NULL;
}
static int w_fval(const char *key, uint32_t *v) /* 1 when key=<hex> is set */
{
    const char *x = w_f(key);
    if (!x)
        return 0;
    *v = (uint32_t)strtoul(x, NULL, 16);
    return 1;
}
static void w_trace(const char *fmt, ...)
{
    const char *t = getenv("W_TRACE");
    if (!t)
        return;
    FILE *f = fopen(t, "a");
    if (!f)
        abort();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- the fake timebase ---- */
static uint64_t w_tb_now = 0x100000;
static uint64_t w_fake_tb(void)
{
    static long step = -1;
    if (step < 0) {
        const char *s = w_f("tbstep");
        step = s ? strtol(s, NULL, 10) : 1000;
    }
    return w_tb_now += (uint64_t)step;
}
#define W_TB() w_fake_tb()
static uint64_t sysGetTimebaseFrequency(void) { return 79800000u; }
static uint64_t sys_time_get_system_time(void) { return 1; }

/* ---- sysutil ---- */
static unsigned w_checks;
static int cellSysutilRegisterCallback(int slot, w_cb cb, void *arg)
{
    uint32_t v;
    (void)slot, (void)cb, (void)arg;
    if (w_fval("setup.sysutil-register", &v))
        return (int)v;
    return 0;
}
static int cellSysutilCheckCallback(void)
{
    char k[32];
    snprintf(k, sizeof k, "quit@%u", ++w_checks);
    return w_f(k) ? 1 : 0; /* c4 treats a nonzero return as an exit request */
}

/* ---- the fake SPU ---- */
static volatile uint32_t *w_payload, *w_commit;
static unsigned w_mbk;
static uint32_t w_dec = 0x7fffffffu;
static int w_setup(const char *k)
{
    uint32_t v;
    char key[48];
    snprintf(key, sizeof key, "setup.%s", k);
    return w_fval(key, &v) ? (int)v : 0;
}
static int sysSpuInitialize(int a, int b) { (void)a, (void)b; return w_setup("init"); }
static int sysSpuImageImport(sysSpuImage *i, const void *bin, int t) { (void)i, (void)bin, (void)t; return w_setup("import"); }
static int sysSpuThreadGroupCreate(uint32_t *g, int n, int p, sysSpuThreadGroupAttribute *a)
{
    (void)n, (void)p, (void)a;
    *g = 7;
    return w_setup("group-create");
}
static int sysSpuThreadInitialize(uint32_t *t, uint32_t g, int i, sysSpuImage *im, sysSpuThreadAttribute *a,
                                  sysSpuThreadArgument *args)
{
    (void)g, (void)i, (void)im, (void)a;
    *t = 9;
    w_payload = (volatile uint32_t *)(uintptr_t)args->arg0;
    w_commit = (volatile uint32_t *)(uintptr_t)args->arg1;
    return w_setup("thread-init");
}
static int sysSpuThreadGroupStart(uint32_t g) { (void)g; return w_setup("group-start"); }
static int sysSpuThreadWriteMb(uint32_t thread, uint32_t seq)
{
    char key[32];
    (void)thread;
    snprintf(key, sizeof key, "mb@%u", ++w_mbk);
    const char *f = w_f(key);
    if (f && !strncmp(f, "err:", 4)) {
        uint32_t rc = (uint32_t)strtoul(f + 4, NULL, 16);
        w_trace("spu-mb %u seq %08x -> %08x", w_mbk, seq, rc);
        return (int)rc;
    }
    w_trace("spu-mb %u seq %08x -> 0%s", w_mbk, seq, f ? (strcmp(f, "timeout") ? " (identity)" : " (no reply)") : "");
    if (f && !strcmp(f, "timeout"))
        return 0; /* submitted; the fake SPU never answers */
    w_dec -= 1000;
    w_payload[0] = f ? seq ^ 1u : seq; /* identity: the payload names another request */
    w_payload[1] = w_dec;
    w_payload[2] = w_dec - 7;
    w_payload[3] = seq & 0x80000000u ? 0xFFFFFFFFu : 0;
    w_commit[0] = seq;
    return 0;
}
static int w_cleanup(const char *k)
{
    uint32_t v;
    char key[48];
    snprintf(key, sizeof key, "cleanup.%s", k);
    int rc = w_fval(key, &v) ? (int)v : 0;
    w_trace("cleanup %s -> %08x", k, (unsigned)rc);
    return rc;
}
static int sysSpuThreadGroupTerminate(uint32_t g, int v) { (void)g, (void)v; return w_cleanup("terminate"); }
static int sysSpuThreadGroupJoin(uint32_t g, uint32_t *c, uint32_t *s) { (void)g, (void)c, (void)s; return w_cleanup("join"); }
static int sysSpuThreadGroupDestroy(uint32_t g) { (void)g; return w_cleanup("destroy"); }
static int sysSpuImageClose(sysSpuImage *i) { (void)i; return w_cleanup("image-close"); }

/* ---- the fake RSX ---- */
static volatile uint32_t w_label;
static uint32_t w_want;
static unsigned w_rsxk;
static int w_rsx_identity;
static uint64_t w_report = 5000;
static int gcm_host_buffer_alloc(uint32_t size, sys_addr_t *h)
{
    uint32_t v;
    (void)size;
    *h = 0x10000000u;
    return w_fval("gcm.host-buffer", &v) ? (int)v : 0;
}
static int cellGcmInit(uint32_t a, uint32_t b, void *h)
{
    uint32_t v;
    (void)a, (void)b, (void)h;
    return w_fval("gcm.init", &v) ? (int)v : 0;
}
static volatile uint32_t *cellGcmGetLabelAddress(uint8_t i)
{
    (void)i;
    return w_f("label") ? NULL : &w_label;
}
static void cellGcmSetTimeStamp(CellGcmContextData *c, uint32_t r) { (void)c, (void)r; }
static void cellGcmSetWriteBackEndLabel(CellGcmContextData *c, uint8_t l, uint32_t v) { (void)c, (void)l, w_want = v; }
static void cellGcmFlush(CellGcmContextData *c)
{
    char key[32];
    (void)c;
    snprintf(key, sizeof key, "rsx@%u", ++w_rsxk);
    const char *f = w_f(key);
    w_trace("rsx-flush %u want %08x%s", w_rsxk, w_want, f ? (strcmp(f, "timeout") ? " (identity)" : " (no write)") : "");
    w_rsx_identity = f && strcmp(f, "timeout") != 0;
    if (!f || w_rsx_identity)
        w_label = w_want;
}
static uint64_t cellGcmGetTimeStamp(uint32_t r)
{
    (void)r;
    if (w_rsx_identity)
        w_label = w_want + 1000u; /* the label changes between the two reads */
    return w_report += 77;
}

/* ---- the file seam: CSV and markers ---- */
static int w_csv_fd = -1, w_mk_fd = -1;
static unsigned w_mk_seq, w_nfprintf, w_nfflush, w_nfsync;
static int w_mk_part_done;
static int w_open(const char *p, int fl, int mode)
{
    uint32_t e;
    size_t n = strlen(p);
    int is_mk = n > 3 && !strcmp(p + n - 3, ".mk");
    if (is_mk) {
        char key[32];
        w_mk_seq = (unsigned)atoi(p + n - 5);
        snprintf(key, sizeof key, "mk%02u.open", w_mk_seq);
        if (w_fval(key, &e)) {
            w_trace("mk-open %02u -> -1 %08x", w_mk_seq, e);
            errno = (int)e;
            return -1;
        }
    } else if (w_fval("csv.open", &e)) {
        w_trace("csv-open -> -1 %08x", e);
        errno = (int)e;
        return -1;
    }
    int fd = open(p, fl, mode);
    if (is_mk) {
        w_mk_fd = fd;
        w_mk_part_done = 0;
        w_trace("mk-open %02u -> %s", w_mk_seq, fd >= 0 ? "fd" : "-1");
    } else {
        w_csv_fd = fd;
        w_trace("csv-open -> %s", fd >= 0 ? "fd" : "-1");
    }
    return fd;
}
static FILE *w_fdopen(int fd, const char *m)
{
    uint32_t e;
    if (w_fval("csv.fdopen", &e)) {
        w_trace("csv-fdopen -> NULL %08x", e);
        errno = (int)e;
        return NULL;
    }
    w_trace("csv-fdopen -> FILE");
    return fdopen(fd, m);
}
static ssize_t w_write(int fd, const void *b, size_t n)
{
    char key[32];
    snprintf(key, sizeof key, "mk%02u.write", w_mk_seq);
    const char *f = fd == w_mk_fd ? w_f(key) : NULL;
    if (f && !strcmp(f, "zero")) {
        w_trace("mk-write %02u %zu -> 0", w_mk_seq, n);
        return 0;
    }
    if (f && !strncmp(f, "part:", 5) && !w_mk_part_done) { /* part:<n>:<hex>: n bytes, then the error */
        size_t k = strtoul(f + 5, NULL, 10);
        w_mk_part_done = 1;
        ssize_t r = write(fd, b, k < n ? k : n);
        w_trace("mk-write %02u %zu -> %zd", w_mk_seq, n, r);
        return r;
    }
    if (f) {
        const char *h = f;
        if (!strncmp(f, "part:", 5)) {
            h = strchr(f + 5, ':');
            if (!h)
                abort();
            h++;
        }
        uint32_t e = (uint32_t)strtoul(h, NULL, 16);
        w_trace("mk-write %02u %zu -> -1 %08x", w_mk_seq, n, e);
        errno = (int)e;
        return -1;
    }
    ssize_t r = write(fd, b, n);
    w_trace("mk-write %02u %zu -> %zd", w_mk_seq, n, r);
    return r;
}
static int w_fsync(int fd)
{
    uint32_t e;
    char key[32];
    if (fd == w_mk_fd) {
        snprintf(key, sizeof key, "mk%02u.fsync", w_mk_seq);
        if (w_fval(key, &e)) {
            w_trace("mk-fsync %02u -> -1 %08x", w_mk_seq, e);
            errno = (int)e;
            return -1;
        }
        w_trace("mk-fsync %02u -> 0", w_mk_seq);
        return fsync(fd);
    }
    snprintf(key, sizeof key, "csv.fsync@%u", ++w_nfsync);
    if (w_fval(key, &e)) {
        w_trace("csv-fsync %u -> -1 %08x", w_nfsync, e);
        errno = (int)e;
        return -1;
    }
    w_trace("csv-fsync %u -> 0", w_nfsync);
    return fsync(fd);
}
static int w_close(int fd)
{
    uint32_t e;
    char key[32];
    int r = close(fd); /* the descriptor is always released */
    if (fd == w_mk_fd) {
        w_mk_fd = -1;
        snprintf(key, sizeof key, "mk%02u.close", w_mk_seq);
        if (w_fval(key, &e)) {
            w_trace("mk-close %02u -> -1 %08x", w_mk_seq, e);
            errno = (int)e;
            return -1;
        }
        w_trace("mk-close %02u -> %d", w_mk_seq, r);
        return r;
    }
    if (w_fval("csv.rawclose", &e)) {
        w_trace("csv-rawclose -> -1 %08x", e);
        errno = (int)e;
        return -1;
    }
    w_trace("csv-rawclose -> %d", r);
    return r;
}
static int w_fprintf(FILE *f, const char *fmt, ...)
{
    char key[32];
    snprintf(key, sizeof key, "csv.fprintf@%u", ++w_nfprintf);
    if (w_f(key)) {
        w_trace("csv-fprintf %u -> -1", w_nfprintf);
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
static int w_fflush(FILE *f)
{
    uint32_t e;
    char key[32];
    snprintf(key, sizeof key, "csv.fflush@%u", ++w_nfflush);
    if (w_fval(key, &e)) {
        w_trace("csv-fflush %u -> EOF %08x", w_nfflush, e);
        errno = (int)e;
        return EOF;
    }
    w_trace("csv-fflush %u -> 0", w_nfflush);
    return fflush(f);
}
static int w_fclose(FILE *f)
{
    uint32_t e;
    int r = fclose(f);
    if (w_fval("csv.fclose", &e)) {
        w_trace("csv-fclose -> EOF %08x", e);
        errno = (int)e;
        return EOF;
    }
    w_trace("csv-fclose -> %d", r);
    return r;
}
#define open w_open
#define fdopen w_fdopen
#define write w_write
#define fsync w_fsync
#define close w_close
#define fprintf w_fprintf
#define fflush w_fflush
#define fclose w_fclose

#endif
