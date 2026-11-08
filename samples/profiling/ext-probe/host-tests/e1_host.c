/* The E1 host harness (see e1_host.h for the model and the fault keys).  HOST ONLY, model evidence only.
 * Linked with -Wl,--wrap for every profiler API call main.c makes and for the writer calls e1_spu_core.c makes, so a
 * fault can be injected at each; a wrapped call without a fault calls the real one. */
#define _GNU_SOURCE
#define PS3DK_PROF_SPU_HOST_TEST 1
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "gpu_port_gcm.h"
#include "prof_gpu.h"
#include "prof_spu.h"
#include "prof_spu_lane.h"
#include "prof_test_hooks.h"
#include "psp_host_emul.h"
#include "psp_layout.h"
#include "psp_spu.h"

#include "../spu/e1_spu_core.h"
#include "e1_host_api.h"

/* ---- faults and trace ---- */
static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;
const char *e1h_f(const char *key)
{
    static __thread char buf[4096], val[4][160];
    static __thread unsigned r;
    const char *s = getenv("E1_FAULTS");
    size_t kl = strlen(key);
    if (!s)
        return NULL;
    if (strlen(s) >= sizeof buf)
        abort();
    snprintf(buf, sizeof buf, "%s", s);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        if (!strncmp(t, key, kl) && (t[kl] == '=' || t[kl] == '\0')) {
            char *v = val[r++ & 3];
            snprintf(v, sizeof val[0], "%s", t[kl] == '=' ? t + kl + 1 : "");
            return v;
        }
    return NULL;
}
int e1h_fval(const char *key, uint32_t *v)
{
    const char *x = e1h_f(key);
    if (!x)
        return 0;
    *v = (uint32_t)strtoul(x, NULL, 16);
    return 1;
}
void e1h_trace(const char *fmt, ...)
{
    const char *t = getenv("E1_TRACE");
    if (!t)
        return;
    pthread_mutex_lock(&g_tm);
    FILE *f = fopen(t, "a");
    if (!f)
        abort();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    pthread_mutex_unlock(&g_tm);
}
const char *e1h_base(void)
{
    const char *b = getenv("E1_BASE_DIR");
    if (!b)
        abort();
    return b;
}
/* the k-th call of one API (1-based) and its fault value, if any: "<key>@<k>" first, then "<key>" (every call) */
static const char *nth(const char *key, unsigned k)
{
    char kk[64];
    snprintf(kk, sizeof kk, "%s@%u", key, k);
    const char *f = e1h_f(kk);
    return f ? f : e1h_f(key);
}

/* ---- the host SPU ---- */
static uint64_t g_ring, g_ctl;
static uint32_t g_slots;
static e1_line *g_payload, *g_commit;
static atomic_int g_terminated, g_mb_full, g_copy_step;
static atomic_uint g_mb_val;
static pthread_t g_th;
static int g_started, g_joined;

static ps3dk_prof_spu_writer g_w __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN)));
static uint8_t g_staging[PS3DK_PROF_SPU_STAGING_BYTES(E1_LS_CAP)] __attribute__((aligned(16)));

typedef struct hport {
    uint32_t dec, tag_mask, last_stat;
    uint64_t res_ver;
    int res_held;
} hport;
static hport g_hp = {0xFFFFF000u, 0x11u, 0, 0, 0};

static void producer_exit(const char *why)
{
    e1h_trace("spu-exit %s", why);
    pthread_exit(NULL);
}
static void wait_terminated(const char *why)
{
    while (!atomic_load(&g_terminated))
        usleep(50);
    producer_exit(why);
}

static uint8_t *at(uint64_t ea) { return (uint8_t *)(uintptr_t)ea; }
static uint32_t p_rd_dec(void *c) { hport *p = c; return p->dec -= 7u; }
static void p_wr_dec(void *c, uint32_t v)
{
    ((hport *)c)->dec = v;
    e1h_trace("spu-dec-write %08x", (unsigned)v);
}
static int p_blocking(void *c) { (void)c; return 0; }
static uint32_t p_rd_mask(void *c) { return ((hport *)c)->tag_mask; }
static void p_wr_mask(void *c, uint32_t m) { ((hport *)c)->tag_mask = m; }
static uint32_t p_tag_wait(void *c) { return ((hport *)c)->tag_mask; }
static void p_getllar(void *c, uint64_t ea, void *ls)
{
    hport *p = c;
    psp_emul_line_lock(ea);
    memcpy(ls, at(ea), 128);
    p->res_ver = psp_emul_version(ea);
    p->res_held = 1;
    psp_emul_line_unlock(ea);
    p->last_stat = PSP_AS_GETLLAR;
}
static void p_putllc(void *c, uint64_t ea, const void *ls)
{
    hport *p = c;
    psp_emul_line_lock(ea);
    if (p->res_held && psp_emul_version(ea) == p->res_ver) {
        memcpy(at(ea), ls, 128);
        psp_emul_version_bump(ea);
        p->last_stat = PSP_AS_PUTLLC_OK;
    } else {
        p->last_stat = PSP_AS_PUTLLC_LOST;
    }
    p->res_held = 0;
    psp_emul_line_unlock(ea);
}
static void p_putlluc(void *c, uint64_t ea, const void *ls)
{
    hport *p = c;
    psp_emul_line_lock(ea);
    memcpy(at(ea), ls, 128);
    psp_emul_line_unlock(ea);
    p->last_stat = PSP_AS_PUTLLUC;
}
static uint32_t p_atomic(void *c)
{
    hport *p = c;
    uint32_t s = p->last_stat;
    p->last_stat = 0;
    return s;
}
static void p_put(void *c, const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
    (void)c, (void)tag;
    memcpy(at(ea), ls, size);
}
static const psp_port g_port = {&g_hp, p_rd_dec, p_wr_dec, p_blocking, p_rd_mask, p_wr_mask, p_tag_wait, p_getllar,
                                p_putllc, p_putlluc, p_atomic, p_put};
const psp_port *ps3dk_prof__spu_host_port = &g_port;

/* the e1 port: the inbound mailbox and the two report lines */
static uint32_t h_read_in_mbox(void *ctx)
{
    (void)ctx;
    for (;;) {
        if (atomic_load(&g_terminated))
            producer_exit("terminated-in-mbox");
        if (atomic_load(&g_mb_full)) {
            uint32_t v = atomic_load(&g_mb_val);
            e1h_trace("spu-mbox-read %08x", (unsigned)v);
            return v;
        }
        usleep(50);
    }
}
static void store_payload(const e1_line *p, uint32_t magic_xor)
{
    for (unsigned i = 0; i < 32u; i++)
        __atomic_store_n(&g_payload->w[i], p->w[i] ^ (i == E1_W_MAGIC ? magic_xor : 0u), __ATOMIC_RELAXED);
}
static void h_report(void *ctx, const e1_line *payload, uint32_t seq)
{
    char k[16];
    (void)ctx;
    snprintf(k, sizeof k, "rep%u", (unsigned)seq);
    const char *f = e1h_f(k);
    e1h_trace("spu-report %u kind %u call %u rc %d ev1 %u ev2 %u dec %u ridx %u%s%s", (unsigned)seq,
              (unsigned)payload->w[E1_W_KIND], (unsigned)payload->w[E1_W_FAIL_CALL],
              (int)(int32_t)payload->w[E1_W_FAIL_RC], (unsigned)payload->w[E1_W_EV1], (unsigned)payload->w[E1_W_EV2],
              (unsigned)payload->w[E1_W_DEC_STATE], (unsigned)payload->w[E1_W_RIDX_STATE], f ? " fault " : "",
              f ? f : "");
    if (f && !strcmp(f, "stale"))
        wait_terminated("report-never-sent");
    store_payload(payload, f && !strcmp(f, "identity") ? 0x1u : 0u);
    if (f && !strcmp(f, "nocommit"))
        wait_terminated("report-without-commit");
#ifdef E1H_PLAIN_REPORT
    *(volatile uint32_t *)&g_commit->w[0] = seq; /* the TSan NEGATIVE CONTROL: a plain store racing the PPU loads */
#else
    __atomic_store_n(&g_commit->w[0], seq, __ATOMIC_RELEASE);
#endif
    if (f && !strcmp(f, "torn")) {
        while (atomic_load(&g_copy_step) != 1)
            sched_yield();
        __atomic_store_n(&g_commit->w[0], seq + 0x100u, __ATOMIC_RELEASE);
        atomic_store(&g_copy_step, 2);
    }
}
void e1h_after_copy(uint32_t s)
{
    char k[16];
    snprintf(k, sizeof k, "rep%u", (unsigned)s);
    const char *f = e1h_f(k);
    if (f && !strcmp(f, "torn")) {
        atomic_store(&g_copy_step, 1);
        while (atomic_load(&g_copy_step) != 2)
            sched_yield();
    }
}
static const e1_spu_port g_e1port = {NULL, h_read_in_mbox, h_report};

static void *producer(void *a)
{
    (void)a;
    e1_spu_run(&g_e1port, &g_w, g_staging, g_ring, g_slots, g_ctl);
    producer_exit("returned");
    return NULL;
}
void e1h_spu_args(uint64_t ea_ring, uint32_t ring_slots, uint64_t ea_ctl, uint64_t report_eas)
{
    g_ring = ea_ring;
    g_slots = ring_slots;
    g_ctl = ea_ctl;
    e1h_trace("spu-args slots %u report_eas %llx", (unsigned)ring_slots, (unsigned long long)report_eas);
}
void e1h_spu_start(e1_line *payload, e1_line *commit)
{
    g_payload = payload;
    g_commit = commit;
    if (pthread_create(&g_th, NULL, producer, NULL) != 0)
        abort();
    g_started = 1;
}
int e1h_spu_write_mb(uint32_t v)
{
    const char *f = e1h_f("mb");
    if (f && !strncmp(f, "err:", 4)) {
        uint32_t rc = (uint32_t)strtoul(f + 4, NULL, 16);
        e1h_trace("spu-mb-write %08x -> %08x (nothing delivered: model)", (unsigned)v, (unsigned)rc);
        return (int)rc;
    }
    if (f && !strncmp(f, "value:", 6))
        v = (uint32_t)strtoul(f + 6, NULL, 16);
    e1h_trace("spu-mb-write delivered %08x -> 0", (unsigned)v);
    atomic_store(&g_mb_val, v);
    atomic_store(&g_mb_full, 1);
    return 0;
}
void e1h_spu_terminate(void)
{
    e1h_trace("spu-terminate");
    atomic_store(&g_terminated, 1);
}
void e1h_spu_join(void)
{
    if (g_started && !g_joined) {
        pthread_join(g_th, NULL);
        g_joined = 1;
    }
    e1h_trace("spu-joined");
}

/* the image: the host-built records of e1_spu_core.c.  On the host a record is 24 bytes in a 32-byte slot, so the
 * writer's 16-byte index arithmetic names slots 0, 2, 4, ...: the table gets one entry per 16-byte slot, the real
 * record where one starts and a filler elsewhere (a host-model artefact; the SPU image has 16-byte records). */
extern const char __start_ps3dkprof_spudesc[] __attribute__((weak));
extern const char __stop_ps3dkprof_spudesc[] __attribute__((weak));
int e1h_register_image(uint32_t *out)
{
    static ps3dk_prof_spu_desc tab[64];
    uintptr_t a = (uintptr_t)__start_ps3dkprof_spudesc, b = (uintptr_t)__stop_ps3dkprof_spudesc;
    uint32_t n = (uint32_t)((b - a) >> 4);
    if (!a || n == 0 || n > 64)
        abort();
    for (uint32_t i = 0; i < n; i++) {
        const ps3dk_prof_spu_desc *r = (const ps3dk_prof_spu_desc *)(a + 16u * i);
        if (r->name)
            tab[i] = *r;
        else
            tab[i] = (ps3dk_prof_spu_desc){(uint32_t)(a + 16u * i), 0, "host-pad", "host"};
        e1h_trace("image-desc %u %s", (unsigned)i, tab[i].name);
    }
    return ps3dk_prof_spu_image_register_table((const uint8_t *)"\x02" "e1-host-build", 14, tab, n, out);
}

/* ---- the GCM device: one bank, one fake context, the executor at flush (R4a Mode S) ---- */
typedef struct rep_ent {
    uint64_t timer;
    uint32_t value, zero;
} rep_ent;
static rep_ent g_rep[GPP_REPORTS] __attribute__((aligned(16)));
static uint32_t g_lab[GPP_LABEL_LAST + 1];
#define CBUF 600u
typedef struct fctx {
    gpp_ctx c; /* first member */
    uint32_t buf[CBUF] __attribute__((aligned(16)));
} fctx;
static fctx g_fx;
static uint64_t g_clock = 1000;
static unsigned g_flushes, g_exec_bad, g_pairs;
static void *rep_a(uint32_t i) { return i < GPP_REPORTS ? &g_rep[i] : NULL; }
static void *lab_a(uint32_t i) { return i <= GPP_LABEL_LAST ? &g_lab[i] : NULL; }
static uint32_t unperm(uint32_t v) { return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16); }
static void exec_pair(const uint32_t *w)
{
    uint32_t r = (w[1] & 0x00ffffffu) / 16u, l = w[3] / 16u;
    if (w[0] != 0x00041800u || (w[1] >> 24) != 1u || (w[1] & 15u) || w[2] != 0x00041d6cu || (w[3] & 15u) ||
        w[4] != 0x00041d70u || r >= 16u || l != 64u) {
        g_exec_bad++;
        e1h_trace("gpu-exec BAD pair");
        return;
    }
    g_clock += 37;
    g_rep[r].timer = g_clock; /* the report before its label */
    g_lab[l] = unperm(w[5]);
    g_pairs++;
}
static void handover(int drop)
{
    uint32_t n = (uint32_t)(g_fx.c.current - g_fx.c.begin);
    if (n % 6u)
        g_exec_bad++;
    if (!drop)
        for (uint32_t k = 0; k + 6 <= n; k += 6)
            exec_pair(g_fx.c.begin + k);
    g_fx.c.current = g_fx.c.begin;
}
static int32_t fcb(gpp_ctx *c, uint32_t count)
{
    (void)c, (void)count;
    handover(0);
    return 0;
}
static void dev_flush(gpp_ctx *c)
{
    (void)c;
    handover(0);
}
static const gpp_dev g_dev = {rep_a, lab_a, dev_flush};
__attribute__((constructor)) static void e1h_gcm_setup(void)
{
    for (uint32_t i = 0; i < GPP_REPORTS; i++)
        g_rep[i] = (rep_ent){0x5100000000000000ull | i, 0xAAAA0000u | i, 0xBBBB0000u | i};
    for (uint32_t i = 0; i <= GPP_LABEL_LAST; i++)
        g_lab[i] = 0x7700u | i;
    g_fx.c.begin = g_fx.c.current = g_fx.buf;
    g_fx.c.end = g_fx.buf + CBUF;
    g_fx.c.callback = fcb;
    ps3dk_prof_test_hooks.gpu_gpp_dev = &g_dev;
}
ps3dk_prof_gcm_ctx *e1h_gcm_ctx(void) { return (ps3dk_prof_gcm_ctx *)(void *)&g_fx.c; }
void e1h_gcm_flush(ps3dk_prof_gcm_ctx *ctx)
{
    char k[32];
    (void)ctx;
    snprintf(k, sizeof k, "gpu.drop@%u", ++g_flushes);
    int drop = e1h_f(k) != NULL;
    e1h_trace("gcm-flush %u%s", g_flushes, drop ? " DROPPED (model: never executed)" : "");
    handover(drop);
}
void e1h_gcm_finish(ps3dk_prof_gcm_ctx *ctx, uint32_t ref)
{
    (void)ctx;
    handover(0);
    e1h_trace("gcm-finish %08x pairs %u bad %u", (unsigned)ref, g_pairs, g_exec_bad);
}

/* ---- file seams ---- */
int e1h_mk_open(const char *path, unsigned seq)
{
    uint32_t e;
    char k[32];
    snprintf(k, sizeof k, "mk%02u.open", seq);
    if (e1h_fval(k, &e)) {
        e1h_trace("mk-open %02u -> -1 %08x", seq, (unsigned)e);
        errno = (int)e;
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    e1h_trace("mk-open %02u -> %s", seq, fd >= 0 ? "fd" : "-1");
    return fd;
}
long e1h_mk_write(int fd, const void *b, size_t n, unsigned seq)
{
    char k[32];
    snprintf(k, sizeof k, "mk%02u.write", seq);
    const char *f = e1h_f(k);
    if (f && !strcmp(f, "zero")) {
        e1h_trace("mk-write %02u %zu -> 0", seq, n);
        return 0;
    }
    if (f) {
        uint32_t e = (uint32_t)strtoul(f, NULL, 16);
        e1h_trace("mk-write %02u %zu -> -1 %08x", seq, n, (unsigned)e);
        errno = (int)e;
        return -1;
    }
    long r = (long)write(fd, b, n);
    e1h_trace("mk-write %02u %zu -> %ld", seq, n, r);
    return r;
}
int e1h_mk_fsync(int fd, unsigned seq)
{
    uint32_t e;
    char k[32];
    snprintf(k, sizeof k, "mk%02u.fsync", seq);
    if (e1h_fval(k, &e)) {
        e1h_trace("mk-fsync %02u -> -1 %08x", seq, (unsigned)e);
        errno = (int)e;
        return -1;
    }
    e1h_trace("mk-fsync %02u -> 0", seq);
    return fsync(fd);
}
int e1h_mk_close(int fd, unsigned seq)
{
    uint32_t e;
    char k[32];
    int r = close(fd); /* the descriptor is always released */
    snprintf(k, sizeof k, "mk%02u.close", seq);
    if (e1h_fval(k, &e)) {
        e1h_trace("mk-close %02u -> -1 %08x", seq, (unsigned)e);
        errno = (int)e;
        return -1;
    }
    e1h_trace("mk-close %02u -> %d", seq, r);
    return r;
}
int e1h_mkdir(const char *path)
{
    uint32_t e;
    const char *f;
    int base = !strcmp(path, e1h_base());
    if (base && e1h_fval("mkdir.base", &e)) {
        e1h_trace("mkdir base -> -1 %08x", (unsigned)e);
        errno = (int)e;
        return -1;
    }
    if (!base && (f = e1h_f("mkdir.run")) != NULL) {
        if (!strcmp(f, "exist")) {
            (void)mkdir(path, 0755); /* a run-id collision: the directory already exists */
        } else {
            e1h_trace("mkdir run -> -1 %s", f);
            errno = (int)strtoul(f, NULL, 16);
            return -1;
        }
    }
    int r = mkdir(path, 0755);
    e1h_trace("mkdir %s -> %d", base ? "base" : "run", r);
    return r;
}
int e1h_cap_open(const char *path)
{
    uint32_t e;
    if (e1h_fval("cap.open", &e)) {
        e1h_trace("cap-open -> -1 %08x", (unsigned)e);
        errno = (int)e;
        return -1;
    }
    return open(path, O_RDONLY);
}
long e1h_cap_read(int fd, void *b, size_t n)
{
    uint32_t e;
    if (e1h_fval("cap.read", &e)) {
        e1h_trace("cap-read -> -1 %08x", (unsigned)e);
        errno = (int)e;
        return -1;
    }
    return (long)read(fd, b, n);
}

/* ---- --wrap: the profiler API calls of main.c and the writer calls of e1_spu_core.c ---- */
#define WRAP_RC(key, k, real_call)                                                                                     \
    do {                                                                                                               \
        const unsigned k_ = (k); /* evaluated once: k may be ++counter */                                              \
        const char *f_ = nth(key, k_);                                                                                 \
        if (f_) {                                                                                                      \
            int rc_ = (int)(uint32_t)strtoul(f_, NULL, 16);                                                            \
            e1h_trace("%s %u -> %d (injected)", key, k_, rc_);                                                         \
            return rc_;                                                                                                \
        }                                                                                                              \
        int rr_ = real_call;                                                                                           \
        e1h_trace("%s %u -> %d", key, k_, rr_);                                                                        \
        return rr_;                                                                                                    \
    } while (0)

int __real_ps3dk_prof_set_sink_path(const char *p);
int __wrap_ps3dk_prof_set_sink_path(const char *p) { WRAP_RC("p.set_sink_path", 1u, __real_ps3dk_prof_set_sink_path(p)); }
int __real_ps3dk_prof_init_ex(const ps3dk_prof_config *c, const ps3dk_prof_ext_config *x);
int __wrap_ps3dk_prof_init_ex(const ps3dk_prof_config *c, const ps3dk_prof_ext_config *x)
{
    WRAP_RC("p.init_ex", 1u, __real_ps3dk_prof_init_ex(c, x));
}
int __real_ps3dk_prof_thread_begin(const char *n);
int __wrap_ps3dk_prof_thread_begin(const char *n) { WRAP_RC("p.thread_begin", 1u, __real_ps3dk_prof_thread_begin(n)); }
int __real_ps3dk_prof_spu_image_register_table(const uint8_t *b, uint32_t l, const ps3dk_prof_spu_desc *d, uint32_t n,
                                               uint32_t *o);
int __wrap_ps3dk_prof_spu_image_register_table(const uint8_t *b, uint32_t l, const ps3dk_prof_spu_desc *d, uint32_t n,
                                               uint32_t *o)
{
    WRAP_RC("p.register", 1u, __real_ps3dk_prof_spu_image_register_table(b, l, d, n, o));
}
int __real_ps3dk_prof_spu_lane_create(const char *n, uint32_t i, int s, ps3dk_prof_spu_lane *o);
int __wrap_ps3dk_prof_spu_lane_create(const char *n, uint32_t i, int s, ps3dk_prof_spu_lane *o)
{
    WRAP_RC("p.lane_create", 1u, __real_ps3dk_prof_spu_lane_create(n, i, s, o));
}
int __real_ps3dk_prof_drain(void);
int __wrap_ps3dk_prof_drain(void)
{
    static unsigned k;
    WRAP_RC("p.drain", ++k, __real_ps3dk_prof_drain());
}
int __real_ps3dk_prof_spu_lane_get_stats(uint32_t h, ps3dk_prof_spu_lane_stats *st);
int __wrap_ps3dk_prof_spu_lane_get_stats(uint32_t h, ps3dk_prof_spu_lane_stats *st)
{
    static unsigned k;
    ++k;
    const char *f = nth("p.lane_stats", k);
    if (f && !strcmp(f, "stuck")) { /* the stats never reach the target (a drain that accepts nothing) */
        int rc = __real_ps3dk_prof_spu_lane_get_stats(h, st);
        st->events = 0;
        return rc;
    }
    WRAP_RC("p.lane_stats", k, __real_ps3dk_prof_spu_lane_get_stats(h, st));
}
int __real_ps3dk_prof_spu_lane_end(uint32_t h, uint64_t t, ps3dk_prof_spu_lane_stats *st);
int __wrap_ps3dk_prof_spu_lane_end(uint32_t h, uint64_t t, ps3dk_prof_spu_lane_stats *st)
{
    WRAP_RC("p.lane_end", 1u, __real_ps3dk_prof_spu_lane_end(h, t, st));
}
int __real_ps3dk_prof_gpu_attach(ps3dk_prof_gcm_ctx *c, uint32_t f, uint32_t n, uint32_t l, uint32_t *g);
int __wrap_ps3dk_prof_gpu_attach(ps3dk_prof_gcm_ctx *c, uint32_t f, uint32_t n, uint32_t l, uint32_t *g)
{
    WRAP_RC("p.gpu_attach", 1u, __real_ps3dk_prof_gpu_attach(c, f, n, l, g));
}
int __real_ps3dk_prof_gpu_frame(uint32_t g, ps3dk_prof_token t);
int __wrap_ps3dk_prof_gpu_frame(uint32_t g, ps3dk_prof_token t)
{
    static unsigned k;
    WRAP_RC("p.gpu_frame", ++k, __real_ps3dk_prof_gpu_frame(g, t));
}
int __real_ps3dk_prof_gpu_begin(uint32_t g, ps3dk_prof_gcm_ctx *c, ps3dk_prof_desc *d, ps3dk_prof_gpu_zone *z);
int __wrap_ps3dk_prof_gpu_begin(uint32_t g, ps3dk_prof_gcm_ctx *c, ps3dk_prof_desc *d, ps3dk_prof_gpu_zone *z)
{
    static unsigned k;
    WRAP_RC("p.gpu_begin", ++k, __real_ps3dk_prof_gpu_begin(g, c, d, z));
}
int __real_ps3dk_prof_gpu_end(uint32_t g, ps3dk_prof_gcm_ctx *c, ps3dk_prof_gpu_zone *z);
int __wrap_ps3dk_prof_gpu_end(uint32_t g, ps3dk_prof_gcm_ctx *c, ps3dk_prof_gpu_zone *z)
{
    static unsigned k;
    WRAP_RC("p.gpu_end", ++k, __real_ps3dk_prof_gpu_end(g, c, z));
}
int __real_ps3dk_prof_gpu_mark(uint32_t g, ps3dk_prof_gcm_ctx *c, ps3dk_prof_desc *d);
int __wrap_ps3dk_prof_gpu_mark(uint32_t g, ps3dk_prof_gcm_ctx *c, ps3dk_prof_desc *d)
{
    static unsigned k;
    WRAP_RC("p.gpu_mark", ++k, __real_ps3dk_prof_gpu_mark(g, c, d));
}
int __real_ps3dk_prof_gpu_get_stats(uint32_t g, ps3dk_prof_gpu_stats *st);
int __wrap_ps3dk_prof_gpu_get_stats(uint32_t g, ps3dk_prof_gpu_stats *st)
{
    static unsigned k;
    WRAP_RC("p.gpu_stats", ++k, __real_ps3dk_prof_gpu_get_stats(g, st));
}
int __real_ps3dk_prof_gpu_detach(uint32_t g, ps3dk_prof_gcm_ctx *c, uint64_t t);
int __wrap_ps3dk_prof_gpu_detach(uint32_t g, ps3dk_prof_gcm_ctx *c, uint64_t t)
{
    WRAP_RC("p.gpu_detach", 1u, __real_ps3dk_prof_gpu_detach(g, c, t));
}
int __real_ps3dk_prof_gpu_release(uint32_t g, uint32_t *r);
int __wrap_ps3dk_prof_gpu_release(uint32_t g, uint32_t *r)
{
    WRAP_RC("p.gpu_release", 1u, __real_ps3dk_prof_gpu_release(g, r));
}
int __real_ps3dk_prof_shutdown(void);
int __wrap_ps3dk_prof_shutdown(void) { WRAP_RC("p.shutdown", 1u, __real_ps3dk_prof_shutdown()); }

/* the writer: w.attach, w.flush@k, w.dec@k, w.fini inject a return code without the real call; w.badidx@k latches an
 * invalid descriptor index right after the k-th real flush returned (the core reports it as call 5 at its next
 * flush) */
int __real_ps3dk_prof_spu_attach(ps3dk_prof_spu_writer *w, uint64_t er, uint32_t rs, uint64_t ec, uint32_t tag,
                                 void *st, uint32_t cap, uint32_t fl);
int __wrap_ps3dk_prof_spu_attach(ps3dk_prof_spu_writer *w, uint64_t er, uint32_t rs, uint64_t ec, uint32_t tag,
                                 void *st, uint32_t cap, uint32_t fl)
{
    WRAP_RC("w.attach", 1u, __real_ps3dk_prof_spu_attach(w, er, rs, ec, tag, st, cap, fl));
}
int __real_ps3dk_prof_spu_flush(ps3dk_prof_spu_writer *w);
int __wrap_ps3dk_prof_spu_flush(ps3dk_prof_spu_writer *w)
{
    static unsigned k;
    char kk[32];
    ++k;
    const char *f = nth("w.flush", k);
    if (f) {
        int rc = (int)(uint32_t)strtoul(f, NULL, 16);
        e1h_trace("w.flush %u -> %d (injected)", k, rc);
        return rc;
    }
    int rc = __real_ps3dk_prof_spu_flush(w);
    snprintf(kk, sizeof kk, "w.badidx@%u", k);
    if (rc == 0 && e1h_f(kk)) {
        w->bad_index = 1;
        e1h_trace("w.flush %u -> 0, then a latched bad index (injected)", k);
    } else {
        e1h_trace("w.flush %u -> %d", k, rc);
    }
    return rc;
}
int __real_ps3dk_prof_spu_dec_write(ps3dk_prof_spu_writer *w, uint32_t v);
int __wrap_ps3dk_prof_spu_dec_write(ps3dk_prof_spu_writer *w, uint32_t v)
{
    static unsigned k;
    WRAP_RC("w.dec", ++k, __real_ps3dk_prof_spu_dec_write(w, v));
}
int __real_ps3dk_prof_spu_fini(ps3dk_prof_spu_writer *w);
int __wrap_ps3dk_prof_spu_fini(ps3dk_prof_spu_writer *w) { WRAP_RC("w.fini", 1u, __real_ps3dk_prof_spu_fini(w)); }
