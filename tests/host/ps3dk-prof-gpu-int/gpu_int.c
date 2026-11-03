/* P3.3 slice R2: GPU (RSX) domains inside the real runtime (host PAL).  The GPU device is an in-order emulator behind
 * the test-hook port (host model, not target evidence; A1/A2 remain assumptions).  One case per process (init is
 * one-shot).  Each case prints "R2 <case> PASS" or "R2 <case> FAIL ..."; captures go to <out-dir> as <name>.prof
 * with <name>.truth (what the calls did, per number) and <name>.expect (the case's exact claims) for gpu_oracle.py.
 *
 * usage: gpu <case> <out-dir> [arg] */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "prg.h"
#include "prof_gpu.h"
#include "prof_spu.h"
#include "prof_test_hooks.h"
#include "psp_host_emul.h"
#include "psp_spu.h"

uint32_t ps3dk_prof__test_gpu_users(unsigned i);
void ps3dk_prof__test_gpu_set_users(unsigned i, uint32_t v);
uint32_t ps3dk_prof__test_gpu_state(unsigned i);
int ps3dk_prof__test_gpu_limits(uint32_t out[8]);
uint64_t ps3dk_prof__test_ext_subtotal(void);
void ps3dk_prof__test_gpu_set_tags(unsigned i, uint32_t tag);
uint32_t ps3dk_prof__test_gpu_tag(unsigned i, unsigned j);

static const char *g_case, *g_out;
static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("R2 %s FAIL line %d: ", g_case, __LINE__);                                                         \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---- the emulated GPU device (in order; host memory under one mutex) ---- */
typedef struct qpair {
    uint32_t r, l, v;
} qpair;
static struct {
    pthread_mutex_t m;
    uint64_t timer[4096];
    uint32_t labelv[256];
    qpair q[1u << 16];
    uint32_t qh, qt;
    uint64_t clock;
    atomic_int auto_exec;     /* execute every queued pair right after each reserve */
    int fail_every, every;    /* every n-th reserve fails (all or nothing) */
    void (*reserve_hook)(void);
    void (*rlabel_hook)(void);
} G = {.m = PTHREAD_MUTEX_INITIALIZER, .clock = 1000};

static void dev_step(uint32_t n)
{
    pthread_mutex_lock(&G.m);
    while (n-- && G.qt < G.qh) {
        qpair p = G.q[G.qt++ & 0xFFFF];
        G.clock += 37;
        G.timer[p.r] = G.clock;
        G.labelv[p.l] = p.v;
    }
    pthread_mutex_unlock(&G.m);
}
static int d_reserve(void *c, uint32_t r, uint32_t l, uint32_t v)
{
    (void)c;
    pthread_mutex_lock(&G.m);
    if (G.fail_every && ++G.every % G.fail_every == 0) {
        pthread_mutex_unlock(&G.m);
        return -1;
    }
    G.q[G.qh++ & 0xFFFF] = (qpair){r, l, v};
    pthread_mutex_unlock(&G.m);
    if (atomic_load(&G.auto_exec))
        dev_step(1u << 30);
    if (G.reserve_hook)
        G.reserve_hook();
    return 0;
}
static void d_flush(void *c) { (void)c; }
static uint32_t d_rlabel(void *c, uint32_t l)
{
    uint32_t v;
    (void)c;
    pthread_mutex_lock(&G.m);
    v = G.labelv[l];
    pthread_mutex_unlock(&G.m);
    if (G.rlabel_hook)
        G.rlabel_hook();
    return v;
}
static void d_wlabel(void *c, uint32_t l, uint32_t v)
{
    (void)c;
    pthread_mutex_lock(&G.m);
    G.labelv[l] = v;
    pthread_mutex_unlock(&G.m);
}
static uint64_t d_rtimer(void *c, uint32_t r)
{
    uint64_t v;
    (void)c;
    pthread_mutex_lock(&G.m);
    v = G.timer[r];
    pthread_mutex_unlock(&G.m);
    return v;
}
static void d_wtimer(void *c, uint32_t r, uint64_t v)
{
    (void)c;
    pthread_mutex_lock(&G.m);
    G.timer[r] = v;
    pthread_mutex_unlock(&G.m);
}
static void d_sync(void *c) { (void)c; }
static const prg_port g_dev = {NULL, d_reserve, d_flush, d_rlabel, d_wlabel, d_rtimer, d_wtimer, d_sync,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/* ---- the I2 host SPU producer (copied from tests/host/ps3dk-prof-spu-int/i2_test.c): psp_spu over the host
 * emulation of the line premises ---- */
/* ---- the host SPU port: real memory, the emulated line protocol, version-checked putllc ---- */
typedef struct tev {
    uint32_t dec;
    uint16_t desc;
    uint8_t kind, depth;
} tev;

typedef struct prod {
    psp_spu s;
    psp_port port;
    uint8_t buf[(64 + 2 * PSP_HEAD) * 8] __attribute__((aligned(16)));
    ps3dk_prof_spu_lane lane;
    uint32_t dec, tag_mask, last_stat;
    uint64_t res_ver;
    int res_held, blocking;
    tev truth[100000];
    uint64_t ntruth;
    int events, stop_flag, inited;
    atomic_int stop;
    pthread_t th;
} prod;

static uint8_t *at(uint64_t ea) { return (uint8_t *)(uintptr_t)ea; }
static uint32_t p_rd_dec(void *c) { prod *p = c; return p->dec -= 7; }
static void p_wr_dec(void *c, uint32_t v) { ((prod *)c)->dec = v; }
static int p_blocking(void *c) { return ((prod *)c)->blocking; }
static uint32_t p_rd_mask(void *c) { return ((prod *)c)->tag_mask; }
static void p_wr_mask(void *c, uint32_t m) { ((prod *)c)->tag_mask = m; }
static uint32_t p_tag_wait(void *c) { return ((prod *)c)->tag_mask; }
static int is_line_a(prod *p, uint64_t ea) { return ea == p->lane.ea_ctl; }
static void p_getllar(void *c, uint64_t ea, void *ls)
{
    prod *p = c;
    int lock = !(is_line_a(p, ea) && psp_emul_drop_line_a);
    if (lock)
        psp_emul_line_lock(ea);
    memcpy(ls, at(ea), 128);
    p->res_ver = psp_emul_version(ea);
    p->res_held = 1;
    if (lock)
        psp_emul_line_unlock(ea);
    p->last_stat = PSP_AS_GETLLAR;
}
static void p_putllc(void *c, uint64_t ea, const void *ls)
{
    prod *p = c;
    psp_emul_line_lock(ea);
    if (p->res_held && psp_emul_version(ea) == p->res_ver) {
        memcpy(at(ea), ls, 128);
        psp_emul_version_bump(ea);
        p->last_stat = PSP_AS_PUTLLC_OK;
    } else {
        p->last_stat = PSP_AS_PUTLLC_LOST; /* the reservation was lost: nothing is written */
    }
    p->res_held = 0;
    psp_emul_line_unlock(ea);
}
static void p_putlluc(void *c, uint64_t ea, const void *ls)
{
    prod *p = c;
    psp_emul_line_lock(ea);
    memcpy(at(ea), ls, 128);
    psp_emul_line_unlock(ea);
    p->last_stat = PSP_AS_PUTLLUC;
}
static uint32_t p_atomic(void *c)
{
    prod *p = c;
    uint32_t s = p->last_stat;
    p->last_stat = 0;
    return s;
}
static void p_put(void *c, const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
    (void)c;
    (void)tag;
    memcpy(at(ea), ls, size);
}

static int prod_init(prod *p, int own_dec)
{
    p->dec = 0xFFFFF000u;
    p->tag_mask = 0x11;
    p->port = (psp_port){p, p_rd_dec, p_wr_dec, p_blocking, p_rd_mask, p_wr_mask, p_tag_wait, p_getllar, p_putllc,
                         p_putlluc, p_atomic, p_put};
    return psp_spu_init(&p->s, &p->port, p->lane.ea_ring, p->lane.ring_slots, p->lane.ea_ctl, 3, p->buf, 64,
                        own_dec);
}

static void ev(prod *p, int begin)
{
    uint64_t q = p->s.seq;
    if (begin)
        psp_spu_begin(&p->s, 0);
    else
        psp_spu_end(&p->s, 0);
    if (p->s.seq != q && p->ntruth < 100000) {
        tev *t = &p->truth[p->ntruth++];
        t->dec = p->dec;
        t->desc = 0;
        t->kind = begin ? PSP_K_BEGIN : PSP_K_END;
        t->depth = (uint8_t)(begin ? p->s.depth - 1 : p->s.depth);
    }
}

/* a producer: N balanced zones, a flush every 16 events, then fini (or until stop for the live cases) */
static void *producer(void *arg)
{
    prod *p = arg;
    int i;
    if (!p->inited && prod_init(p, 0) != PSP_OK)
        return NULL;
    p->inited = 1;
    for (i = 0; p->events < 0 ? !atomic_load(&p->stop) : i < p->events; i++) {
        ev(p, 1);
        ev(p, 0);
        if (i % 8 == 7) {
            while (psp_spu_flush(&p->s) == PSP_EBUSY)
                ;
            if (p->events < 0)
                usleep(200);
        }
    }
    psp_spu_flush(&p->s);
    if (!p->stop_flag)
        psp_spu_fini(&p->s);
    return NULL;
}

static void write_spu_truth(const char *name, prod **ps, int n)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.spu-truth", g_out, name);
    f = fopen(path, "w");
    for (int k = 0; f && k < n; k++) {
        prod *p = ps[k];
        uint64_t ex = p->ntruth < 4096 ? p->ntruth : 4096;
        fprintf(f, "#lane %u base 0 count %llu export %llu\n", p->lane.handle, (unsigned long long)p->ntruth,
                (unsigned long long)ex);
        for (uint64_t q = 0; q < ex; q++)
            fprintf(f, "%u %llu %u %u %u %u\n", p->lane.handle, (unsigned long long)q, p->truth[q].dec,
                    p->truth[q].desc, p->truth[q].kind, p->truth[q].depth);
    }
    if (f)
        fclose(f);
}

/* ---- truth: every NUMBERED call (an OK return), in number order ---- */
typedef struct tr {
    uint8_t kind, depth;
    const char *desc;
    uint32_t owner;
    uint64_t frame;
} tr;
static tr g_tr[70000];
static int g_ntr;
static uint32_t g_cur_owner;
static uint64_t g_cur_frame;
static int g_depth;
static void note(uint8_t kind, const char *desc)
{
    if (kind == 2)
        g_depth--;
    g_tr[g_ntr++] = (tr){kind, (uint8_t)g_depth, desc, g_cur_owner, g_cur_frame};
    if (kind == 1)
        g_depth++;
}

/* ---- the runtime ---- */
static ps3dk_prof_ext_config ext(uint32_t lanes, uint32_t gpus, uint32_t slots)
{
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.spu_lanes_max = lanes;
    x.spu_ring_slots = lanes ? 1024 : 0;
    x.gpu_domains_max = gpus;
    x.gpu_report_slots_max = slots;
    return x;
}
static ps3dk_prof_config cfg(uint32_t mode)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = mode;
    c.app = "r2";
    c.build_id = "r2";
    c.drain_interval_us = 1000;    /* THREAD mode: drain every millisecond */
    c.quiesce_timeout_us = 100000; /* a registered thread paused across shutdown: NOT_QUIESCED after 100 ms */
    return c;
}

/* the callback sink: every write is one framed chunk; fail_type makes the first chunk of that type fail */
static struct {
    FILE *f;
    atomic_int fail_type, failed, writes_after;
} S;
static long cb_write(void *ctx, const void *b, unsigned long n)
{
    const uint8_t *u = b;
    uint32_t type = n >= 4 ? (uint32_t)u[0] | (uint32_t)u[1] << 8 | (uint32_t)u[2] << 16 | (uint32_t)u[3] << 24 : 0;
    (void)ctx;
    if (atomic_load(&S.failed)) {
        atomic_fetch_add(&S.writes_after, 1);
        return -1;
    }
    if (atomic_load(&S.fail_type) && (int)type == atomic_load(&S.fail_type)) {
        atomic_store(&S.failed, 1);
        return -1;
    }
    return fwrite(b, 1, n, S.f) == n ? (long)n : -1;
}
static int cb_close(void *ctx)
{
    (void)ctx;
    return fclose(S.f) == 0 ? 0 : -1;
}

static int start(uint32_t mode, ps3dk_prof_ext_config x, const char *cbname)
{
    ps3dk_prof_config c = cfg(mode);
    ps3dk_prof_test_hooks.gpu_port = &g_dev;
    if (cbname) {
        char path[512];
        ps3dk_prof_sink_callbacks cb = {cb_write, NULL, cb_close, NULL};
        snprintf(path, sizeof path, "%s/%s.prof", g_out, cbname);
        S.f = fopen(path, "wb");
        if (!S.f || ps3dk_prof_set_sink_callbacks(&cb) != 0)
            return -1;
    } else if (ps3dk_prof_set_sink_path(g_out) != 0) {
        return -1;
    }
    return ps3dk_prof_init_ex(&c, &x);
}
static int take_capture(const char *name)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "cd '%s' && f=$(ls r2-*.prof 2>/dev/null | head -1) && [ -n \"$f\" ] && mv \"$f\" '%s.prof'",
             g_out, name);
    return system(cmd) == 0 ? 0 : -1;
}
static void write_truth(const char *name)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.truth", g_out, name);
    f = fopen(path, "w");
    for (int i = 0; i < g_ntr; i++)
        fprintf(f, "%d %u %u %s %u %llu\n", i, g_tr[i].kind, g_tr[i].depth, g_tr[i].desc, g_tr[i].owner,
                (unsigned long long)g_tr[i].frame);
    fclose(f);
}
static FILE *expect(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.expect", g_out, name);
    return fopen(path, "w");
}

/* GPU calls with the truth notes (the macros give every site its own scope-2 descriptor) */
static ps3dk_prof_desc D_A = {"gpu_a", "", "gpu_int.c", 1, 2, 0, 0}, D_B = {"gpu_b", "", "gpu_int.c", 2, 2, 0, 0},
                       D_C = {"gpu_c", "", "gpu_int.c", 3, 2, 0, 0}, D_N = {"gpu_new", "", "gpu_int.c", 4, 2, 0, 0},
                       D_CPU = {"cpu_scope", "", "gpu_int.c", 5, 1, 0, 0};
#define CTX ((ps3dk_prof_gcm_ctx *)(uintptr_t)0x1000)
#define CTX2 ((ps3dk_prof_gcm_ctx *)(uintptr_t)0x2000)
static int B(uint32_t h, ps3dk_prof_desc *d, ps3dk_prof_gpu_zone *z)
{
    int rc = ps3dk_prof_gpu_begin(h, CTX, d, z);
    if (rc == PS3DK_PROF_OK)
        note(1, d->name);
    return rc;
}
static int E(uint32_t h, ps3dk_prof_gpu_zone *z, const char *name)
{
    int rc = ps3dk_prof_gpu_end(h, CTX, z);
    if (rc == PS3DK_PROF_OK)
        note(2, name);
    return rc;
}
static int M(uint32_t h, ps3dk_prof_desc *d)
{
    int rc = ps3dk_prof_gpu_mark(h, CTX, d);
    if (rc == PS3DK_PROF_OK)
        note(3, d->name);
    return rc;
}
static int F(uint32_t h, ps3dk_prof_token t)
{
    int rc = ps3dk_prof_gpu_frame(h, t);
    if (rc == PS3DK_PROF_OK) {
        g_cur_owner = t.owner;
        g_cur_frame = t.frame;
    }
    return rc;
}

/* a CPU thread that registers, begins `frames` frames, then waits for its retirement signal */
typedef struct cpu_owner {
    pthread_t th;
    int frames;
    atomic_int go_retire, retired;
    ps3dk_prof_token tok;
    atomic_int ready;
    uint64_t slot;
} cpu_owner;
static void *owner_main(void *a)
{
    cpu_owner *o = a;
    if (ps3dk_prof_thread_begin("owner") != 0)
        return NULL;
    for (int i = 0; i < o->frames; i++)
        ps3dk_prof_frame();
    o->tok = ps3dk_prof_frame_token();
    o->slot = ps3dk_prof_test_read(PS3DK_PROF_TEST_SLOT);
    atomic_store(&o->ready, 1);
    while (!atomic_load(&o->go_retire))
        usleep(100);
    ps3dk_prof_thread_end();
    atomic_store(&o->retired, 1);
    return NULL;
}
static void owner_start(cpu_owner *o, int frames)
{
    o->frames = frames;
    pthread_create(&o->th, NULL, owner_main, o);
    while (!atomic_load(&o->ready))
        usleep(100);
}
static void owner_retire(cpu_owner *o)
{
    atomic_store(&o->go_retire, 1);
    pthread_join(o->th, NULL);
}

static void finish_expect(const char *name, int exit_code, uint64_t events, uint64_t lost, int final_state,
                          int final_present, const char *extra)
{
    FILE *e = expect(name);
    fprintf(e, "exit %d\nevents %llu\nlost %llu\nfinal_state %d\nfinal_present %d\n%s", exit_code,
            (unsigned long long)events, (unsigned long long)lost, final_state, final_present, extra ? extra : "");
    fclose(e);
}

/* ================================================================ cases */

/* ---- configuration ---- */
static int refused_then(ps3dk_prof_ext_config bad, ps3dk_prof_ext_config good)
{
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    CHECK(ps3dk_prof_set_sink_path(g_out) == 0, "sink");
    CHECK(ps3dk_prof_init_ex(&c, &bad) == PS3DK_PROF_EINVAL, "refused");
    CHECK(ps3dk_prof_init_ex(&c, &good) == PS3DK_PROF_OK, "then the valid config initializes");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}
static int c_cfg_reject(const char *which)
{
    ps3dk_prof_ext_config x = ext(0, 2, 0), good = ext(0, 2, 0);
    if (!strcmp(which, "gpu-5")) x.gpu_domains_max = 5;
    else if (!strcmp(which, "slots-1")) x.gpu_report_slots_max = 1;
    else if (!strcmp(which, "slots-1025")) x.gpu_report_slots_max = 1025;
    else if (!strcmp(which, "slots-without-gpu")) { x = ext(0, 0, 0); x.gpu_report_slots_max = 256; }
    else if (!strcmp(which, "reserved-0")) x.reserved[0] = 1;
    else if (!strcmp(which, "reserved-1")) x.reserved[1] = 1;
    else if (!strcmp(which, "ring-without-spu")) x.spu_ring_slots = 64;
    else if (!strcmp(which, "image-without-spu")) x.spu_image_bytes_max = 4096;
    else if (!strcmp(which, "mem-without-ext")) { x = ext(0, 0, 0); x.spu_mem_max = 1; }
    else { CHECK(0, "unknown reject case %s", which); return 1; }
    return refused_then(x, good);
}
/* the four combinations: storage, ENOTSUP on the absent side, the partition values */
static int c_cfg_combo(const char *which)
{
    uint32_t lim[8], g = 0;
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    ps3dk_prof_ext_config x = !strcmp(which, "spu") ? ext(2, 0, 0) : !strcmp(which, "gpu") ? ext(0, 2, 0)
                              : !strcmp(which, "both") ? ext(2, 2, 64) : ext(0, 0, 0);
    ps3dk_prof_test_hooks.gpu_port = &g_dev;
    CHECK(ps3dk_prof_set_sink_path(g_out) == 0, "sink");
    CHECK(ps3dk_prof_init_ex(&c, &x) == 0, "init");
    ps3dk_prof__test_gpu_limits(lim);
    if (!strcmp(which, "off") || !strcmp(which, "spu")) {
        CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &g) == PS3DK_PROF_ENOTSUP, "attach ENOTSUP without GPU domains");
        CHECK(lim[6] == 0, "no GPU table");
    }
    if (!strcmp(which, "off") || !strcmp(which, "gpu")) {
        ps3dk_prof_spu_lane l;
        CHECK(ps3dk_prof_spu_lane_create("x", 0, 0, &l) == PS3DK_PROF_ENOTSUP, "SPU ENOTSUP without lanes");
        CHECK(lim[0] == 0, "no PSP state");
    }
    if (!strcmp(which, "spu"))
        CHECK(lim[0] == 65536 && lim[1] == 262144 && lim[2] == 4096, "SPU alone keeps the reader's limits");
    if (!strcmp(which, "gpu"))
        CHECK(lim[3] == 4096 && lim[4] == 65536 && lim[5] == 16384 && lim[7] == 256, "GPU ledger, default slots");
    if (!strcmp(which, "both"))
        CHECK(lim[0] == 61440 && lim[1] == 245760 && lim[2] == 4032 && lim[3] == 4096 && lim[5] == 16384 &&
                  lim[7] == 64,
              "the static partition (got %u %u %u / %u %u)", lim[0], lim[1], lim[2], lim[3], lim[5]);
    if (strcmp(which, "off") && strcmp(which, "spu")) {
        CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &g) == 0 && g == 1026, "attach: id 1026");
        CHECK(ps3dk_prof_gpu_detach(g, CTX, 100000) == 0, "detach (no events)");
        CHECK(ps3dk_prof_gpu_release(g, NULL) == 0, "release");
    }
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    if (!strcmp(which, "gpu") || !strcmp(which, "both")) {
        char name[64];
        snprintf(name, sizeof name, "r2-combo-%s", which);
        CHECK(take_capture(name) == 0, "capture");
        write_truth(name);
        finish_expect(name, 0, 0, 0, 1, 1, NULL);
    }
    return 0;
}
/* the extension cap: probe the subtotal, then exactly it (OK) and one byte short (EINVAL) */
static int c_cfg_cap(const char *which, const char *arg)
{
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    ps3dk_prof_ext_config x = ext(1, 2, 64);
    CHECK(ps3dk_prof_set_sink_path(g_out) == 0, "sink");
    if (!strcmp(which, "probe")) {
        CHECK(ps3dk_prof_init_ex(&c, &x) == 0, "init");
        printf("R2 SUBTOTAL %llu\n", (unsigned long long)ps3dk_prof__test_ext_subtotal());
    } else {
        x.spu_mem_max = strtoull(arg, NULL, 10) - (!strcmp(which, "short") ? 1 : 0);
        CHECK(ps3dk_prof_init_ex(&c, &x) == (!strcmp(which, "short") ? PS3DK_PROF_EINVAL : PS3DK_PROF_OK),
              "cap %s", which);
        if (!strcmp(which, "short"))
            return 0;
    }
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}
/* the k-th GPU snap mutex init fails: ENOMEM after unwind; then the same config initializes */
static int c_cfg_init_fail(void)
{
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    ps3dk_prof_ext_config x = ext(1, 3, 0);
    CHECK(ps3dk_prof_set_sink_path(g_out) == 0, "sink");
    ps3dk_prof_test_hooks.gpu_mutex_init_fail_at = 3;
    CHECK(ps3dk_prof_init_ex(&c, &x) == PS3DK_PROF_ENOMEM, "the third snap mutex fails: ENOMEM");
    ps3dk_prof_test_hooks.gpu_mutex_init_fail_at = 0;
    CHECK(ps3dk_prof_init_ex(&c, &x) == PS3DK_PROF_OK, "retry succeeds");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}

/* ---- API refusals (one process: every refusal leaves every pin dropped) ---- */
static void *foreign_thread(void *a)
{
    uint32_t h = *(uint32_t *)a;
    ps3dk_prof_gpu_zone z;
    ps3dk_prof_gpu_stats st;
    CHECK(ps3dk_prof_gpu_begin(h, CTX, &D_A, &z) == PS3DK_PROF_EPERM && z.gpu == 0, "foreign begin: EPERM, INERT");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == PS3DK_PROF_EPERM, "foreign stats: EPERM");
    CHECK(ps3dk_prof_gpu_release(h, NULL) == PS3DK_PROF_EPERM, "foreign release: EPERM");
    return NULL;
}
static uint32_t g_reentry_h;
static int g_reentry_rc = 99;
static void reentry_from_reserve(void)
{
    ps3dk_prof_gpu_zone z;
    G.reserve_hook = NULL;
    g_reentry_rc = ps3dk_prof_gpu_begin(g_reentry_h, CTX, &D_A, &z);
}
static int users_all_zero(void)
{
    for (unsigned i = 0; i < 2; i++)
        if (ps3dk_prof__test_gpu_users(i))
            return 0;
    return 1;
}
static int c_api_refusals(void)
{
    uint32_t h = 0, h2 = 0, u;
    ps3dk_prof_gpu_zone z, z2, forged;
    ps3dk_prof_token bad;
    pthread_t th;
    cpu_owner own = {0};
    uint8_t sentinel[sizeof(ps3dk_prof_gpu_zone)];
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h) == PS3DK_PROF_ENOTINIT, "before init: ENOTINIT");
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 2, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("main") == 0, "thread_begin");
    /* attach */
    CHECK(ps3dk_prof_gpu_attach(NULL, 0, 16, 64, &h) == PS3DK_PROF_EINVAL, "NULL ctx");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, NULL) == PS3DK_PROF_EINVAL, "NULL out");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 1, 64, &h) == PS3DK_PROF_EINVAL, "count 1");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 65, 64, &h) == PS3DK_PROF_EINVAL, "count over the configured slots");
    CHECK(ps3dk_prof_gpu_attach(CTX, 2048 - 15, 16, 64, &h) == PS3DK_PROF_EINVAL, "reports past the area");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0xFFFFFFF0u, 16, 64, &h) == PS3DK_PROF_EINVAL, "report_first near u32 max");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 63, &h) == PS3DK_PROF_EINVAL, "label below the user range");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 256, &h) == PS3DK_PROF_EINVAL, "label above the user range");
    CHECK(ps3dk_prof_gpu_attach(CTX, 2048 - 16, 16, 64, &h) == 0 && h == 1026, "exactly the last reports: OK");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 65, &h2) == PS3DK_PROF_EINVAL, "same ctx");
    CHECK(ps3dk_prof_gpu_attach(CTX2, 2048 - 20, 8, 65, &h2) == PS3DK_PROF_EINVAL, "overlapping reports");
    CHECK(ps3dk_prof_gpu_attach(CTX2, 0, 16, 64, &h2) == PS3DK_PROF_EINVAL, "same label");
    CHECK(ps3dk_prof_gpu_attach(CTX2, 0, 16, 65, &h2) == 0 && h2 == 1027, "a disjoint second domain");
    CHECK(ps3dk_prof_gpu_attach((ps3dk_prof_gcm_ctx *)(uintptr_t)0x3000, 100, 16, 66, &u) == PS3DK_PROF_EFULL,
          "table full: EFULL");
    /* zones */
    memset(sentinel, 0xA5, sizeof sentinel);
    CHECK(ps3dk_prof_gpu_begin(h, CTX, &D_A, NULL) == PS3DK_PROF_EINVAL, "NULL out: EINVAL, no write");
    memset(&z, 0xA5, sizeof z);
    CHECK(ps3dk_prof_gpu_begin(h, CTX2, &D_A, &z) == PS3DK_PROF_EINVAL && z.gpu == 0 && z.begin == 0,
          "wrong ctx: EINVAL, INERT written");
    CHECK(ps3dk_prof_gpu_begin(h, CTX, &D_CPU, &z) == PS3DK_PROF_EINVAL && z.gpu == 0, "a scope-1 descriptor");
    CHECK(ps3dk_prof_gpu_begin(999, CTX, &D_A, &z) == PS3DK_PROF_EINVAL, "an unknown handle");
    CHECK(ps3dk_prof_gpu_begin(0, CTX, &D_A, &z) == PS3DK_PROF_EINVAL, "handle 0");
    CHECK(B(h, &D_A, &z) == 0 && z.gpu == h, "begin");
    forged = z;
    forged.begin += 5;
    CHECK(ps3dk_prof_gpu_end(h, CTX, &forged) == PS3DK_PROF_EINVAL, "a forged zone");
    z2 = (ps3dk_prof_gpu_zone)PS3DK_PROF_GPU_ZONE_INIT;
    CHECK(ps3dk_prof_gpu_end(h, CTX, &z2) == PS3DK_PROF_EINVAL, "an INERT zone");
    forged = z;
    forged.gpu = h2;
    CHECK(ps3dk_prof_gpu_end(h2, CTX2, &forged) == PS3DK_PROF_EINVAL, "another domain's zone");
    CHECK(E(h, &z, "gpu_a") == 0, "end");
    CHECK(ps3dk_prof_gpu_end(h, CTX, &z) == PS3DK_PROF_EINVAL, "the same zone again");
    CHECK(M(h, &D_B) == 0, "mark");
    CHECK(ps3dk_prof_gpu_mark(h, CTX, &D_CPU) == PS3DK_PROF_EINVAL, "mark with a scope-1 descriptor");
    /* frame tokens */
    owner_start(&own, 2);
    bad = own.tok;
    bad.reserved = 1;
    CHECK(ps3dk_prof_gpu_frame(h, bad) == PS3DK_PROF_EINVAL, "reserved nonzero");
    bad = own.tok;
    bad.owner = 77777;
    CHECK(ps3dk_prof_gpu_frame(h, bad) == PS3DK_PROF_EINVAL, "unknown owner");
    bad = own.tok;
    bad.frame = own.tok.frame + 1;
    CHECK(ps3dk_prof_gpu_frame(h, bad) == PS3DK_PROF_EINVAL, "a frame not yet begun");
    bad = (ps3dk_prof_token){0, 0, 5};
    CHECK(ps3dk_prof_gpu_frame(h, bad) == PS3DK_PROF_EINVAL, "NONE with a frame id");
    CHECK(F(h, own.tok) == 0, "a valid token");
    CHECK(M(h, &D_C) == 0, "a mark attributed to the owner");
    CHECK(F(h, (ps3dk_prof_token){0, 0, 0}) == 0, "back to NONE");
    /* threads and reentry */
    pthread_create(&th, NULL, foreign_thread, &h);
    pthread_join(th, NULL);
    g_reentry_h = h;
    G.reserve_hook = reentry_from_reserve;
    CHECK(M(h, &D_A) == 0 && g_reentry_rc == PS3DK_PROF_EBUSY, "a GPU call from a port callback: EBUSY");
    CHECK(users_all_zero(), "every refusal dropped its pin");
    /* the users bound: refused without incrementing */
    ps3dk_prof__test_gpu_set_users(0, 0x80000000u);
    CHECK(ps3dk_prof_gpu_mark(h, CTX, &D_A) == PS3DK_PROF_EBUSY && ps3dk_prof__test_gpu_users(0) == 0x80000000u,
          "users at the bound: EBUSY, unchanged");
    ps3dk_prof__test_gpu_set_users(0, 0);
    /* finish both domains */
    dev_step(1u << 30);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == 0, "detach");
    CHECK(ps3dk_prof_gpu_release(h, &u) == 0 && u == 1, "release");
    CHECK(ps3dk_prof_gpu_mark(h, CTX, &D_A) == PS3DK_PROF_EINVAL, "a stale handle after release");
    CHECK(ps3dk_prof_gpu_detach(h2, CTX2, 1000000) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "the second");
    owner_retire(&own);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(users_all_zero(), "no pin left");
    CHECK(take_capture("r2-api-refusals") == 0, "capture");
    write_truth("r2-api-refusals");
    finish_expect("r2-api-refusals", 0, (uint64_t)g_ntr, 0, 1, 1, "domains 2\n");
    return 0;
}

/* 64 lifetime ids, then EFULL; ids never reused */
static int c_lifetime(void)
{
    uint32_t h, last = 0;
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 16), NULL) == 0, "start");
    for (int i = 0; i < 64; i++) {
        CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h) == 0 && h == 1026u + (uint32_t)i, "attach %d -> %u", i, h);
        CHECK(h != last, "never reused");
        last = h;
        CHECK(ps3dk_prof_gpu_detach(h, CTX, 100000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "cycle %d", i);
    }
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h) == PS3DK_PROF_EFULL, "the 65th: EFULL");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-lifetime") == 0, "capture");
    write_truth("r2-lifetime");
    finish_expect("r2-lifetime", 0, 0, 0, 1, 1, "domains 64\n");
    return 0;
}

/* ---- end to end: THREAD mode, the GPU executing on its own thread, two owners, detach FINALIZED ---- */
static atomic_int g_gpu_stop;
static void *gpu_thread(void *a)
{
    (void)a;
    while (!atomic_load(&g_gpu_stop)) {
        dev_step(64);
        usleep(50);
    }
    return NULL;
}
static int c_steady(void)
{
    uint32_t h;
    pthread_t gt;
    cpu_owner a = {0}, b = {0};
    ps3dk_prof_gpu_stats st;
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(0, 1, 256), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    owner_start(&a, 3);
    owner_start(&b, 5);
    pthread_create(&gt, NULL, gpu_thread, NULL);
    CHECK(ps3dk_prof_gpu_attach(CTX, 32, 256, 100, &h) == 0, "attach");
    for (int i = 0; i < 400; i++) { /* a frame loop: the owner switches every 20 frames; paced like real frames */
        ps3dk_prof_gpu_zone z;
        if (i % 20 == 0)
            CHECK(F(h, (i / 20) & 1 ? a.tok : b.tok) == 0, "frame %d", i);
        CHECK(B(h, &D_A, &z) == 0, "begin");
        CHECK(M(h, &D_B) == 0, "mark");
        CHECK(E(h, &z, "gpu_a") == 0, "end");
        if (i % 20 == 19)
            usleep(3000);
    }
    CHECK(ps3dk_prof_gpu_calibrate(h, CTX, 1000000) == 0, "calibrate");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 5000000) == 0, "detach");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.retired && st.final_state == 1 && st.final_written &&
              st.events == 1200 && st.lost == 0 && st.brackets == 1,
          "FINALIZED 1200 (events %llu lost %llu state %u)", (unsigned long long)st.events,
          (unsigned long long)st.lost, st.final_state);
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    atomic_store(&g_gpu_stop, 1);
    pthread_join(gt, NULL);
    owner_retire(&a);
    owner_retire(&b);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-steady") == 0, "capture");
    write_truth("r2-steady");
    finish_expect("r2-steady", 0, 1200, 0, 1, 1, "brackets 1\n");
    return 0;
}

/* ---- the descriptor HOLD and the two-owner case (APP mode, deterministic drains).  The ctx owner is a helper
 * thread driven by commands; the main thread is the designated drainer.  The drainer_after_tables hook makes the
 * helper submit while the drain sits between its declarations (2b) and its GPU step (3b). ---- */
static struct {
    pthread_t th;
    atomic_int cmd, done;
    uint32_t h;
    cpu_owner *a, *b;
    ps3dk_prof_gpu_stats st;
} H;
enum { H_NONE, H_ATTACH, H_HELD_PAIR, H_MARK_OLD, H_DETACH, H_RELEASE, H_QUIT, H_DETACH_SHORT, H_ORPHAN_EVENTS, H_STATS,
       H_MARK_NEW, H_THREAD_END };
static int g_rc_detach;
static void *helper_main(void *a)
{
    (void)a;
    CHECK(ps3dk_prof_thread_begin("ctx-owner") == 0, "helper thread_begin");
    for (;;) {
        int c;
        while (!(c = atomic_load(&H.cmd)))
            usleep(50);
        switch (c) {
        case H_ATTACH: CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &H.h) == 0, "helper attach"); break;
        case H_HELD_PAIR: /* owner A + a descriptor registered NOW (after 2b), then owner B + a declared one */
            CHECK(F(H.h, H.a->tok) == 0, "frame A");
            CHECK(M(H.h, &D_N) == 0, "mark with a new descriptor (held)");
            CHECK(F(H.h, H.b->tok) == 0, "frame B");
            CHECK(M(H.h, &D_A) == 0, "mark attributed to B");
            break;
        case H_MARK_OLD: CHECK(M(H.h, &D_A) == 0, "mark"); break;
        case H_MARK_NEW: CHECK(M(H.h, &D_N) == 0, "mark with a descriptor registered after 2b"); break;
        case H_ORPHAN_EVENTS:
            CHECK(F(H.h, H.a->tok) == 0, "frame from the generation-zero owner");
            CHECK(M(H.h, &D_A) == 0, "a mark naming it");
            break;
        case H_DETACH: g_rc_detach = ps3dk_prof_gpu_detach(H.h, CTX, 2000000); break;
        case H_STATS: CHECK(ps3dk_prof_gpu_get_stats(H.h, &H.st) == 0, "helper stats"); break;
        case H_DETACH_SHORT: g_rc_detach = ps3dk_prof_gpu_detach(H.h, CTX, 20000); break;
        case H_RELEASE: CHECK(ps3dk_prof_gpu_release(H.h, NULL) == 0, "helper release"); break;
        case H_THREAD_END: ps3dk_prof_thread_end(); break;
        case H_QUIT:
            ps3dk_prof_thread_end(); /* a no-op after H_THREAD_END */
            atomic_store(&H.cmd, 0);
            atomic_store(&H.done, 1);
            return NULL;
        }
        atomic_store(&H.cmd, 0);
        atomic_store(&H.done, 1);
    }
}
static void hcmd(int c)
{
    atomic_store(&H.done, 0);
    atomic_store(&H.cmd, c);
    while (!atomic_load(&H.done))
        usleep(50);
}
/* a command the main (designated APP drain) thread must keep draining for: a non-designated detach only progresses
 * through the designated thread's drains (the drainer alone finalizes) */
static void hcmd_draining(int c)
{
    atomic_store(&H.done, 0);
    atomic_store(&H.cmd, c);
    while (!atomic_load(&H.done)) {
        (void)ps3dk_prof_drain();
        usleep(200);
    }
}
static int g_hook_cmd;
static void after_tables(void)
{
    if (g_hook_cmd) {
        int c = g_hook_cmd;
        g_hook_cmd = 0;
        hcmd(c);
    }
}
static void hold_setup(int frames_a)
{
    static cpu_owner a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    H.a = &a;
    H.b = &b;
    atomic_store(&G.auto_exec, 1);
    pthread_create(&H.th, NULL, helper_main, NULL);
    hcmd(H_ATTACH);
    owner_start(&a, frames_a);
    owner_start(&b, 2);
    CHECK(ps3dk_prof_drain() == 0, "drain 1: THREADS for A and B");
    ps3dk_prof_test_hooks.drainer_after_tables = after_tables;
}
/* codex's case: two owners queued, the first prefix held, the second owner declared then RETIRED with its slot freed
 * and REUSED before the drainer ever reads its event: its evidence must come from the free scan (decl_acc of that
 * slot then names the new incarnation, which must never bind); then everything unblocks and is emitted exactly */
static int c_two_owner(void)
{
    cpu_owner c = {0};
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("drainer") == 0, "thread_begin");
    hold_setup(2);
    atomic_store(&G.auto_exec, 0); /* the GPU holds the prefix: nothing is read until it runs */
    g_hook_cmd = H_HELD_PAIR;
    CHECK(ps3dk_prof_drain() == 0, "drain 2: the pair (owner A, then owner B) is submitted, not executed");
    owner_retire(H.b);
    CHECK(ps3dk_prof_drain() == 0, "drain 3: B's slot is freed (the free scan records B's evidence)");
    owner_start(&c, 1);
    CHECK(c.slot == H.b->slot, "C reuses B's freed slot (B %llu, C %llu)", (unsigned long long)H.b->slot,
          (unsigned long long)c.slot);
    CHECK(ps3dk_prof_drain() == 0, "drain 4: C declared (decl_acc of that slot now names C)");
    dev_step(1u << 30);
    CHECK(ps3dk_prof_drain() == 0, "drain 5: both emitted with their exact owners");
    hcmd(H_STATS);
    CHECK(H.st.events == 2 && H.st.held == 0 && H.st.attribution_orphaned == 0, "2 events, no hold, no orphan");
    hcmd_draining(H_DETACH);
    CHECK(g_rc_detach == 0, "detach FINALIZED: no permanent hold (got %d)", g_rc_detach);
    hcmd(H_RELEASE);
    hcmd(H_QUIT);
    pthread_join(H.th, NULL);
    owner_retire(H.a);
    owner_retire(&c);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-two-owner") == 0, "capture");
    write_truth("r2-two-owner");
    finish_expect("r2-two-owner", 0, 2, 0, 1, 1, NULL);
    return 0;
}
/* the descriptor HOLD: a descriptor registered after the drain's DESCRIPTORS step (2b) and executed before its GPU
 * step (3b): held there (the prefix stops), declared and emitted by the next drain */
static int c_desc_hold(void)
{
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("drainer") == 0, "thread_begin");
    hold_setup(1);
    g_hook_cmd = H_MARK_NEW;
    CHECK(ps3dk_prof_drain() == 0, "drain 2");
    hcmd(H_STATS);
    CHECK(H.st.held >= 1 && H.st.events == 0, "held at its descriptor (held %llu events %llu)",
          (unsigned long long)H.st.held, (unsigned long long)H.st.events);
    hcmd(H_MARK_OLD);
    CHECK(ps3dk_prof_drain() == 0, "drain 3: declared, both emitted in order");
    hcmd(H_STATS);
    CHECK(H.st.events == 2, "2 events");
    hcmd_draining(H_DETACH);
    CHECK(g_rc_detach == 0, "detach");
    hcmd(H_RELEASE);
    hcmd(H_QUIT);
    pthread_join(H.th, NULL);
    owner_retire(H.a);
    owner_retire(H.b);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-desc-hold") == 0, "capture");
    write_truth("r2-desc-hold");
    finish_expect("r2-desc-hold", 0, 2, 0, 1, 1, NULL);
    return 0;
}
/* generation zero: the THREADS identity of that incarnation is never declared (the pre-existing limitation): its
 * events are ORPHAN-held, never rewritten; detach times out; the seal covers them with LOSS 4 + ABANDONED */
static int c_orphan(void)
{
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("drainer") == 0, "thread_begin"); /* slot 0 */
    {
        static cpu_owner a, b;
        H.a = &a;
        H.b = &b;
        atomic_store(&G.auto_exec, 1);
        pthread_create(&H.th, NULL, helper_main, NULL);
        hcmd(H_ATTACH); /* the helper registers into slot 1 on its first descriptor use; attach does not register */
        ps3dk_prof_test_hooks.slot_generation_seed_set = 1;
        ps3dk_prof_test_hooks.slot_generation_seed = 0xFFFFFFFFu; /* the next registration: generation 0 */
        ps3dk_prof_test_hooks.slot_generation_seed_slot = 2; /* slot 0 the drainer, 1 the helper, 2 the owner */
        owner_start(&a, 1);
        ps3dk_prof_test_hooks.slot_generation_seed_set = 0;
    }
    CHECK(ps3dk_prof_drain() == 0, "drain 1");
    hcmd(H_ORPHAN_EVENTS);
    owner_retire(H.a); /* freed undeclared: the free scan marks the entry ORPHAN */
    CHECK(ps3dk_prof_drain() == 0, "drain 2");
    CHECK(ps3dk_prof_drain() == 0, "drain 3");
    hcmd(H_STATS);
    CHECK(H.st.attribution_orphaned == 1 && H.st.events == 0, "ORPHAN-held, never emitted (orphaned %llu events %llu)",
          (unsigned long long)H.st.attribution_orphaned, (unsigned long long)H.st.events);
    hcmd_draining(H_DETACH_SHORT); /* drains keep running: the timeout is the HOLD, not a missing drain */
    CHECK(g_rc_detach == PS3DK_PROF_ETIMEDOUT, "detach times out on the orphan hold (got %d)", g_rc_detach);
    hcmd(H_THREAD_END);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown seals: LOSS 4 + ABANDONED");
    hcmd(H_RELEASE);
    hcmd(H_QUIT);
    pthread_join(H.th, NULL);
    CHECK(take_capture("r2-orphan") == 0, "capture");
    write_truth("r2-orphan");
    /* exit 2: the PRE-EXISTING generation-zero limitation (the owner's own CPU records name its undeclared
     * incarnation); the GPU domain's part is verified by the domain checker */
    finish_expect("r2-orphan", 2, 0, 1, 2, 1, "loss 0 1 4\npreexisting_malformed 1\n");
    return 0;
}

/* ---- sink failure with a held prefix: DISCARD; detach completes; retired, no FINAL; release OK ---- */
static int c_sink_fail(int type, const char *name)
{
    static cpu_owner a, b;
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    ps3dk_prof_ext_config x = ext(0, 1, 64);
    (void)c;
    (void)x;
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), name) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("drainer") == 0, "thread_begin");
    H.a = &a;
    H.b = &b;
    atomic_store(&G.auto_exec, 1);
    pthread_create(&H.th, NULL, helper_main, NULL);
    hcmd(H_ATTACH);
    owner_start(&a, 1);
    owner_start(&b, 1);
    CHECK(ps3dk_prof_drain() == 0, "drain 1");
    ps3dk_prof_test_hooks.drainer_after_tables = after_tables;
    if (type == 14)
        hcmd(H_MARK_OLD); /* an emittable event: its GPU_EVENTS chunk is the one that fails */
    g_hook_cmd = H_HELD_PAIR; /* a held prefix in place when the failure hits */
    atomic_store(&S.fail_type, type);
    (void)ps3dk_prof_drain();
    if (type == 3)
        hcmd(H_MARK_OLD); /* the next drain's DESCRIPTORS (the held descriptor's) fails */
    (void)ps3dk_prof_drain();
    (void)ps3dk_prof_drain();
    if (type != 16)
        CHECK(atomic_load(&S.failed), "the sink failed on type %d", type);
    if (type != 16) { /* an owner registered AFTER the failure is never declared: only DISCARD lets it progress */
        static cpu_owner c;
        owner_start(&c, 1);
        H.a = &c;
        hcmd(H_ORPHAN_EVENTS);
        (void)ps3dk_prof_drain();
        (void)ps3dk_prof_drain();
        H.a = &a;
        owner_retire(&c);
    }
    hcmd_draining(H_DETACH);
    CHECK(g_rc_detach == 0, "detach completes on the discard path (got %d)", g_rc_detach);
    hcmd(H_RELEASE);
    hcmd(H_QUIT);
    pthread_join(H.th, NULL);
    owner_retire(&a);
    owner_retire(&b);
    ps3dk_prof_thread_end();
    (void)ps3dk_prof_shutdown();
    CHECK(atomic_load(&S.failed), "the sink failed");
    CHECK(atomic_load(&S.writes_after) == 0, "nothing was offered to the sink after its failure (%d)",
          atomic_load(&S.writes_after));
    write_truth(name);
    {
        FILE *e = expect(name);
        fprintf(e, "exit 1\nsink_dead 1\n");
        fclose(e);
    }
    return 0;
}

/* ---- seal paths ---- */
static int c_detach_timeout_then_seal(void)
{
    uint32_t h, rel = 9;
    int rc;
    ps3dk_prof_gpu_stats st;
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    CHECK(M(h, &D_A) == 0 && M(h, &D_B) == 0, "two marks, never executed");
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 20000) == PS3DK_PROF_ETIMEDOUT, "detach times out (stays DETACHING)");
    CHECK(ps3dk_prof_gpu_release(h, &rel) == PS3DK_PROF_EBUSY && rel == 0, "no release before retirement");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown seals from DETACHING");
    dev_step(1u << 30); /* the app's Finish */
    rc = ps3dk_prof_gpu_get_stats(h, &st);
    CHECK(rc == 0 && st.retired && st.final_state == 2 && st.final_written && st.lost == 2,
          "after shutdown the frozen stats stand: retired, ABANDONED (rc %d)", rc);
    rc = ps3dk_prof_gpu_release(h, &rel);
    CHECK(rc == 0 && rel == 1, "release after the seal and the app's Finish (rc %d released %u)", rc, rel);
    ps3dk_prof_thread_end();
    CHECK(take_capture("r2-detach-timeout-seal") == 0, "capture");
    write_truth("r2-detach-timeout-seal");
    finish_expect("r2-detach-timeout-seal", 4, 0, 2, 2, 1, "loss 0 2 4\n");
    return 0;
}

/* ---- forced schedules ---- */
static atomic_int g_pause_stage, g_pause_go;
static void pause_after_pin(void)
{
    if (atomic_load(&g_pause_stage) == 1) {
        atomic_store(&g_pause_stage, 2);
        while (!atomic_load(&g_pause_go))
            usleep(50);
    }
}
static uint32_t g_stale_h;
static int g_stale_rc = 99;
static void *stale_caller(void *a)
{
    ps3dk_prof_gpu_stats st;
    (void)a;
    g_stale_rc = ps3dk_prof_gpu_get_stats(g_stale_h, &st);
    return NULL;
}
static int c_sched_stale(void)
{
    uint32_t h1, h2, h3;
    pthread_t th;
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 2, 16), NULL) == 0, "start");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h1) == 0, "attach X");
    CHECK(ps3dk_prof_gpu_detach(h1, CTX, 100000) == 0 && ps3dk_prof_gpu_release(h1, NULL) == 0, "X released");
    /* a stale caller pinned on slot 0 (after users++, before its id load) across a reattach */
    g_stale_h = h1;
    ps3dk_prof_test_hooks.gpu_after_pin_inc = pause_after_pin;
    atomic_store(&g_pause_stage, 1);
    pthread_create(&th, NULL, stale_caller, NULL);
    while (atomic_load(&g_pause_stage) != 2)
        usleep(50);
    CHECK(ps3dk_prof__test_gpu_users(0) == 1, "slot 0 pinned by the stale caller");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h2) == 0 && h2 == 1027, "reattach");
    CHECK(ps3dk_prof__test_gpu_state(0) == 0 && ps3dk_prof__test_gpu_state(1) == 1,
          "the pinned slot was skipped: the new domain is in slot 1");
    CHECK(ps3dk_prof_gpu_attach(CTX2, 32, 16, 65, &h3) == PS3DK_PROF_EFULL, "the pinned slot is not reusable: EFULL");
    atomic_store(&g_pause_go, 1);
    pthread_join(th, NULL);
    ps3dk_prof_test_hooks.gpu_after_pin_inc = NULL;
    CHECK(g_stale_rc == PS3DK_PROF_EINVAL, "the stale call resumes and misses (EINVAL), got %d", g_stale_rc);
    CHECK(ps3dk_prof__test_gpu_users(0) == 0 && ps3dk_prof__test_gpu_users(1) == 0, "pins dropped");
    CHECK(ps3dk_prof_gpu_attach(CTX2, 32, 16, 65, &h3) == 0 && h3 == 1028, "the slot is reusable after the unpin");
    CHECK(ps3dk_prof_gpu_detach(h2, CTX, 100000) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "h2");
    CHECK(ps3dk_prof_gpu_detach(h3, CTX2, 100000) == 0 && ps3dk_prof_gpu_release(h3, NULL) == 0, "h3");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-sched-stale") == 0, "capture");
    write_truth("r2-sched-stale");
    finish_expect("r2-sched-stale", 0, 0, 0, 1, 1, "domains 3\n");
    return 0;
}
/* shutdown between the wrapper's shutdown check and the core: the core refuses (the seal's SC CAS came first); and a
 * call admitted by the core, paused inside reserve across the seal: frozen out of the file, a reentrant release is
 * refused, and the call completes after the seal */
static uint32_t g_sh;
static int g_sh_mark_rc = 99, g_sh_release_rc = 99, g_sh_reentry_rc = 99, g_sh_mode;
static void sh_pause(void)
{
    if (atomic_load(&g_pause_stage) == 1) {
        atomic_store(&g_pause_stage, 2);
        while (!atomic_load(&g_pause_go))
            usleep(50);
        if (g_sh_mode == 2) /* inside reserve: the caller's own release is reentry */
            g_sh_reentry_rc = ps3dk_prof_gpu_release(g_sh, NULL);
    }
}
static void *sh_owner(void *a)
{
    (void)a;
    atomic_store(&G.auto_exec, 1);
    if (ps3dk_prof_thread_begin("render") != 0 || ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &g_sh) != 0)
        return NULL;
    if (M(g_sh, &D_A) != 0) /* number 0: also registers the descriptor before the shutdown claim */
        return NULL;
    usleep(20000); /* drained (THREAD mode, 1 ms) */
    if (g_sh_mode == 1)
        ps3dk_prof_test_hooks.gpu_after_prologue = sh_pause;
    else
        G.reserve_hook = sh_pause;
    atomic_store(&g_pause_stage, 1);
    g_sh_mark_rc = M(g_sh, &D_A);
    ps3dk_prof_test_hooks.gpu_after_prologue = NULL;
    G.reserve_hook = NULL;
    dev_step(1u << 30); /* the app's Finish */
    g_sh_release_rc = ps3dk_prof_gpu_release(g_sh, NULL);
    return NULL;
}
static int c_sched_shutdown(const char *which)
{
    pthread_t th;
    char name[64];
    g_sh_mode = !strcmp(which, "after-check") ? 1 : 2;
    snprintf(name, sizeof name, "r2-sched-%s", which);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(0, 1, 64), NULL) == 0, "start");
    pthread_create(&th, NULL, sh_owner, NULL);
    while (atomic_load(&g_pause_stage) != 2)
        usleep(50);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_ENOTQUIESCED,
          "shutdown seals while the registered caller is paused (NOT_QUIESCED after the bound)");
    atomic_store(&g_pause_go, 1);
    pthread_join(th, NULL);
    if (g_sh_mode == 1) {
        CHECK(g_sh_mark_rc == PS3DK_PROF_ESTOPPED, "after the wrapper check: the core refuses (got %d)", g_sh_mark_rc);
    } else {
        CHECK(g_sh_reentry_rc == PS3DK_PROF_EBUSY, "release from inside the paused call: EBUSY (got %d)",
              g_sh_reentry_rc);
        CHECK(g_sh_mark_rc == PS3DK_PROF_OK, "the admitted call completes after the seal (got %d)", g_sh_mark_rc);
    }
    CHECK(g_sh_release_rc == PS3DK_PROF_OK, "release after the call's exit and the app's Finish (got %d)",
          g_sh_release_rc);
    CHECK(take_capture(name) == 0, "capture");
    g_ntr = 1; /* number 0 only: the paused call (numbered or not) lies beyond the frozen frontier */
    write_truth(name);
    finish_expect(name, 1, 1, 0, 1, 1, "not_quiesced 1\n"); /* the caller paused across shutdown */
    return 0;
}

/* ---- detach on the designated APP drain thread: the core polls UNLOCKED and its inline drain takes the extension
 * mutex itself (a wrapper holding it across the core would self-deadlock: the watchdog kills that) ---- */
static int c_detach_inline(void)
{
    uint32_t h;
    alarm(20);
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    CHECK(M(h, &D_A) == 0 && M(h, &D_B) == 0, "two marks, no explicit drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 2000000) == 0, "detach completes through its inline drains");
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    alarm(0);
    CHECK(take_capture("r2-detach-inline") == 0, "capture");
    write_truth("r2-detach-inline");
    finish_expect("r2-detach-inline", 0, 2, 0, 1, 1, NULL);
    return 0;
}

/* ---- threaded (TSan): the render thread switches frame owners while owners churn (register, frame, retire, their
 * slots freed and reused) under the drain thread and the GPU thread.  nolock: the NEGATIVE control (the snap mutex
 * does nothing: TSan must report the publication race) ---- */
static pthread_mutex_t g_tok_m = PTHREAD_MUTEX_INITIALIZER;
static ps3dk_prof_token g_toks[4];
static atomic_int g_churn_stop;
static void *churn_one(void *a)
{
    int k = (int)(intptr_t)a;
    if (ps3dk_prof_thread_begin("churn") != 0)
        return NULL;
    ps3dk_prof_frame();
    pthread_mutex_lock(&g_tok_m);
    g_toks[k] = ps3dk_prof_frame_token();
    pthread_mutex_unlock(&g_tok_m);
    usleep(3000 + 1000 * k);
    pthread_mutex_lock(&g_tok_m);
    g_toks[k] = (ps3dk_prof_token){0, 0, 0};
    pthread_mutex_unlock(&g_tok_m);
    ps3dk_prof_thread_end();
    return NULL;
}
static void *churn_main(void *a)
{
    (void)a;
    for (int i = 0; !atomic_load(&g_churn_stop); i++) {
        pthread_t t[4];
        for (int k = 0; k < 4; k++)
            pthread_create(&t[k], NULL, churn_one, (void *)(intptr_t)k);
        for (int k = 0; k < 4; k++)
            pthread_join(t[k], NULL);
    }
    return NULL;
}
static int c_tsan(int nolock)
{
    uint32_t h;
    pthread_t gt, ct;
    ps3dk_prof_gpu_stats st;
    int frames_ok = 0, frames_refused = 0, frames_attr = 0;
    char name[64];
    snprintf(name, sizeof name, "r2-tsan-%s", nolock ? "nolock" : "run");
    ps3dk_prof_test_hooks.gpu_snap_nolock = nolock;
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(0, 1, 256), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    pthread_create(&gt, NULL, gpu_thread, NULL);
    pthread_create(&ct, NULL, churn_main, NULL);
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 256, 64, &h) == 0, "attach");
    for (int i = 0; i < 3000; i++) {
        ps3dk_prof_gpu_zone z;
        if (i % 10 == 0) {
            ps3dk_prof_token t;
            pthread_mutex_lock(&g_tok_m);
            t = g_toks[(i / 10) % 4];
            pthread_mutex_unlock(&g_tok_m);
            int rc = F(h, t); /* the owner may retire between the copy and the call: EINVAL; entries busy: EBUSY */
            if (rc == 0) {
                frames_ok++;
                frames_attr += t.owner != 0;
            }
            else if (rc == PS3DK_PROF_EINVAL || rc == PS3DK_PROF_EBUSY)
                frames_refused++;
            else
                CHECK(0, "frame rc %d", rc);
        }
        CHECK(B(h, &D_A, &z) == 0, "begin");
        CHECK(M(h, &D_B) == 0, "mark");
        CHECK(E(h, &z, "gpu_a") == 0, "end");
        if (i % 10 == 9)
            usleep(1500);
    }
    CHECK(F(h, (ps3dk_prof_token){0, 0, 0}) == 0, "NONE");
    atomic_store(&g_churn_stop, 1);
    pthread_join(ct, NULL);
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 10000000) == 0, "detach completes: no permanent hold under owner churn");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.retired && st.final_state == 1, "FINALIZED");
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    atomic_store(&g_gpu_stop, 1);
    pthread_join(gt, NULL);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    printf("R2 %s frames ok %d attributed %d refused %d events %llu lost %llu orphaned %llu\n", g_case, frames_ok, frames_attr, frames_refused,
           (unsigned long long)st.events, (unsigned long long)st.lost, (unsigned long long)st.attribution_orphaned);
    CHECK(frames_attr > 10, "real owners were attributed (%d of %d frames)", frames_attr, frames_ok);
    CHECK(take_capture(name) == 0, "capture");
    write_truth(name);
    {
        char extra[64];
        snprintf(extra, sizeof extra, "lossy %d\n", st.lost ? 1 : 0);
        finish_expect(name, st.lost ? 4 : 0, st.events, st.lost, 1, 1, extra);
    }
    return 0;
}

/* ---- SPU + GPU in one capture: the partition; the GPU LOSS budget exhausted (4096 chunks): that domain stops
 * OVERFLOWED while the SPU side keeps its own (unchanged) limits ---- */
static int c_gpu_exhaust(void)
{
    uint32_t h, lim[8], img;
    int stopped = 0;
    ps3dk_prof_gpu_stats st;
    static const ps3dk_prof_spu_desc sd[1] = {{0x100, 7, "spu_zone", "spu/k.c"}};
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01r2", 3, sd, 1, &img) == 0, "an SPU image");
    ps3dk_prof__test_gpu_limits(lim);
    CHECK(lim[0] == 61440 && lim[1] == 245760 && lim[2] == 4032 && lim[3] == 4096, "the partition");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    G.fail_every = 2; /* every other submission refused: one LOSS record per success */
    for (int i = 0; i < 20000 && !stopped; i++) {
        int rc = M(h, &D_A);
        if (rc == PS3DK_PROF_ESTOPPED)
            stopped = i;
        else
            CHECK(rc == 0, "mark %d rc %d", i, rc);
        if (i % 16 == 15)
            CHECK(ps3dk_prof_drain() == 0, "drain");
    }
    G.fail_every = 0;
    CHECK(stopped > 8000, "admission stopped once the budget was exhausted (at %d)", stopped);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 2000000) == 0, "detach");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.final_state == 3 && st.loss_exhausted, "OVERFLOWED");
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    ps3dk_prof__test_gpu_limits(lim);
    CHECK(lim[0] == 61440, "the SPU limits are untouched");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-gpu-exhaust") == 0, "capture");
    write_truth("r2-gpu-exhaust");
    finish_expect("r2-gpu-exhaust", 4, st.events, st.lost, 3, 1, "exhausted 1\nloss-chunks 4096\n");
    return 0;
}

/* ---- install tags: the last available tag is used once; the entry is then RETIRED from installation (never
 * wrapped); with no installable entry gpu_frame is EBUSY and the current owner is unchanged; the tags survive a
 * release + reattach in the same table slot ---- */
static int c_tags(void)
{
    uint32_t h, h2;
    cpu_owner a = {0}, b = {0};
    const uint32_t last = 0x3FFFFFFEu; /* GPU_TAG_MAX - 1 */
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    owner_start(&a, 1);
    owner_start(&b, 1);
    CHECK(ps3dk_prof_drain() == 0, "drain: THREADS");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    ps3dk_prof__test_gpu_set_tags(0, last - 1);
    for (int k = 0; k < 8; k++) { /* each switch takes the next entry at the LAST tag (the previous is drained) */
        CHECK(F(h, (k & 1) ? b.tok : a.tok) == 0, "install %d at the last available tag", k);
        CHECK(M(h, &D_A) == 0, "mark %d", k);
        CHECK(ps3dk_prof_drain() == 0, "drain");
    }
    for (unsigned j = 0; j < 8; j++)
        CHECK(ps3dk_prof__test_gpu_tag(0, j) == last, "entry %u at the last tag", j);
    CHECK(ps3dk_prof_gpu_frame(h, a.tok) == PS3DK_PROF_EBUSY, "every entry retired: EBUSY (the owner was b)");
    CHECK(M(h, &D_B) == 0, "a mark still names the unchanged current owner");
    g_cur_owner = b.tok.owner;
    g_cur_frame = b.tok.frame;
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "detach + release");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h2) == 0 && h2 == 1027, "reattach in the same table slot");
    CHECK(ps3dk_prof_gpu_frame(h2, a.tok) == PS3DK_PROF_EBUSY, "the retired tags survive the slot reuse: EBUSY");
    CHECK(ps3dk_prof_gpu_frame(h2, (ps3dk_prof_token){0, 0, 0}) == 0, "NONE needs no entry");
    CHECK(ps3dk_prof_gpu_detach(h2, CTX, 1000000) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "h2");
    owner_retire(&a);
    owner_retire(&b);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-tags") == 0, "capture");
    write_truth("r2-tags");
    finish_expect("r2-tags", 0, 9, 0, 1, 1, "domains 2\n");
    return 0;
}

/* ---- the lease: with the GPU paused, every owner entry stays leased by its undrained tickets; the ninth switch is
 * EBUSY (the current owner unchanged); after the GPU runs, every event carries its exact owner ---- */
static int c_lease(void)
{
    uint32_t h;
    cpu_owner a = {0}, b = {0};
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    owner_start(&a, 1);
    owner_start(&b, 1);
    CHECK(ps3dk_prof_drain() == 0, "drain: THREADS");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    atomic_store(&G.auto_exec, 0);
    for (int k = 0; k < 8; k++) {
        CHECK(F(h, (k & 1) ? b.tok : a.tok) == 0, "switch %d", k);
        CHECK(M(h, &D_A) == 0, "mark %d", k);
    }
    CHECK(ps3dk_prof_gpu_frame(h, a.tok) == PS3DK_PROF_EBUSY, "all 8 entries leased by undrained tickets: EBUSY");
    CHECK(M(h, &D_B) == 0, "a mark names the unchanged current owner (b)");
    dev_step(1u << 30);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(F(h, a.tok) == 0, "drained: an entry is reclaimable again");
    CHECK(M(h, &D_C) == 0, "mark");
    dev_step(1u << 30);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "detach + release");
    owner_retire(&a);
    owner_retire(&b);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-lease") == 0, "capture");
    write_truth("r2-lease");
    finish_expect("r2-lease", 0, 10, 0, 1, 1, NULL);
    return 0;
}

/* ---- faults at the new lock sites ---- */
static int c_fault(const char *which)
{
    uint32_t h = 0, h2 = 0, rel = 9;
    int rc;
    ps3dk_prof_gpu_stats st;
    char name[64];
    snprintf(name, sizeof name, "r2-fault-%s", which);
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 2, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    if (!strcmp(which, "attach-lock")) {
        ps3dk_prof_test_hooks.spu_lock_fail_at = 1;
        ps3dk_prof_test_hooks.spu_ops = 0;
        CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h) == PS3DK_PROF_ESTOPPED && h == 0, "attach: nothing attached");
    } else if (!strcmp(which, "attach-unlock")) {
        ps3dk_prof_test_hooks.spu_unlock_fail_site = 4; /* U_GPU_ATTACH */
        CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h) == PS3DK_PROF_ESTOPPED && h == 1026,
              "attach: ESTOPPED with the diagnostic handle");
        CHECK(ps3dk_prof__test_gpu_state(0) == 2, "the slot is QUARANTINED");
        CHECK(ps3dk_prof_gpu_attach(CTX2, 32, 16, 65, &h2) == PS3DK_PROF_ESTOPPED, "a later attach: ESTOPPED");
    } else {
        CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h) == 0, "attach");
        CHECK(M(h, &D_A) == 0, "mark");
        CHECK(ps3dk_prof_drain() == 0, "drain");
        if (!strcmp(which, "detach-unlock")) {
            /* the DRAINER finalizes the detach (here: the designated thread's inline drain); its closing unlock fails */
            ps3dk_prof_test_hooks.spu_unlock_fail_site = 1; /* U_STEP3B */
            CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == PS3DK_PROF_ESTOPPED, "detach: ESTOPPED");
            CHECK(ps3dk_prof_gpu_release(h, &rel) == PS3DK_PROF_ESTOPPED && rel == 0, "release: retained");
        } else if (!strcmp(which, "release-unlock")) {
            CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == 0, "detach");
            ps3dk_prof_test_hooks.spu_unlock_fail_site = 6; /* U_GPU_RELEASE */
            CHECK(ps3dk_prof_gpu_release(h, &rel) == PS3DK_PROF_ESTOPPED && rel == 1,
                  "release: ESTOPPED, released=1 (it took effect)");
            CHECK(ps3dk_prof__test_gpu_state(0) == 2, "the slot is QUARANTINED");
            CHECK(ps3dk_prof_gpu_attach(CTX, 0, 16, 64, &h2) == PS3DK_PROF_ESTOPPED, "a later attach: ESTOPPED");
        } else if (!strcmp(which, "snap-lock")) {
            ps3dk_prof_test_hooks.gpu_snap_ops = 0;
            ps3dk_prof_test_hooks.gpu_snap_lock_fail_at = 1;
            CHECK(ps3dk_prof_gpu_mark(h, CTX, &D_A) == PS3DK_PROF_ESTOPPED, "the publication fails: ESTOPPED");
            ps3dk_prof_test_hooks.gpu_snap_lock_fail_at = 0;
            CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.fault && !st.retired, "a domain fault only");
            CHECK(ps3dk_prof_gpu_mark(h, CTX, &D_A) == PS3DK_PROF_ESTOPPED, "no admission");
            CHECK(ps3dk_prof_gpu_detach(h, CTX, 20000) == PS3DK_PROF_ESTOPPED, "detach: ESTOPPED");
            CHECK(ps3dk_prof_gpu_release(h, &rel) == PS3DK_PROF_EBUSY && rel == 0, "release: EBUSY (never retired)");
        } else if (!strcmp(which, "snap-unlock")) {
            ps3dk_prof_test_hooks.gpu_snap_ops = 0;
            ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1;
            CHECK(ps3dk_prof_gpu_mark(h, CTX, &D_A) == PS3DK_PROF_ESTOPPED, "the seam: ESTOPPED");
            ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 0;
            CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.fault && !st.retired, "a domain fault only");
            CHECK(ps3dk_prof_gpu_release(h, &rel) == PS3DK_PROF_EBUSY && rel == 0, "release: EBUSY");
        } else if (!strcmp(which, "seal-unlock")) {
            ps3dk_prof_test_hooks.spu_unlock_fail_site = 2; /* U_SEAL */
        }
    }
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    (void)h2;
    if (!strncmp(which, "snap", 4)) {
        CHECK(rc == 0, "shutdown: the trailer is written (a domain-only fault), got %d", rc);
        CHECK(take_capture(name) == 0, "capture");
        write_truth(name);
        finish_expect(name, 4, 1, 0, 0, 0, NULL);
    } else {
        CHECK(rc == PS3DK_PROF_ESINKFAIL, "shutdown fails closed (an extension fault), got %d", rc);
        CHECK(take_capture(name) == 0, "capture");
        write_truth(name);
        {
            FILE *e = expect(name);
            fprintf(e, "exit 1\nsink_dead 1\n");
            fclose(e);
        }
    }
    return 0;
}

/* ================================================================ codex 14:03 corrections */

/* the callback sink's observations: which threads wrote, which chunk types, a pause, reentry from the callback */
static pthread_t g_ctx_th, g_main_th;
static atomic_int g_cb_ctx_writes, g_cb_other_writes, g_cb_types, g_cb_arm, g_cb_reentry_gpu = 99, g_cb_reentry_spu = 99;
static atomic_int g_cb_hold_type, g_cb_hold_armed, g_cb_holding, g_cb_hold_go;
static uint32_t g_cb_h;
static long cb_write_obs(void *ctx, const void *b, unsigned long n)
{
    const uint8_t *u = b;
    uint32_t type = n >= 4 ? u[0] : 0;
    if (pthread_equal(pthread_self(), g_ctx_th))
        atomic_fetch_add(&g_cb_ctx_writes, 1);
    if (!pthread_equal(pthread_self(), g_main_th))
        atomic_fetch_add(&g_cb_other_writes, 1);
    if (type < 31)
        atomic_fetch_or(&g_cb_types, 1 << type);
    if ((int)type == atomic_load(&g_cb_hold_type) && atomic_exchange(&g_cb_hold_armed, 0)) {
        atomic_store(&g_cb_holding, 1); /* the terminal turn is inside its FINAL write */
        while (!atomic_load(&g_cb_hold_go))
            usleep(200);
        atomic_store(&g_cb_holding, 0);
    }
    if (type == 5 && atomic_exchange(&g_cb_arm, 0)) { /* a v1 RECORDS chunk: pause the encoder, try reentry */
        ps3dk_prof_spu_lane_stats st;
        g_cb_reentry_gpu = ps3dk_prof_gpu_mark(g_cb_h, CTX, &D_A);
        g_cb_reentry_spu = ps3dk_prof_spu_lane_get_stats(1, &st);
        usleep(30000);
    }
    return cb_write(ctx, b, n);
}
static int start_obs(uint32_t mode, ps3dk_prof_ext_config x, const char *name)
{
    char path[512];
    ps3dk_prof_config c = cfg(mode);
    ps3dk_prof_sink_callbacks cb = {cb_write_obs, NULL, cb_close, NULL};
    ps3dk_prof_test_hooks.gpu_port = &g_dev;
    snprintf(path, sizeof path, "%s/%s.prof", g_out, name);
    S.f = fopen(path, "wb");
    if (!S.f || ps3dk_prof_set_sink_callbacks(&cb) != 0)
        return -1;
    return ps3dk_prof_init_ex(&c, &x);
}

/* 1. THREAD mode: the drain thread is encoding v1 RECORDS (paused in the sink) while the ctx thread detaches: the
 * ctx thread never writes; the detach completes through the drainer; sink-callback reentry is refused */
static int c_detach_overlap(void)
{
    uint32_t h;
    g_ctx_th = pthread_self();
    g_main_th = pthread_self(); /* unused here: every write must come from the drain thread */
    atomic_store(&G.auto_exec, 1);
    CHECK(start_obs(PS3DK_PROF_MODE_THREAD, ext(1, 1, 256), "r2-detach-overlap") == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 256, 64, &h) == 0, "attach");
    g_cb_h = h;
    for (int i = 0; i < 300; i++) {
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("cpu");
        PS3DK_PROF_ZONE_END(z);
        CHECK(M(h, &D_A) == 0, "mark %d", i);
        if (i % 50 == 49)
            usleep(3000);
    }
    atomic_store(&g_cb_arm, 1);
    for (int i = 0; i < 50; i++) { /* RECORDS for the drainer to encode while the detach runs */
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("cpu");
        PS3DK_PROF_ZONE_END(z);
    }
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 5000000) == 0, "detach completes through the drain thread");
    CHECK(atomic_load(&g_cb_arm) == 0, "the encoder pause happened");
    CHECK(atomic_load(&g_cb_ctx_writes) == 0, "the ctx thread never wrote (%d writes)", atomic_load(&g_cb_ctx_writes));
    CHECK(g_cb_reentry_gpu == PS3DK_PROF_EBUSY && g_cb_reentry_spu == PS3DK_PROF_EBUSY,
          "GPU and SPU calls from a sink callback: EBUSY (%d %d)", g_cb_reentry_gpu, g_cb_reentry_spu);
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    write_truth("r2-detach-overlap");
    finish_expect("r2-detach-overlap", 0, 300, 0, 1, 1, NULL);
    return 0;
}

/* 1b. APP mode, a NON-designated ctx owner detaches while the designated thread drains: every write is the
 * designated thread's */
static int c_detach_app_helper(void)
{
    static cpu_owner a, b;
    g_main_th = pthread_self();
    g_ctx_th = (pthread_t)0;
    atomic_store(&G.auto_exec, 1);
    CHECK(start_obs(PS3DK_PROF_MODE_APP, ext(0, 1, 64), "r2-detach-app-helper") == 0, "start");
    CHECK(ps3dk_prof_thread_begin("drainer") == 0, "thread_begin");
    H.a = &a;
    H.b = &b;
    pthread_create(&H.th, NULL, helper_main, NULL);
    hcmd(H_ATTACH);
    g_ctx_th = H.th;
    hcmd(H_MARK_OLD);
    hcmd(H_MARK_OLD);
    hcmd_draining(H_DETACH);
    CHECK(g_rc_detach == 0, "the non-designated detach completes through the designated drains (%d)", g_rc_detach);
    CHECK(atomic_load(&g_cb_other_writes) == 0, "only the designated thread wrote (%d other writes)",
          atomic_load(&g_cb_other_writes));
    hcmd(H_RELEASE);
    hcmd(H_QUIT);
    pthread_join(H.th, NULL);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    write_truth("r2-detach-app-helper");
    finish_expect("r2-detach-app-helper", 0, 2, 0, 1, 1, NULL);
    return 0;
}

/* 2. a snap fault on the domain's FIRST publication, before any drain declared it: the domain is still declared
 * (DOMAIN + EPOCH need no snapshot), never finalized, so the reader cannot call the capture complete */
static int c_snap_first(int unlock)
{
    uint32_t h, rel = 9;
    char name[64];
    snprintf(name, sizeof name, "r2-snap-first-%s", unlock ? "unlock" : "lock");
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    ps3dk_prof_test_hooks.gpu_snap_ops = 0;
    if (unlock)
        ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1;
    else
        ps3dk_prof_test_hooks.gpu_snap_lock_fail_at = 1;
    CHECK(ps3dk_prof_gpu_mark(h, CTX, &D_A) == PS3DK_PROF_ESTOPPED, "the first publication fails");
    ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = ps3dk_prof_test_hooks.gpu_snap_lock_fail_at = 0;
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown (a domain-only fault keeps the trailer)");
    CHECK(ps3dk_prof_gpu_release(h, &rel) == PS3DK_PROF_EBUSY && rel == 0, "never retired: borrowed");
    CHECK(take_capture(name) == 0, "capture");
    g_ntr = unlock ? g_ntr : 0; /* the unlock seam numbered (and submitted) the mark before its publication failed */
    write_truth(name);
    finish_expect(name, 4, 0, 0, 0, 0, NULL);
    return 0;
}

/* 3. a failed init AFTER the GPU snap mutexes were initialized destroys each of them exactly once; then a retry */
void ps3dk_prof__test_gpu_mutex_counts(uint32_t *inits, uint32_t *destroys);
static int c_init_unwind(const char *which)
{
    uint32_t i0, d0, i1, d1, i2, d2;
    ps3dk_prof_config c = cfg(!strcmp(which, "thread") ? PS3DK_PROF_MODE_THREAD : PS3DK_PROF_MODE_APP);
    ps3dk_prof_ext_config x = ext(1, 3, 0);
    ps3dk_prof_sink_callbacks cb = {cb_write, NULL, cb_close, NULL};
    int want = !strcmp(which, "clock") ? PS3DK_PROF_ECLOCK : !strcmp(which, "sink") ? PS3DK_PROF_ESINKFAIL
                                                                                     : PS3DK_PROF_ETHREAD;
    ps3dk_prof_test_hooks.gpu_port = &g_dev;
    ps3dk_prof__test_gpu_mutex_counts(&i0, &d0);
    if (!strcmp(which, "thread")) {
        char path[512];
        snprintf(path, sizeof path, "%s/cb.prof", g_out);
        S.f = fopen(path, "wb");
        CHECK(ps3dk_prof_set_sink_callbacks(&cb) == 0, "callbacks");
        ps3dk_prof_test_hooks.thread_create_fail = 1;
    } else {
        CHECK(ps3dk_prof_set_sink_path(g_out) == 0, "sink");
        if (!strcmp(which, "clock"))
            ps3dk_prof_test_hooks.mono_clock_fail = 1;
        else
            ps3dk_prof_test_hooks.open_errno = 13; /* EACCES */
    }
    CHECK(ps3dk_prof_init_ex(&c, &x) == want, "init fails after the GPU mutexes exist");
    ps3dk_prof__test_gpu_mutex_counts(&i1, &d1);
    CHECK(i1 - i0 == 3 && d1 - d0 == 3, "3 initialized, 3 destroyed (got %u, %u)", i1 - i0, d1 - d0);
    ps3dk_prof_test_hooks.thread_create_fail = ps3dk_prof_test_hooks.mono_clock_fail = 0;
    ps3dk_prof_test_hooks.open_errno = 0;
    CHECK(ps3dk_prof_init_ex(&c, &x) == PS3DK_PROF_OK, "retry succeeds");
    ps3dk_prof__test_gpu_mutex_counts(&i2, &d2);
    CHECK(i2 - i1 == 3 && d2 == d1, "the retry initialized 3 more, destroyed none");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}

/* 7a. gpu_frame's thread_mutex: a refused lock, or an unlock that reports failure after unlocking (the runtime's
 * terminal PAL fault, failing the capture closed): ESTOPPED; the core's owner unchanged; no leaked lease */
static int c_frame_thread(int unlock)
{
    uint32_t h;
    cpu_owner a = {0};
    int rc;
    char name[64];
    snprintf(name, sizeof name, "r2-frame-thread-%s", unlock ? "unlock" : "lock");
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    owner_start(&a, 1);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    CHECK(M(h, &D_A) == 0, "mark under NONE");
    ps3dk_prof_test_hooks.p_lock_ops = ps3dk_prof_test_hooks.p_unlock_ops = 0;
    if (unlock)
        ps3dk_prof_test_hooks.p_unlock_fail_at = 1;
    else
        ps3dk_prof_test_hooks.p_lock_fail_at = 1;
    rc = ps3dk_prof_gpu_frame(h, a.tok);
    ps3dk_prof_test_hooks.p_lock_fail_at = ps3dk_prof_test_hooks.p_unlock_fail_at = 0;
    CHECK(rc == PS3DK_PROF_ESTOPPED, "frame: ESTOPPED (got %d)", rc);
    CHECK(ps3dk_prof_gpu_frame(h, a.tok) == PS3DK_PROF_ESTOPPED, "every later frame: ESTOPPED (lock_failed)");
    CHECK(M(h, &D_B) == 0, "a mark still names the unchanged owner (NONE)");
    (void)ps3dk_prof_drain(); /* fails closed (lock_failed): its result is the shutdown's below */
    owner_retire(&a);
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    CHECK(rc == PS3DK_PROF_ESINKFAIL, "the capture fails closed (got %d)", rc);
    CHECK(take_capture(name) == 0, "capture");
    write_truth(name);
    {
        FILE *e = expect(name);
        fprintf(e, "exit 1\nsink_dead 1\n");
        fclose(e);
    }
    return 0;
}

/* 7b. get_stats' closing unlock (U_GPU_STATS) */
static int c_fault_stats(void)
{
    uint32_t h;
    ps3dk_prof_gpu_stats st;
    memset(&st, 0xA5, sizeof st);
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    ps3dk_prof_test_hooks.spu_unlock_fail_site = 7; /* U_GPU_STATS */
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == PS3DK_PROF_ESTOPPED && st.events == 0xA5A5A5A5A5A5A5A5ull,
          "stats: ESTOPPED, nothing written");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == PS3DK_PROF_ESTOPPED, "then ESTOPPED (ext_fault)");
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_ESINKFAIL, "fails closed");
    CHECK(take_capture("r2-fault-stats-unlock") == 0, "capture");
    write_truth("r2-fault-stats-unlock");
    {
        FILE *e = expect("r2-fault-stats-unlock");
        fprintf(e, "exit 1\nsink_dead 1\n");
        fclose(e);
    }
    return 0;
}

/* 7c. SPU API reentry from GPU port code that holds the extension mutex (attach's poison writes): EBUSY, never a
 * recursive acquisition */
static int g_wt_rc = 99;
static void spu_from_port(void)
{
    ps3dk_prof_spu_lane_stats st;
    G.reserve_hook = NULL;
    g_wt_rc = ps3dk_prof_spu_lane_get_stats(1, &st);
}
static uint32_t g_attach_hook_calls;
static void d_wtimer_hooked(void *c, uint32_t r, uint64_t v)
{
    if (!g_attach_hook_calls++)
        spu_from_port();
    d_wtimer(c, r, v);
}
static int c_spu_reentry(void)
{
    uint32_t h;
    static prg_port dev;
    dev = g_dev;
    dev.write_timer = d_wtimer_hooked;
    alarm(20);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 1, 64), NULL) == 0, "start");
    ps3dk_prof_test_hooks.gpu_port = &dev;
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    CHECK(g_wt_rc == PS3DK_PROF_EBUSY, "an SPU call from inside attach's port callback: EBUSY (got %d)", g_wt_rc);
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "detach + release");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    alarm(0);
    return 0;
}

/* 5a. a FRAME dropped on the owner's full ring: the token is eligible (current_frame advanced) but the FRAME is not
 * in the file: the GPU events keep the owner/frame; the reader reports them UNRESOLVED, never certified */
static int c_dropped_frame(void)
{
    uint32_t h;
    static cpu_owner a;
    static atomic_int filled;
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    owner_start(&a, 1);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    (void)filled;
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    /* fill the RENDER thread's ring without draining, then begin a frame: the FRAME record is dropped */
    for (int i = 0; i < 2500; i++) { /* 5000 records > the 4096-record ring: full */
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("fill");
        PS3DK_PROF_ZONE_END(z);
    }
    ps3dk_prof_frame();
    {
        ps3dk_prof_token t = ps3dk_prof_frame_token();
        CHECK(t.owner != 0, "an eligible token for the dropped frame");
        ps3dk_prof_gpu_zone gz;
        CHECK(F(h, t) == 0, "gpu_frame accepts it");
        CHECK(B(h, &D_A, &gz) == 0 && E(h, &gz, "gpu_a") == 0, "a zone attributed to it (BEGIN carries the ref)");
    }
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "detach + release");
    owner_retire(&a);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-dropped-frame") == 0, "capture");
    write_truth("r2-dropped-frame");
    finish_expect("r2-dropped-frame", 0, 2, 0, 1, 1, "unresolved_frames 1\n");
    return 0;
}

/* 5b. SPU + GPU with a REAL host SPU producer: lane 1 runs while the GPU LOSS budget is exhausted; lane 2 runs after
 * the GPU domain stopped OVERFLOWED; both lanes complete; per-source truth for both */
static prod g_lanes[2];
static int c_mixed_spu(void)
{
    uint32_t h;
    int stopped = 0;
    prod *pp[2] = {&g_lanes[0], &g_lanes[1]};
    static const ps3dk_prof_spu_desc sd[1] = {{0x100, 7, "spu_zone", "spu/k.c"}};
    uint32_t img;
    FILE *e;
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(2, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01r2", 3, sd, 1, &img) == 0, "image");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    g_lanes[0].events = 300;
    CHECK(ps3dk_prof_spu_lane_create("lane1", img, 0, &g_lanes[0].lane) == 0, "lane 1");
    pthread_create(&g_lanes[0].th, NULL, producer, &g_lanes[0]);
    G.fail_every = 2;
    for (int i = 0; i < 20000 && !stopped; i++) {
        int rc = M(h, &D_A);
        if (rc == PS3DK_PROF_ESTOPPED)
            stopped = i;
        else
            CHECK(rc == 0, "mark %d rc %d", i, rc);
        if (i % 16 == 15)
            CHECK(ps3dk_prof_drain() == 0, "drain");
    }
    G.fail_every = 0;
    CHECK(stopped > 8000, "the GPU domain stopped on its exhausted LOSS budget (at %d)", stopped);
    pthread_join(g_lanes[0].th, NULL);
    CHECK(ps3dk_prof_spu_lane_end(g_lanes[0].lane.handle, 2000000, NULL) == 0, "lane 1 ends");
    g_lanes[1].events = 200; /* SPU work continues after the GPU exhaustion */
    CHECK(ps3dk_prof_spu_lane_create("lane2", img, 0, &g_lanes[1].lane) == 0, "lane 2");
    pthread_create(&g_lanes[1].th, NULL, producer, &g_lanes[1]);
    for (int i = 0; i < 100; i++) {
        CHECK(ps3dk_prof_drain() == 0, "drain");
        usleep(500);
    }
    pthread_join(g_lanes[1].th, NULL);
    CHECK(ps3dk_prof_spu_lane_end(g_lanes[1].lane.handle, 2000000, NULL) == 0, "lane 2 ends");
    ps3dk_prof_gpu_stats gst;
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 2000000) == 0, "GPU detach");
    CHECK(ps3dk_prof_gpu_get_stats(h, &gst) == 0 && gst.final_state == 3 && gst.loss_exhausted, "GPU OVERFLOWED");
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "GPU release");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-mixed-spu") == 0, "capture");
    write_truth("r2-mixed-spu");
    write_spu_truth("r2-mixed-spu", pp, 2);
    {
        char path[512];
        snprintf(path, sizeof path, "%s/r2-mixed-spu.spu-expect", g_out);
        e = fopen(path, "w");
        fprintf(e, "exit 4\nverdict complete\nsources 3\n");
        for (int k = 0; k < 2; k++)
            fprintf(e, "lane %u COMPLETE 1 1 %llu 0\nlosses %u\n", g_lanes[k].lane.handle,
                    (unsigned long long)g_lanes[k].ntruth, g_lanes[k].lane.handle);
        fclose(e);
    }
    finish_expect("r2-mixed-spu", 4, gst.events, gst.lost, 3, 1, "exhausted 1\nloss-chunks 4096\nspu_truth 1\n");
    return 0;
}

/* 4. a corrupt SPU publication with GPU events pending: the sink fails at the PSP detection, before any GPU output */
static int c_mixed_corrupt(void)
{
    uint32_t h, img;
    static const ps3dk_prof_spu_desc sd[1] = {{0x100, 7, "spu_zone", "spu/k.c"}};
    static prod cp;
    int rc;
    g_main_th = pthread_self();
    g_ctx_th = pthread_self();
    atomic_store(&G.auto_exec, 1);
    CHECK(start_obs(PS3DK_PROF_MODE_APP, ext(1, 1, 64), "r2-mixed-corrupt") == 0, "start");
    CHECK(ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01r2", 3, sd, 1, &img) == 0, "image");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    CHECK(ps3dk_prof_drain() == 0, "drain: declarations");
    cp.events = 20;
    CHECK(ps3dk_prof_spu_lane_create("k", img, 0, &cp.lane) == 0, "lane");
    producer(&cp);
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(M(h, &D_A) == 0 && M(h, &D_B) == 0, "two GPU marks, executed, pending");
    ((uint8_t *)(uintptr_t)cp.lane.ea_ring)[6] = 0xEE; /* slot 0's kind byte: not a defined kind */
    atomic_store(&g_cb_types, 0);
    CHECK(ps3dk_prof_drain() == PS3DK_PROF_ESINKFAIL, "the drain reports the sink failure");
    CHECK(!(atomic_load(&g_cb_types) & (1 << 14)), "no GPU_EVENTS reached the sink after the corruption");
    CHECK(ps3dk_prof_spu_lane_end(cp.lane.handle, 2000000, NULL) == 0, "the lane still ends (output-free)");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0,
          "the GPU domain still detaches (output-free)");
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    CHECK(rc == PS3DK_PROF_ESINKFAIL, "shutdown: ESINKFAIL (%d)", rc);
    CHECK(!(atomic_load(&g_cb_types) & (1 << 6)) && !(atomic_load(&g_cb_types) & (1 << 14)), "no TRAILER, no events");
    write_truth("r2-mixed-corrupt");
    {
        FILE *e = expect("r2-mixed-corrupt");
        fprintf(e, "exit 1\nsink_dead 1\n");
        fclose(e);
    }
    return 0;
}

/* 5c. the mixed threaded run (TSan): an SPU producer, the render thread's GPU zones, the drain thread, a detach */
static int c_mixed_tsan(void)
{
    uint32_t h, img;
    pthread_t gt;
    static const ps3dk_prof_spu_desc sd[1] = {{0x100, 7, "spu_zone", "spu/k.c"}};
    static prod sp;
    ps3dk_prof_gpu_stats st;
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(1, 1, 256), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01r2", 3, sd, 1, &img) == 0, "image");
    pthread_create(&gt, NULL, gpu_thread, NULL);
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 256, 64, &h) == 0, "attach");
    sp.events = 600;
    CHECK(ps3dk_prof_spu_lane_create("t", img, 0, &sp.lane) == 0, "lane");
    pthread_create(&sp.th, NULL, producer, &sp);
    for (int i = 0; i < 1500; i++) {
        ps3dk_prof_gpu_zone z;
        CHECK(B(h, &D_A, &z) == 0 && M(h, &D_B) == 0 && E(h, &z, "gpu_a") == 0, "zone %d", i);
        if (i % 10 == 9)
            usleep(1500);
    }
    pthread_join(sp.th, NULL);
    ps3dk_prof_spu_lane_stats lst;
    CHECK(ps3dk_prof_spu_lane_end(sp.lane.handle, 5000000, &lst) == 0, "lane end");
    CHECK(ps3dk_prof_gpu_detach(h, CTX, 10000000) == 0, "detach");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.final_state == 1, "FINALIZED");
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    atomic_store(&g_gpu_stop, 1);
    pthread_join(gt, NULL);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r2-mixed-tsan") == 0, "capture");
    write_truth("r2-mixed-tsan");
    /* the reader's exit covers BOTH sources: a slow (TSan) drain may overflow the SPU ring (counted LOSS) */
    finish_expect("r2-mixed-tsan", st.lost || lst.lost ? 4 : 0, st.events, st.lost, 1, 1, st.lost ? "lossy 1\n" : NULL);
    return 0;
}

/* ================================================================ codex 14:53: detach COMPLETION (THREAD mode) */

/* the drain thread's terminal turn is held inside its FINAL write: `retired` is already set, the turn is not done.
 * held:   a waiter times out (never OK early); released: OK; a repeated wait: OK
 * fail:   the sink refuses FINAL: OK with an output-free retirement (retired 1, final_written 0)
 * unlock: the turn's closing extension unlock fails (the real unlock, then the error) while FINAL is held: ESTOPPED */
static void pause_100ms(void) { usleep(100000); }
static int c_detach_completion(const char *which)
{
    uint32_t h, rel = 9;
    int rc;
    ps3dk_prof_gpu_stats st;
    char name[64];
    snprintf(name, sizeof name, "r2-completion-%s", which);
    g_ctx_th = pthread_self();
    g_main_th = pthread_self();
    atomic_store(&G.auto_exec, 1);
    CHECK(start_obs(PS3DK_PROF_MODE_THREAD, ext(0, 1, 64), name) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    CHECK(M(h, &D_A) == 0 && M(h, &D_B) == 0, "two marks");
    usleep(20000); /* drained */
    if (!strcmp(which, "fail")) {
        atomic_store(&S.fail_type, 16);
        CHECK(ps3dk_prof_gpu_detach(h, CTX, 3000000) == 0, "the sink refused FINAL: OK, output-free retirement");
        CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.retired && !st.final_written, "retired, final_written 0");
        CHECK(ps3dk_prof_gpu_release(h, &rel) == 0 && rel == 1, "release");
    } else {
        atomic_store(&g_cb_hold_type, 16);
        atomic_store(&g_cb_hold_armed, 1);
        rc = ps3dk_prof_gpu_detach(h, CTX, 100000);
        CHECK(atomic_load(&g_cb_holding), "the terminal turn is held inside its FINAL write");
        CHECK(rc == PS3DK_PROF_ETIMEDOUT, "pending: ETIMEDOUT, never OK while FINAL is being written (got %d)", rc);
        if (!strcmp(which, "unlock")) {
            ps3dk_prof_test_hooks.ext_before_failing_unlock = pause_100ms; /* anything published before it shows */
            ps3dk_prof_test_hooks.spu_unlock_fail_site = 1; /* U_STEP3B: this held turn's closing unlock */
        }
        atomic_store(&g_cb_hold_go, 1);
        rc = ps3dk_prof_gpu_detach(h, CTX, 3000000);
        if (!strcmp(which, "unlock")) {
            CHECK(rc == PS3DK_PROF_ESTOPPED, "the turn's closing unlock failed: ESTOPPED (got %d)", rc);
            CHECK(ps3dk_prof_gpu_release(h, &rel) == PS3DK_PROF_ESTOPPED && rel == 0, "retained");
        } else {
            CHECK(rc == PS3DK_PROF_OK, "released: OK (got %d)", rc);
            CHECK(ps3dk_prof_gpu_detach(h, CTX, 1000) == PS3DK_PROF_OK, "a repeated wait: OK");
            CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.retired && st.final_written && st.final_state == 1,
                  "FINALIZED, FINAL accepted");
            CHECK(ps3dk_prof_gpu_release(h, &rel) == 0 && rel == 1, "release");
        }
    }
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    write_truth(name);
    if (!strcmp(which, "held")) {
        CHECK(rc == 0, "shutdown");
        finish_expect(name, 0, 2, 0, 1, 1, NULL);
    } else {
        FILE *e;
        CHECK(rc == PS3DK_PROF_ESINKFAIL, "shutdown fails closed (got %d)", rc);
        e = expect(name);
        fprintf(e, "exit 1\nsink_dead 1\n");
        fclose(e);
    }
    return 0;
}

/* the seal wins against a waiting detach: the GPU never runs, so the detach cannot finalize; shutdown seals */
static int g_sw_rc = 99;
static uint32_t g_sw_h;
static atomic_int g_sw_ready;
static void *sw_owner(void *a)
{
    (void)a;
    if (ps3dk_prof_thread_begin("render") != 0 || ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &g_sw_h) != 0)
        return NULL;
    (void)M(g_sw_h, &D_A); /* never executed */
    ps3dk_prof_thread_end(); /* quiescent for the shutdown; the detach needs no registration */
    atomic_store(&g_sw_ready, 1);
    g_sw_rc = ps3dk_prof_gpu_detach(g_sw_h, CTX, 5000000);
    return NULL;
}
static int c_detach_seal_wins(void)
{
    pthread_t th;
    uint32_t rel = 9;
    atomic_store(&G.auto_exec, 0);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(0, 1, 64), NULL) == 0, "start");
    pthread_create(&th, NULL, sw_owner, NULL);
    while (!atomic_load(&g_sw_ready))
        usleep(200);
    usleep(20000);
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown seals while the detach waits");
    pthread_join(th, NULL);
    CHECK(g_sw_rc == PS3DK_PROF_ESTOPPED, "the seal won: ESTOPPED (got %d)", g_sw_rc);
    (void)rel;
    CHECK(take_capture("r2-detach-seal-wins") == 0, "capture");
    write_truth("r2-detach-seal-wins");
    finish_expect("r2-detach-seal-wins", 4, 0, 1, 2, 1, "loss 0 1 4\n");
    return 0;
}

/* gpu_frame's thread_mutex unlock seam on its REFUSAL paths: an invalid token and every entry leased */
static int c_frame_unlock_refusal(const char *which)
{
    uint32_t h;
    cpu_owner a = {0}, b = {0};
    int rc;
    char name[64];
    snprintf(name, sizeof name, "r2-frame-unlock-%s", which);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(0, 1, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    owner_start(&a, 1);
    owner_start(&b, 1);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h) == 0, "attach");
    atomic_store(&G.auto_exec, 0);
    if (!strcmp(which, "busy"))
        for (int k = 0; k < 8; k++)
            CHECK(F(h, (k & 1) ? b.tok : a.tok) == 0 && M(h, &D_A) == 0, "lease %d", k);
    ps3dk_prof_test_hooks.p_unlock_ops = 0;
    ps3dk_prof_test_hooks.p_unlock_fail_at = 1;
    if (!strcmp(which, "busy"))
        rc = ps3dk_prof_gpu_frame(h, a.tok); /* would be EBUSY */
    else
        rc = ps3dk_prof_gpu_frame(h, (ps3dk_prof_token){77777u, 0u, 0u}); /* would be EINVAL */
    ps3dk_prof_test_hooks.p_unlock_fail_at = 0;
    CHECK(rc == PS3DK_PROF_ESTOPPED, "a failed closing unlock on the refusal path: ESTOPPED (got %d)", rc);
    CHECK(ps3dk_prof_gpu_frame(h, a.tok) == PS3DK_PROF_ESTOPPED, "every later frame: ESTOPPED");
    dev_step(1u << 30);
    owner_retire(&a);
    owner_retire(&b);
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    CHECK(rc == PS3DK_PROF_ESINKFAIL, "the capture fails closed (got %d)", rc);
    CHECK(take_capture(name) == 0, "capture");
    write_truth(name);
    {
        FILE *e = expect(name);
        fprintf(e, "exit 1\nsink_dead 1\n");
        fclose(e);
    }
    return 0;
}

/* ================================================================ codex 16:11: the acknowledgement's lifetime */

/* THREAD mode: the drain thread pauses between its REAL closing unlock and the detach acknowledgement.  Meanwhile:
 * the timed-out waiter's release is EBUSY (the completion is pending); an attach goes into ANOTHER free slot (two
 * slots) or is EFULL (one slot: the pending slot is neither free nor reusable).  Then the ack publishes to the right
 * domain only; release succeeds and the same slot is reattached immediately (a fresh domain, its own completion). */
static atomic_int g_ack_hold, g_ack_holding, g_ack_go;
/* the handshake uses RELAXED atomics on purpose: it must add no happens-before edge of its own, so that TSan judges
 * only the runtime's synchronization (an acquire/release handshake here would order a racy post-unlock scan before
 * the main thread's attach and hide it) */
static void ack_pause(void)
{
    if (atomic_exchange_explicit(&g_ack_hold, 0, memory_order_relaxed)) {
        atomic_store_explicit(&g_ack_holding, 1, memory_order_relaxed);
        while (!atomic_load_explicit(&g_ack_go, memory_order_relaxed))
            usleep(200);
    }
}
static int c_ack_lifetime(int slots)
{
    uint32_t h1, h2 = 0, h3, rel = 9;
    int rc;
    char name[64];
    snprintf(name, sizeof name, "r2-ack-lifetime-%d", slots);
    atomic_store(&G.auto_exec, 1);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(0, (uint32_t)slots, 64), NULL) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h1) == 0, "attach h1");
    CHECK(M(h1, &D_A) == 0 && M(h1, &D_B) == 0, "two marks");
    usleep(20000);
    ps3dk_prof_test_hooks.gpu_before_ack = ack_pause;
    atomic_store_explicit(&g_ack_hold, 1, memory_order_relaxed);
    rc = ps3dk_prof_gpu_detach(h1, CTX, 100000);
    for (int i = 0; i < 2000 && !atomic_load_explicit(&g_ack_holding, memory_order_relaxed); i++)
        usleep(500);
    CHECK(atomic_load_explicit(&g_ack_holding, memory_order_relaxed),
          "the drain thread is paused between its closing unlock and the ack");
    CHECK(rc == PS3DK_PROF_ETIMEDOUT, "pending: the waiter times out (got %d)", rc);
    CHECK(ps3dk_prof_gpu_release(h1, &rel) == PS3DK_PROF_EBUSY && rel == 0,
          "release while the acknowledgement is pending: EBUSY, released 0");
    if (slots == 2)
        CHECK(ps3dk_prof_gpu_attach(CTX2, 128, 64, 65, &h2) == 0, "an attach into the OTHER free slot during the ack");
    else
        CHECK(ps3dk_prof_gpu_attach(CTX2, 128, 64, 65, &h2) == PS3DK_PROF_EFULL, "the pending slot is not reusable");
    atomic_store_explicit(&g_ack_go, 1, memory_order_relaxed);
    CHECK(ps3dk_prof_gpu_detach(h1, CTX, 3000000) == 0, "the ack published: OK");
    CHECK(ps3dk_prof_gpu_release(h1, &rel) == 0 && rel == 1, "release after the ack");
    CHECK(ps3dk_prof_gpu_attach(CTX, 0, 64, 64, &h3) == 0 && h3 != h1, "the same slot reattached at once (a new id)");
    CHECK(ps3dk_prof_gpu_detach(h3, CTX, 3000000) == 0 && ps3dk_prof_gpu_release(h3, NULL) == 0,
          "the reattached domain's own completion");
    if (slots == 2)
        CHECK(ps3dk_prof_gpu_detach(h2, CTX2, 3000000) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "h2");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture(name) == 0, "capture");
    write_truth(name);
    finish_expect(name, 0, 2, 0, 1, 1, slots == 2 ? "domains 3\n" : "domains 2\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    g_case = argv[1];
    g_out = argv[2];
    if (!strncmp(g_case, "cfg-reject-", 11))
        c_cfg_reject(g_case + 11);
    else if (!strncmp(g_case, "cfg-combo-", 10))
        c_cfg_combo(g_case + 10);
    else if (!strncmp(g_case, "cfg-cap-", 8))
        c_cfg_cap(g_case + 8, argc > 3 ? argv[3] : "0");
    else if (!strcmp(g_case, "cfg-init-fail"))
        c_cfg_init_fail();
    else if (!strcmp(g_case, "api-refusals"))
        c_api_refusals();
    else if (!strcmp(g_case, "lifetime"))
        c_lifetime();
    else if (!strcmp(g_case, "steady"))
        c_steady();
    else if (!strcmp(g_case, "two-owner"))
        c_two_owner();
    else if (!strcmp(g_case, "desc-hold"))
        c_desc_hold();
    else if (!strcmp(g_case, "orphan"))
        c_orphan();
    else if (!strcmp(g_case, "sink-fail-descriptors"))
        c_sink_fail(3, "r2-sink-fail-descriptors");
    else if (!strcmp(g_case, "sink-fail-events"))
        c_sink_fail(14, "r2-sink-fail-events");
    else if (!strcmp(g_case, "sink-fail-final"))
        c_sink_fail(16, "r2-sink-fail-final");
    else if (!strcmp(g_case, "detach-timeout-seal"))
        c_detach_timeout_then_seal();
    else if (!strcmp(g_case, "sched-stale"))
        c_sched_stale();
    else if (!strncmp(g_case, "sched-shutdown-", 15))
        c_sched_shutdown(g_case + 15);
    else if (!strcmp(g_case, "detach-overlap"))
        c_detach_overlap();
    else if (!strcmp(g_case, "detach-app-helper"))
        c_detach_app_helper();
    else if (!strcmp(g_case, "snap-first-lock"))
        c_snap_first(0);
    else if (!strcmp(g_case, "snap-first-unlock"))
        c_snap_first(1);
    else if (!strncmp(g_case, "init-unwind-", 12))
        c_init_unwind(g_case + 12);
    else if (!strcmp(g_case, "frame-thread-lock"))
        c_frame_thread(0);
    else if (!strcmp(g_case, "frame-thread-unlock"))
        c_frame_thread(1);
    else if (!strcmp(g_case, "fault-stats-unlock"))
        c_fault_stats();
    else if (!strcmp(g_case, "spu-reentry"))
        c_spu_reentry();
    else if (!strcmp(g_case, "dropped-frame"))
        c_dropped_frame();
    else if (!strcmp(g_case, "mixed-spu"))
        c_mixed_spu();
    else if (!strcmp(g_case, "mixed-corrupt"))
        c_mixed_corrupt();
    else if (!strcmp(g_case, "mixed-tsan"))
        c_mixed_tsan();
    else if (!strncmp(g_case, "completion-", 11))
        c_detach_completion(g_case + 11);
    else if (!strcmp(g_case, "detach-seal-wins"))
        c_detach_seal_wins();
    else if (!strncmp(g_case, "frame-unlock-", 13))
        c_frame_unlock_refusal(g_case + 13);
    else if (!strcmp(g_case, "ack-lifetime-1"))
        c_ack_lifetime(1);
    else if (!strcmp(g_case, "ack-lifetime-2"))
        c_ack_lifetime(2);
    else if (!strcmp(g_case, "crash-control")) /* a harness-failure control: a crash must never count as a kill */
        abort();
    else if (!strcmp(g_case, "lease"))
        c_lease();
    else if (!strcmp(g_case, "tags"))
        c_tags();
    else if (!strcmp(g_case, "gpu-exhaust"))
        c_gpu_exhaust();
    else if (!strcmp(g_case, "detach-inline"))
        c_detach_inline();
    else if (!strcmp(g_case, "tsan-run"))
        c_tsan(0);
    else if (!strcmp(g_case, "tsan-nolock"))
        c_tsan(1);
    else if (!strncmp(g_case, "fault-", 6))
        c_fault(g_case + 6);
    else {
        printf("R2 %s FAIL unknown case\n", g_case);
        return 2;
    }
    printf("R2 %s %s\n", g_case, g_bad ? "FAIL" : "PASS");
    return g_bad ? 1 : 0;
}
