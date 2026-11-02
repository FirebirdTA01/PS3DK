/* P3.2 integration slice I2: the lane API, step 3b, the seal and lane lifetime inside the real runtime (host PAL),
 * with real host threads as SPU producers.  The producers run the committed psp_spu core through a host port over
 * the HOST EMULATION of the line premises (psp_host_emul.h): this is host-model evidence, not target evidence.
 * One case per process (init is one-shot).  Each case prints "I2 <case> PASS" or "I2 <case> FAIL ..."; captures
 * go to <out-dir>, with .truth and .expect files for the checker where the case states an exact expectation.
 *
 * usage: i2 <case> <out-dir> */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "prof_spu.h"
#include "prof_test_hooks.h"
#include "psp_host_emul.h"
#include "psp_spu.h"

unsigned ps3dk_prof__test_decl_acc_valid(unsigned slot);

/* ---- allocation instrumentation (as I1) ---- */
static atomic_int g_counting;
static atomic_long g_allocs;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
int __real_posix_memalign(void **, size_t, size_t);
void *__real_aligned_alloc(size_t, size_t);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t a, size_t b);
void *__wrap_realloc(void *p, size_t n);
void __wrap_free(void *p);
int __wrap_posix_memalign(void **m, size_t a, size_t n);
void *__wrap_aligned_alloc(size_t a, size_t n);
static void note(void)
{
    if (atomic_load(&g_counting))
        atomic_fetch_add(&g_allocs, 1);
}
void *__wrap_malloc(size_t n) { note(); return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { note(); return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { note(); return __real_realloc(p, n); }
void __wrap_free(void *p) { note(); __real_free(p); }
int __wrap_posix_memalign(void **m, size_t a, size_t n) { note(); return __real_posix_memalign(m, a, n); }
void *__wrap_aligned_alloc(size_t a, size_t n) { note(); return __real_aligned_alloc(a, n); }

static const char *g_case, *g_out;
static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("I2 %s FAIL line %d: ", g_case, __LINE__);                                                         \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

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

/* ---- capture / truth / expectation files ---- */
static void write_truth(const char *name, prod **ps, int n)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.truth", g_out, name);
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

/* the single capture file the runtime wrote into g_out, renamed to <name>.prof */
static int take_capture(const char *name)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "cd '%s' && f=$(ls i2-*.prof 2>/dev/null | head -1) && [ -n \"$f\" ] && mv \"$f\" '%s.prof'",
             g_out, name);
    return system(cmd) == 0 ? 0 : -1;
}

static FILE *expect(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.expect", g_out, name);
    return fopen(path, "w");
}

static ps3dk_prof_config cfg(uint32_t mode)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = mode;
    c.app = "i2";
    c.build_id = "i2";
    return c;
}

static ps3dk_prof_ext_config ext(uint32_t lanes, uint32_t image_bytes)
{
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.spu_lanes_max = lanes;
    x.spu_ring_slots = 1024;
    x.spu_image_bytes_max = image_bytes;
    return x;
}

static const ps3dk_prof_spu_desc g_desc[1] = {{0x100, 7, "spu_zone", "spu/k.c"}};
static uint32_t g_image;

static int start(uint32_t mode, uint32_t lanes)
{
    ps3dk_prof_config c = cfg(mode);
    ps3dk_prof_ext_config x = ext(lanes, 0);
    int rc;
    if (ps3dk_prof_set_sink_path(g_out) != 0)
        return -1;
    rc = ps3dk_prof_init_ex(&c, &x);
    if (rc != PS3DK_PROF_OK)
        return rc;
    return ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01i2", 3, g_desc, 1, &g_image);
}

/* ---- cases ---- */

/* run n producers to completion with the runtime in mode; owner_self for the first lane; lane_end each */
static int basic(uint32_t mode, int n, const char *name)
{
    static prod ps[4];
    prod *pp[4];
    FILE *e;
    CHECK(start(mode, 4) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("main") == 0, "thread_begin");
    for (int k = 0; k < n; k++) {
        pp[k] = &ps[k];
        ps[k].events = 300 + 50 * k; /* at most 800 events: within one 1024-slot ring even with no drain */
        CHECK(ps3dk_prof_spu_lane_create("lane", g_image, k == 0, &ps[k].lane) == 0, "create %d", k);
        pthread_create(&ps[k].th, NULL, producer, &ps[k]);
    }
    if (mode == PS3DK_PROF_MODE_APP)
        for (int i = 0; i < 400; i++) {
            ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("ppu");
            PS3DK_PROF_ZONE_END(z);
            CHECK(ps3dk_prof_drain() == 0, "drain");
            usleep(500);
        }
    for (int k = 0; k < n; k++) {
        ps3dk_prof_spu_lane_stats st;
        pthread_join(ps[k].th, NULL); /* the join precondition */
        CHECK(ps3dk_prof_spu_lane_end(ps[k].lane.handle, 2000000, &st) == 0, "end %d", k);
        CHECK(st.have_post_join && st.post_next_seq == ps[k].s.seq, "post-join next %llu vs producer %llu",
              (unsigned long long)st.post_next_seq, (unsigned long long)ps[k].s.seq);
        CHECK(ps3dk_prof_spu_lane_get_stats(ps[k].lane.handle, &st) == PS3DK_PROF_EINVAL, "stale handle");
    }
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture(name) == 0, "capture");
    write_truth(name, pp, n);
    e = expect(name);
    fprintf(e, "exit 0\nverdict complete\nsources %d\n", n);
    for (int k = 0; k < n; k++)
        fprintf(e, "lane %u COMPLETE 1 1 %llu 0\nlosses %u\n", ps[k].lane.handle, (unsigned long long)ps[k].ntruth,
                ps[k].lane.handle);
    fclose(e);
    return 0;
}

/* a producer still running at shutdown: sealed ABANDONED at its last snapshot; lane_end after the join: ESTOPPED */
static int seal_live(void)
{
    static prod p;
    prod *pp[1] = {&p};
    FILE *e;
    ps3dk_prof_spu_lane_stats st;
    CHECK(start(PS3DK_PROF_MODE_THREAD, 2) == 0, "start");
    p.events = -1;
    p.stop_flag = 1;
    CHECK(ps3dk_prof_spu_lane_create("live", g_image, 0, &p.lane) == 0, "create");
    pthread_create(&p.th, NULL, producer, &p);
    usleep(50000);
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    atomic_store(&p.stop, 1);
    pthread_join(p.th, NULL);
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 1000, &st) == PS3DK_PROF_ESTOPPED, "end after the seal");
    CHECK(take_capture("i2-seal-live") == 0, "capture");
    write_truth("i2-seal-live", pp, 1);
    e = expect("i2-seal-live");
    fprintf(e, "exit 4\nverdict complete\nsources 1\ntruth %u\n", p.lane.handle);
    fclose(e);
    return 0;
}

/* sink callbacks for the reentry, held-sink and failure cases */
static atomic_int g_cb_mode, g_cb_writes, g_reentry_done, g_held_started, g_failed;
static atomic_long g_release_us;
static unsigned char *g_cbuf;
static size_t g_clen;
static ps3dk_prof_spu_lane g_lane_for_cb;
static int g_reentry_rc[3][4];
static void reentry_probe(int which)
{
    ps3dk_prof_spu_lane sentinel;
    ps3dk_prof_spu_lane_stats st;
    uint32_t img = 77;
    memset(&sentinel, 0xAB, sizeof sentinel);
    memset(&st, 0xCD, sizeof st);
    g_reentry_rc[which][0] = ps3dk_prof_spu_lane_create("x", g_image, 0, &sentinel);
    g_reentry_rc[which][1] = ps3dk_prof_spu_lane_get_stats(g_lane_for_cb.handle, &st);
    g_reentry_rc[which][2] = ps3dk_prof_spu_lane_end(g_lane_for_cb.handle, 0, &st);
    g_reentry_rc[which][3] = ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01x", 2, g_desc, 1, &img);
    CHECK(((unsigned char *)&sentinel)[0] == 0xAB && img == 77 && ((unsigned char *)&st)[0] == 0xCD,
          "callback %d: outputs were written", which);
}
static atomic_uint g_types_ok; /* the chunk types the callback sink accepted (after the magic) */
static long cb_write(void *ctx, const void *b, unsigned long n)
{
    (void)ctx;
    int w = atomic_fetch_add(&g_cb_writes, 1);
    if (atomic_load(&g_cb_mode) == 1 && w == 6 && !atomic_load(&g_reentry_done)) {
        reentry_probe(0);
        atomic_store(&g_reentry_done, 1);
    }
    uint32_t type = n >= 4 ? ((const uint8_t *)b)[0] | ((const uint8_t *)b)[1] << 8 : 0;
    if (atomic_load(&g_cb_mode) == 2 && type >= 8 && type <= 16 && !atomic_load(&g_held_started)) {
        /* held: block 300 ms inside the first EXTENSION chunk write (step 3b holds spu_mutex across it) */
        atomic_store(&g_held_started, 1);
        usleep(300000);
        atomic_store(&g_release_us, (long)now_us());
    }
    if ((atomic_load(&g_cb_mode) == 3 && w >= 9) || (atomic_load(&g_cb_mode) == 4 && (type == 4 || g_failed))) {
        atomic_store(&g_failed, 1);
        return -1; /* the sink fails (mode 4: at the THREADS chunk, then stays failed) */
    }
    g_cbuf = realloc(g_cbuf, g_clen + n);
    memcpy(g_cbuf + g_clen, b, n);
    g_clen += n;
    if (w > 0 && type < 32)
        atomic_fetch_or(&g_types_ok, 1u << type);
    return (long)n;
}
static int cb_flush(void *ctx)
{
    (void)ctx;
    if (atomic_load(&g_cb_mode) == 1)
        reentry_probe(1);
    return 0;
}
static int cb_close(void *ctx)
{
    (void)ctx;
    if (atomic_load(&g_cb_mode) == 1)
        reentry_probe(2);
    return 0;
}
static int start_cb(uint32_t mode)
{
    ps3dk_prof_config c = cfg(mode);
    ps3dk_prof_ext_config x = ext(2, 0);
    ps3dk_prof_sink_callbacks cb = {cb_write, cb_flush, cb_close, NULL};
    if (ps3dk_prof_set_sink_callbacks(&cb) != 0 || ps3dk_prof_init_ex(&c, &x) != 0)
        return -1;
    return ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01i2", 3, g_desc, 1, &g_image);
}

static int reentry(void)
{
    static prod p;
    atomic_store(&g_cb_mode, 1);
    CHECK(start_cb(PS3DK_PROF_MODE_APP) == 0, "start");
    p.events = 20;
    CHECK(ps3dk_prof_spu_lane_create("l", g_image, 0, &p.lane) == 0, "create");
    g_lane_for_cb = p.lane;
    producer(&p);
    for (int i = 0; i < 4; i++)
        CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 1000000, NULL) == 0, "end after the callbacks");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    for (int w = 0; w < 3; w++)
        for (int k = 0; k < 4; k++)
            CHECK(g_reentry_rc[w][k] == PS3DK_PROF_EBUSY, "callback %d call %d rc %d", w, k, g_reentry_rc[w][k]);
    CHECK(atomic_load(&g_reentry_done), "the write callback probe ran");
    return 0;
}

/* APP mode, lane_end from a thread that is not the designated drainer: it never drains, only polls */
static atomic_int g_other_rc0, g_other_rc20;
static atomic_long g_other_t20;
static ps3dk_prof_spu_lane g_other_lane;
static void *other_end(void *arg)
{
    uint64_t t0;
    (void)arg;
    atomic_store(&g_other_rc0, ps3dk_prof_spu_lane_end(g_other_lane.handle, 0, NULL));
    t0 = now_us();
    atomic_store(&g_other_rc20, ps3dk_prof_spu_lane_end(g_other_lane.handle, 20000, NULL));
    atomic_store(&g_other_t20, (long)(now_us() - t0));
    return NULL;
}
static int app_other_thread(void)
{
    static prod p;
    pthread_t th;
    CHECK(start(PS3DK_PROF_MODE_APP, 2) == 0, "start");
    p.events = 10;
    CHECK(ps3dk_prof_spu_lane_create("o", g_image, 0, &p.lane) == 0, "create");
    producer(&p);
    g_other_lane = p.lane;
    pthread_create(&th, NULL, other_end, NULL);
    pthread_join(th, NULL);
    CHECK(atomic_load(&g_other_rc0) == PS3DK_PROF_ETIMEDOUT, "timeout 0: %d", atomic_load(&g_other_rc0));
    CHECK(atomic_load(&g_other_rc20) == PS3DK_PROF_ETIMEDOUT && atomic_load(&g_other_t20) >= 20000,
          "timeout 20 ms: rc %d after %ld us", atomic_load(&g_other_rc20), atomic_load(&g_other_t20));
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 1000000, NULL) == 0, "the designated thread drains and ends it");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}

/* THREAD mode, a sink write held 300 ms: lane_end(10 ms) from another thread returns only after the release */
static atomic_long g_ret_us;
static atomic_int g_held_rc;
static void *held_end(void *arg)
{
    ps3dk_prof_spu_lane *l = arg;
    atomic_store(&g_held_rc, ps3dk_prof_spu_lane_end(l->handle, 10000, NULL));
    atomic_store(&g_ret_us, (long)now_us());
    return NULL;
}
static int held_sink(void)
{
    static prod p;
    pthread_t th;
    atomic_store(&g_cb_mode, 2);
    CHECK(start_cb(PS3DK_PROF_MODE_THREAD) == 0, "start");
    p.events = 400;
    CHECK(ps3dk_prof_spu_lane_create("h", g_image, 0, &p.lane) == 0, "create");
    producer(&p);
    while (!atomic_load(&g_held_started))
        usleep(100);
    pthread_create(&th, NULL, held_end, &p.lane);
    pthread_join(th, NULL);
    CHECK(atomic_load(&g_release_us) != 0, "the sink was held");
    printf("HELD release %ld return %ld rc %d\n", atomic_load(&g_release_us), atomic_load(&g_ret_us),
           atomic_load(&g_held_rc));
    /* ordering only: the timeout did not bound the wait for the mutex held across the sink write */
    CHECK(atomic_load(&g_ret_us) >= atomic_load(&g_release_us), "returned before the held sink released");
    while (ps3dk_prof_spu_lane_end(p.lane.handle, 1000000, NULL) == PS3DK_PROF_ETIMEDOUT)
        ;
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}

static int sink_failure(void)
{
    static prod p;
    atomic_store(&g_cb_mode, 3);
    CHECK(start_cb(PS3DK_PROF_MODE_THREAD) == 0, "start");
    p.events = 400;
    CHECK(ps3dk_prof_spu_lane_create("f", g_image, 0, &p.lane) == 0, "create");
    producer(&p);
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 2000000, NULL) == 0, "end: the lifetime progresses after failure");
    {
        ps3dk_prof_spu_lane_stats st;
        CHECK(ps3dk_prof_spu_lane_get_stats(p.lane.handle, &st) == PS3DK_PROF_EINVAL, "stale after the end");
    }
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_ESINKFAIL, "shutdown is ESINKFAIL");
    return 0;
}

static int clock_cases(void)
{
    static prod p;
    CHECK(start(PS3DK_PROF_MODE_APP, 2) == 0, "start");
    p.events = 5;
    CHECK(ps3dk_prof_spu_lane_create("c", g_image, 0, &p.lane) == 0, "create");
    producer(&p);
    ps3dk_prof_test_hooks.mono_clock_fail = 1;
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 1000, NULL) == PS3DK_PROF_ECLOCK, "ECLOCK");
    ps3dk_prof_test_hooks.mono_clock_fail = 0;
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, UINT64_MAX - 1, NULL) == 0, "a saturated deadline");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}

/* mutex faults: (a) the hook unlocks for real, then reports failure */
static int fault(const char *which)
{
    static prod p;
    ps3dk_prof_spu_lane l2;
    ps3dk_prof_spu_lane_stats st;
    uint32_t img = 99;
    int rc;
    CHECK(start(PS3DK_PROF_MODE_APP, 4) == 0, "start");
    p.events = 5;
    CHECK(ps3dk_prof_spu_lane_create("a", g_image, 0, &p.lane) == 0, "create");
    producer(&p);
    ps3dk_prof_test_hooks.spu_ops = 0;
    if (!strcmp(which, "lock")) {
        memset(&l2, 0xAB, sizeof l2);
        ps3dk_prof_test_hooks.spu_lock_fail_at = 1;
        rc = ps3dk_prof_spu_lane_create("b", g_image, 0, &l2);
        CHECK(rc == PS3DK_PROF_EBUSY && ((unsigned char *)&l2)[0] == 0xAB, "lock failure: EBUSY, unchanged (%d)", rc);
        ps3dk_prof_test_hooks.spu_lock_fail_at = 0;
        CHECK(ps3dk_prof_spu_lane_create("b", g_image, 0, &l2) == 0 && l2.handle == p.lane.handle + 1,
              "a later create succeeds with the next id: no lane was made by the failed call");
        CHECK(ps3dk_prof_spu_lane_end(l2.handle, 100000, NULL) == 0, "end");
        CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 100000, NULL) == 0, "end");
        CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
        return 0;
    }
    ps3dk_prof_test_hooks.spu_unlock_fail_at = 1;
    if (!strcmp(which, "unlock-create")) {
        memset(&l2, 0, sizeof l2);
        rc = ps3dk_prof_spu_lane_create("b", g_image, 0, &l2);
        CHECK(rc == PS3DK_PROF_ESTOPPED && l2.handle == p.lane.handle + 1,
              "ESTOPPED with the created lane's handle (diagnostic ownership): rc %d handle %u", rc, l2.handle);
    } else if (!strcmp(which, "unlock-stats")) {
        memset(&st, 0xCD, sizeof st);
        rc = ps3dk_prof_spu_lane_get_stats(p.lane.handle, &st);
        CHECK(rc == PS3DK_PROF_ESTOPPED && ((unsigned char *)&st)[0] == 0xCD,
              "stats: ESTOPPED with no output written (a read that mutated nothing): %d", rc);
    } else if (!strcmp(which, "unlock-image")) {
        rc = ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01y", 2, g_desc, 1, &img);
        CHECK(rc == PS3DK_PROF_ESTOPPED && img == 1, "image: ESTOPPED with the registered index (%d, %u)", rc, img);
    } else { /* unlock-end: the first unlock in lane_end is after the end request */
        rc = ps3dk_prof_spu_lane_end(p.lane.handle, 1000, NULL);
        CHECK(rc == PS3DK_PROF_ESTOPPED, "end: %d", rc);
    }
    ps3dk_prof_test_hooks.spu_unlock_fail_at = 0;
    /* the latch: every later call is ESTOPPED, so a retry can never duplicate or double-release */
    CHECK(ps3dk_prof_spu_lane_create("c", g_image, 0, &l2) == PS3DK_PROF_ESTOPPED, "create after the fault");
    CHECK(ps3dk_prof_spu_lane_get_stats(p.lane.handle, &st) == PS3DK_PROF_ESTOPPED, "stats after the fault");
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 0, NULL) == PS3DK_PROF_ESTOPPED, "end after the fault");
    CHECK(ps3dk_prof_drain() != 0, "the drain fails closed after the fault");
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_ESINKFAIL, "shutdown fails closed");
    return 0;
}

static int fault_drainer(void)
{
    static prod p;
    CHECK(start(PS3DK_PROF_MODE_APP, 2) == 0, "start");
    p.events = 5;
    CHECK(ps3dk_prof_spu_lane_create("d", g_image, 0, &p.lane) == 0, "create");
    producer(&p);
    ps3dk_prof_test_hooks.spu_ops = 0;
    ps3dk_prof_test_hooks.spu_lock_fail_at = 1; /* the next spu_mutex lock is step 3b's */
    CHECK(ps3dk_prof_drain() == PS3DK_PROF_ESINKFAIL, "the drain fails closed");
    ps3dk_prof_test_hooks.spu_lock_fail_at = 0;
    {
        ps3dk_prof_spu_lane_stats st;
        CHECK(ps3dk_prof_spu_lane_get_stats(p.lane.handle, &st) == PS3DK_PROF_ESTOPPED, "the extension is stopped");
    }
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_ESINKFAIL, "shutdown fails closed");
    return 0;
}

/* ---- admission races ---- */
static pthread_mutex_t g_rm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rc = PTHREAD_COND_INITIALIZER;
static int g_paused, g_go, g_claimed, g_create_done, g_race;
static void wait_for(int *flag)
{
    pthread_mutex_lock(&g_rm);
    while (!*flag)
        pthread_cond_wait(&g_rc, &g_rm);
    pthread_mutex_unlock(&g_rm);
}
static void set_flag(int *flag)
{
    pthread_mutex_lock(&g_rm);
    *flag = 1;
    pthread_cond_broadcast(&g_rc);
    pthread_mutex_unlock(&g_rm);
}
static void pause_hook(void)
{
    if (g_race == 2) { /* checked, not yet mutated: let the claim happen, then continue */
        set_flag(&g_paused);
        wait_for(&g_go);
    }
}
static void claim_hook(void)
{
    if (g_race == 2) {
        set_flag(&g_go); /* the claim happened: release the paused create */
    } else if (g_race == 3) {
        set_flag(&g_claimed);
        wait_for(&g_create_done); /* hold the shutdown between its claim and the seal */
    }
}
static atomic_int g_race_rc;
static ps3dk_prof_spu_lane g_race_lane;
static void *race_create(void *arg)
{
    (void)arg;
    if (g_race == 3)
        wait_for(&g_claimed);
    atomic_store(&g_race_rc, ps3dk_prof_spu_lane_create("r", g_image, 0, &g_race_lane));
    set_flag(&g_create_done);
    return NULL;
}
static int race(int which)
{
    pthread_t th;
    char name[32];
    FILE *e;
    int want_ok = which <= 2;
    CHECK(start(PS3DK_PROF_MODE_APP, 2) == 0, "start");
    g_race = which; /* after start: its image registration is an admission too */
    ps3dk_prof_test_hooks.spu_admission_pause = pause_hook;
    ps3dk_prof_test_hooks.after_shutdown_claim = claim_hook;
    if (which == 1) {
        atomic_store(&g_race_rc, ps3dk_prof_spu_lane_create("r", g_image, 0, &g_race_lane));
        CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    } else if (which == 2) {
        pthread_create(&th, NULL, race_create, NULL);
        wait_for(&g_paused);           /* the create passed its admission check and holds spu_mutex */
        CHECK(ps3dk_prof_shutdown() == 0, "shutdown"); /* the claim releases it; the seal waits for the mutex */
        pthread_join(th, NULL);
    } else if (which == 3) {
        pthread_create(&th, NULL, race_create, NULL);
        CHECK(ps3dk_prof_shutdown() == 0, "shutdown"); /* claim -> hook waits for the create -> seal */
        pthread_join(th, NULL);
    } else {
        CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
        atomic_store(&g_race_rc, ps3dk_prof_spu_lane_create("r", g_image, 0, &g_race_lane));
    }
    CHECK(atomic_load(&g_race_rc) == (want_ok ? PS3DK_PROF_OK : PS3DK_PROF_ESTOPPED), "race %d: create rc %d", which,
          atomic_load(&g_race_rc));
    snprintf(name, sizeof name, "i2-race-%d", which);
    CHECK(take_capture(name) == 0, "capture");
    e = expect(name);
    /* an admitted lane is sealed (it never started: ABANDONED, no events); a refused one is not in the file */
    fprintf(e, "exit %d\nverdict complete\nsources %d\n", want_ok ? 4 : 0, want_ok ? 1 : 0);
    fclose(e);
    {
        char tp[512];
        snprintf(tp, sizeof tp, "%s/i2-race-%d.truth", g_out, which);
        e = fopen(tp, "w");
    }
    if (want_ok)
        fprintf(e, "#lane %u base 0 count 0 export 0\n", g_race_lane.handle);
    fclose(e);
    return 0;
}

/* ---- the exact owner record: near-wrap, THREADS failure, retirement after binding ---- */

/* scan a capture for chunk types (and THREADS incarnations) */
static void scan(const char *path, int *lanes_entries, int *threads_has_inc2)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    *lanes_entries = 0;
    *threads_has_inc2 = 0;
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return;
    }
    fclose(f);
    for (long o = 16; o + 20 <= n;) {
        uint32_t type = b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24;
        uint32_t len = b[o + 4] | b[o + 5] << 8 | b[o + 6] << 16 | (uint32_t)b[o + 7] << 24;
        const unsigned char *pl = b + o + 16;
        if (type == 12)
            *lanes_entries += (int)(pl[0] | pl[1] << 8);
        if (type == 4) { /* THREADS: u32 count, entries {u32 inc, u32 slot, u64 os, str name} */
            uint32_t cnt = pl[0] | pl[1] << 8, at = 4;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t inc = pl[at] | pl[at + 1] << 8 | pl[at + 2] << 16 | (uint32_t)pl[at + 3] << 24;
                uint16_t nl = (uint16_t)(pl[at + 16] | pl[at + 17] << 8);
                if (inc == 2)
                    *threads_has_inc2 = 1;
                at += 18 + nl;
            }
        }
        o += 16 + len + 4;
    }
    free(b);
}

static int near_wrap(int with_ext)
{
    static prod a, bb;
    char path[512];
    int lanes, inc2;
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.slot_generation_seed_set = 1;
    ps3dk_prof_test_hooks.slot_generation_seed_slot = 0;
    ps3dk_prof_test_hooks.slot_generation_seed = UINT32_MAX - 1; /* the first registration gets UINT32_MAX */
    if (with_ext) {
        CHECK(start(PS3DK_PROF_MODE_APP, 4) == 0, "start");
    } else {
        CHECK(ps3dk_prof_set_sink_path(g_out) == 0 && ps3dk_prof_init(&c) == 0, "start (no extension)");
    }
    CHECK(ps3dk_prof_thread_begin("gmax") == 0, "register at generation UINT32_MAX");
    if (with_ext) {
        a.events = 20;
        CHECK(ps3dk_prof_spu_lane_create("A", g_image, 1, &a.lane) == 0, "lane A (owner generation UINT32_MAX)");
        CHECK(ps3dk_prof_drain() == 0, "drain: THREADS for incarnation 1 accepted, lane A declared");
        producer(&a);
    }
    CHECK(ps3dk_prof_drain() == 0, "drain");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_drain() == 0, "drain: retirement frees slot 0 (decl_gen and decl_acc cleared)");
    CHECK(ps3dk_prof_thread_begin("g0") == 0, "register again into slot 0: generation wraps to 0");
    if (with_ext) {
        bb.events = 5;
        CHECK(ps3dk_prof_spu_lane_create("B", g_image, 1, &bb.lane) == 0, "lane B (owner generation 0)");
        producer(&bb);
    }
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_drain() == 0, "drain");
    if (with_ext)
        CHECK(ps3dk_prof_spu_lane_end(a.lane.handle, 1000000, NULL) == 0, "end A");
    CHECK(ps3dk_prof_shutdown() >= -100, "shutdown (its result is reported, not asserted)");
    snprintf(path, sizeof path, "near-wrap-%s", with_ext ? "ext" : "v1");
    CHECK(take_capture(path) == 0, "capture");
    snprintf(path, sizeof path, "%s/near-wrap-%s.prof", g_out, with_ext ? "ext" : "v1");
    scan(path, &lanes, &inc2);
    printf("NEAR-WRAP %s: SPU_LANES entries %d; THREADS declares incarnation 2 (generation 0): %s\n",
           with_ext ? "ext" : "v1", lanes, inc2 ? "yes" : "NO");
    if (with_ext)
        CHECK(lanes == 1, "only lane A is declared; lane B (owner generation 0, never accepted) stays pending");
    return 0;
}

/* an owner at generation 0 on a slot never used before: the empty record is all zero, so only the explicit valid
 * flag tells them apart (the v1 code does not declare generation 0 either: decl_gen also resets to 0) */
static int gen_zero_fresh_slot(void)
{
    static prod p;
    char path[512];
    int lanes, inc2;
    ps3dk_prof_test_hooks.slot_generation_seed_set = 1;
    ps3dk_prof_test_hooks.slot_generation_seed_slot = 0;
    ps3dk_prof_test_hooks.slot_generation_seed = UINT32_MAX; /* the first registration wraps to generation 0 */
    CHECK(start(PS3DK_PROF_MODE_APP, 2) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("g0") == 0, "register at generation 0 on a fresh slot");
    p.events = 5;
    CHECK(ps3dk_prof_spu_lane_create("Z", g_image, 1, &p.lane) == 0, "lane Z (owner generation 0)");
    producer(&p);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof__test_decl_acc_valid(0) == 0, "no accepted THREADS identity for generation 0");
    CHECK(ps3dk_prof_drain() == 0, "drain");
    ps3dk_prof_shutdown();
    CHECK(take_capture("gen-zero") == 0, "capture");
    snprintf(path, sizeof path, "%s/gen-zero.prof", g_out);
    scan(path, &lanes, &inc2);
    (void)inc2;
    printf("GEN-ZERO: SPU_LANES entries %d\n", lanes);
    CHECK(lanes == 0, "lane Z is never declared: its owner identity was never accepted");
    return 0;
}

static int threads_sink_failure(void)
{
    ps3dk_prof_spu_lane l;
    char path[512];
    int lanes, inc2;
    atomic_store(&g_cb_mode, 0);
    CHECK(start_cb(PS3DK_PROF_MODE_APP) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("t") == 0, "register");
    CHECK(ps3dk_prof_spu_lane_create("owned", g_image, 1, &l) == 0, "create");
    atomic_store(&g_cb_mode, 4); /* the THREADS chunk write fails (and the sink then stays failed) */
    CHECK(ps3dk_prof_drain() == PS3DK_PROF_ESINKFAIL, "the drain's THREADS write fails");
    CHECK(ps3dk_prof__test_decl_acc_valid(0) == 0, "the owner record stays invalid: THREADS was encoded, not accepted");
    CHECK(ps3dk_prof_spu_lane_end(l.handle, 100000, NULL) == 0, "end: retired without output");
    ps3dk_prof_thread_end();
    ps3dk_prof_shutdown();
    snprintf(path, sizeof path, "%s/threads-sink.prof", g_out);
    {
        FILE *f = fopen(path, "wb");
        fwrite(g_cbuf, 1, g_clen, f);
        fclose(f);
    }
    scan(path, &lanes, &inc2);
    CHECK(lanes == 0, "no SPU_LANES after the refused THREADS (%d)", lanes);
    return 0;
}

static int owner_retires_after_bind(void)
{
    static prod p;
    prod *pp[1] = {&p};
    FILE *e;
    CHECK(start(PS3DK_PROF_MODE_APP, 2) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("owner") == 0, "register");
    p.events = 40;
    CHECK(ps3dk_prof_spu_lane_create("bound", g_image, 1, &p.lane) == 0, "create");
    CHECK(prod_init(&p, 0) == PSP_OK, "producer init");
    p.inited = 1;
    CHECK(ps3dk_prof_drain() == 0, "drain: declared and bound");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_drain() == 0, "drain: the owner retires and its slot is freed");
    producer(&p); /* every event comes after the owner retired */
    CHECK(ps3dk_prof_spu_lane_end(p.lane.handle, 1000000, NULL) == 0, "end");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("i2-owner-retired") == 0, "capture");
    write_truth("i2-owner-retired", pp, 1);
    e = expect("i2-owner-retired");
    fprintf(e, "exit 0\nverdict complete\nsources 1\nlane %u COMPLETE 1 1 %llu 0\n", p.lane.handle,
            (unsigned long long)p.ntruth);
    fclose(e);
    return 0;
}

/* the chunk-sequence ceiling: in extension mode the chunk at UINT64_MAX is refused before any sink call */
static int chunk_seq(int with_ext)
{
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    ps3dk_prof_ext_config x = ext(with_ext ? 1 : 0, 0);
    ps3dk_prof_sink_callbacks cb = {cb_write, NULL, cb_close, NULL};
    int rc;
    atomic_store(&g_cb_mode, 0);
    ps3dk_prof_test_hooks.chunk_seq_seed = UINT64_MAX - 2;
    CHECK(ps3dk_prof_set_sink_callbacks(&cb) == 0 && ps3dk_prof_init_ex(&c, with_ext ? &x : NULL) == 0, "init");
    CHECK(ps3dk_prof_thread_begin("t") == 0, "register");
    rc = ps3dk_prof_drain();
    printf("CHUNKSEQ %s: drain rc %d, sink writes %d (the magic + chunks)\n", with_ext ? "ext" : "v1", rc,
           atomic_load(&g_cb_writes));
    if (with_ext)
        CHECK(rc == PS3DK_PROF_ESINKFAIL && atomic_load(&g_cb_writes) == 3,
              "ext: magic + HEADER (seq MAX-2) + CLOCKS (seq MAX-1), then the refusal before the next sink call");
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    if (with_ext)
        CHECK(rc == PS3DK_PROF_ESINKFAIL, "shutdown ESINKFAIL, no trailer");
    return 0;
}

/* the reservation-loss emulation: a PPU store between getllar and putllc survives, and putllc reports LOST */
static int reservation_loss(void)
{
    static prod p;
    uint8_t snap[128] __attribute__((aligned(128)));
    CHECK(start(PS3DK_PROF_MODE_APP, 1) == 0, "start");
    CHECK(ps3dk_prof_spu_lane_create("v", g_image, 0, &p.lane) == 0, "create");
    CHECK(prod_init(&p, 0) == PSP_OK, "init");
    p_getllar(&p, p.lane.ea_ctl, snap);
    (void)p_atomic(&p);
    psp_emul_line_lock(p.lane.ea_ctl); /* a PPU line-A store, exactly as the runtime's adapter performs it */
    psp_be64(at(p.lane.ea_ctl), 0x55);
    psp_emul_version_bump(p.lane.ea_ctl);
    psp_emul_line_unlock(p.lane.ea_ctl);
    p_putllc(&p, p.lane.ea_ctl, snap);
    CHECK(p_atomic(&p) == PSP_AS_PUTLLC_LOST, "putllc reports LOST");
    CHECK(psp_rd64(at(p.lane.ea_ctl)) == 0x55, "the PPU store survived");
    psp_be64(at(p.lane.ea_ctl), 0); /* restore the read index the producer expects */
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}

/* the image arena: exact fit, one byte short (state unchanged), then a smaller image */
static int arena(int exact)
{
    static char names[8][600];
    static ps3dk_prof_spu_desc d[8];
    ps3dk_prof_config c = cfg(PS3DK_PROF_MODE_APP);
    ps3dk_prof_ext_config x = ext(1, 4096);
    uint32_t img = 1234;
    int rc;
    /* need = 8 descriptors (24 bytes each on LP64) + 8 x (name + NUL + empty file NUL) */
    size_t L = (4096 - 8 * sizeof(ps3dk_prof_spu_desc)) / 8 - 2;
    for (int k = 0; k < 8; k++) {
        memset(names[k], 'n', L);
        names[k][L] = 0;
        d[k] = (ps3dk_prof_spu_desc){(uint32_t)k * 16, 1, names[k], ""};
    }
    if (!exact)
        names[7][L] = 'n', names[7][L + 1] = 0; /* one byte more than fits */
    CHECK(ps3dk_prof_set_sink_path(g_out) == 0 && ps3dk_prof_init_ex(&c, &x) == 0, "init");
    rc = ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01a", 2, d, 8, &img);
    if (exact) {
        CHECK(rc == 0 && img == 0, "exact fit accepted (%d)", rc);
    } else {
        CHECK(rc == PS3DK_PROF_ENOMEM && img == 1234, "one byte short: ENOMEM, output unchanged (%d)", rc);
        rc = ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01b", 2, g_desc, 1, &img);
        CHECK(rc == 0 && img == 0, "then a smaller image registers as index 0 (%d, %u)", rc, img);
    }
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    return 0;
}

/* no allocation while lanes are active: from after the producers start until before they are joined */
static int alloc_lanes(int control)
{
    static prod ps[2];
    CHECK(start(PS3DK_PROF_MODE_THREAD, 2) == 0, "start");
    for (int k = 0; k < 2; k++) {
        ps[k].events = 400;
        CHECK(ps3dk_prof_spu_lane_create("m", g_image, 0, &ps[k].lane) == 0, "create");
        CHECK(prod_init(&ps[k], 0) == PSP_OK, "init");
        ps[k].inited = 1;
    }
    atomic_store(&g_counting, 1);
    if (control) { /* each wrapped entry point: malloc, posix_memalign, aligned_alloc, and their frees */
        void *volatile q = malloc(16);
        void *r = NULL;
        free(q);
        CHECK(posix_memalign(&r, 128, 256) == 0, "posix_memalign");
        free(r);
        q = aligned_alloc(128, 256);
        free(q);
    }
    for (int k = 0; k < 2; k++)
        producer(&ps[k]); /* on this thread: no thread creation inside the window */
    for (int k = 0; k < 2; k++)
        CHECK(ps3dk_prof_spu_lane_end(ps[k].lane.handle, 2000000, NULL) == 0, "end");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    atomic_store(&g_counting, 0);
    printf("ALLOCS %s %ld\n", g_case, atomic_load(&g_allocs));
    CHECK(control ? atomic_load(&g_allocs) == 6 : atomic_load(&g_allocs) == 0, "allocations %ld",
          atomic_load(&g_allocs));
    return 0;
}

/* ---- the remaining unlock-failure sites: step 3b, the seal, lane_end's final release ---- */
static int unlock_site(uint32_t site)
{
    static prod p;
    ps3dk_prof_spu_lane l2;
    ps3dk_prof_spu_lane_stats st;
    int rc;
    atomic_store(&g_cb_mode, 0);
    CHECK(start_cb(PS3DK_PROF_MODE_APP) == 0, "start");
    p.events = 20;
    CHECK(ps3dk_prof_thread_begin("main") == 0, "thread_begin"); /* the owner, so lane_end drains inline */
    CHECK(ps3dk_prof_spu_lane_create("u", g_image, 1, &p.lane) == 0, "create");
    producer(&p);
    ps3dk_prof_test_hooks.spu_unlock_fail_site = site;
    if (site == 1) { /* step 3b's unlock, in an ordinary drain */
        CHECK(ps3dk_prof_drain() == PS3DK_PROF_ESINKFAIL, "the drain fails closed");
    } else if (site == 3) { /* lane_end's final release: the release happened, so the output describes it */
        memset(&st, 0xCD, sizeof st);
        rc = ps3dk_prof_spu_lane_end(p.lane.handle, 2000000, &st);
        CHECK(rc == PS3DK_PROF_ESTOPPED, "end: ESTOPPED (%d)", rc);
        CHECK(st.have_post_join && st.post_next_seq == p.s.seq, "the final stats describe the completed release");
    }
    CHECK(ps3dk_prof_test_hooks.spu_unlock_fail_site == (site == 2 ? 2u : 0u), "site %u reached", site);
    if (site != 2) { /* the latch */
        CHECK(ps3dk_prof_spu_lane_create("c", g_image, 0, &l2) == PS3DK_PROF_ESTOPPED, "create after the fault");
        CHECK(ps3dk_prof_spu_lane_get_stats(p.lane.handle, &st) == PS3DK_PROF_ESTOPPED, "stats after the fault");
        CHECK(ps3dk_prof_drain() == PS3DK_PROF_ESINKFAIL, "a later drain fails closed");
    }
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    CHECK(rc == PS3DK_PROF_ESINKFAIL, "shutdown is ESINKFAIL (%d)", rc);
    CHECK(ps3dk_prof_test_hooks.spu_unlock_fail_site == 0, "the site was reached by the end of shutdown");
    CHECK(!(atomic_load(&g_types_ok) & (1u << 6)), "no TRAILER was written");
    return 0;
}

/* ---- PSP-detected corruption fails the RUNTIME sink: a slot and a line in an ordinary drain, a line first seen by
 * the seal ---- */
static prod g_cp;
static void corrupt_line_b(void)
{
    uint64_t lb = g_cp.lane.ea_ctl + PSP_B;
    psp_emul_line_lock(lb); /* a reserved line-B byte: an impossible but generation-consistent version */
    at(lb)[PSP_B_EPOCHS_WRITTEN - PSP_B + 4] = 1;
    psp_emul_line_unlock(lb);
}
static int corrupt_case(int which)
{
    ps3dk_prof_spu_lane_stats st;
    ps3dk_prof_spu_lane l2;
    unsigned w0 = 0;
    int rc;
    atomic_store(&g_cb_mode, 0);
    CHECK(start_cb(PS3DK_PROF_MODE_APP) == 0, "start");
    g_cp.events = 20;
    if (which == 2)
        g_cp.stop_flag = 1; /* no fini: the seal still has a live lane to read */
    CHECK(ps3dk_prof_spu_lane_create("k", g_image, 0, &g_cp.lane) == 0, "create");
    producer(&g_cp);
    if (which == 0)
        at(g_cp.lane.ea_ring)[6] = 0xEE; /* slot 0's kind byte: not a defined kind */
    else if (which == 1)
        corrupt_line_b();
    if (which != 2) {
        CHECK(ps3dk_prof_drain() == PS3DK_PROF_ESINKFAIL, "the drain reports the sink failure");
        w0 = (unsigned)atomic_load(&g_cb_writes);
        /* the output-free lifecycle: the lane still ends, with its post-join counters, and its slot is released */
        CHECK(ps3dk_prof_spu_lane_end(g_cp.lane.handle, 2000000, &st) == 0, "end after the corruption");
        CHECK(st.have_post_join && st.post_next_seq == g_cp.s.seq, "post-join stats");
        CHECK(ps3dk_prof_spu_lane_get_stats(g_cp.lane.handle, &st) == PS3DK_PROF_EINVAL, "stale after the end");
        CHECK(ps3dk_prof_spu_lane_create("k2", g_image, 0, &l2) == 0, "the released slot is reusable");
        CHECK(ps3dk_prof_spu_lane_end(l2.handle, 2000000, NULL) == 0, "end of the unused lane");
    } else {
        ps3dk_prof_test_hooks.spu_before_seal = corrupt_line_b;
    }
    rc = ps3dk_prof_shutdown();
    CHECK(rc == PS3DK_PROF_ESINKFAIL, "shutdown is ESINKFAIL (%d)", rc);
    if (which != 2)
        CHECK((unsigned)atomic_load(&g_cb_writes) == w0, "no sink output after the corruption (%u vs %u)",
              (unsigned)atomic_load(&g_cb_writes), w0);
    CHECK(!(atomic_load(&g_types_ok) & (1u << 6)), "no TRAILER was written");
    CHECK(ps3dk_prof_spu_lane_end(g_cp.lane.handle, 0, NULL) != PS3DK_PROF_OK, "nothing is live after shutdown");
    return 0;
}

/* ---- the emulation's one ownership mapping: each version counter is owned by its line's mutex ---- */
#define COLL_N 200000
static uint64_t g_coll_ea[4];
static void *coll_thread(void *arg)
{
    uint64_t ea = g_coll_ea[(uintptr_t)arg];
    for (int i = 0; i < COLL_N; i++) {
        psp_emul_line_lock(ea);
        psp_emul_version_bump(ea);
        psp_emul_line_unlock(ea);
    }
    return NULL;
}
static int emul_collision(void)
{
    pthread_t th[4];
    uint64_t same = 0, v0[4];
    /* 128 and 94208: distinct lines on different mutex stripes whose versions shared one counter in r1 */
    g_coll_ea[0] = 128;
    g_coll_ea[1] = 94208;
    CHECK(psp_emul_stripe(128) != psp_emul_stripe(94208), "the r1 pair is on different stripes");
    for (uint64_t ea = 256; ea < (1ull << 32) && !same; ea += 128) /* a forced same-stripe collision */
        if (psp_emul_stripe(ea) == psp_emul_stripe(128))
            same = ea;
    CHECK(same != 0, "a same-stripe line exists");
    g_coll_ea[2] = same;
    g_coll_ea[3] = 94208 + 128;
    /* single-threaded: a bump is visible on exactly the lines of the same stripe */
    for (uint64_t ea = 0; ea < 128ull * 4096; ea += 128) {
        uint64_t a, b;
        psp_emul_line_lock(128);
        a = psp_emul_version(ea);
        psp_emul_version_bump(128);
        b = psp_emul_version(ea);
        psp_emul_line_unlock(128);
        if ((a != b) != (psp_emul_stripe(ea) == psp_emul_stripe(128))) {
            CHECK(0, "line %llu: version shared %d, stripe shared %d", (unsigned long long)ea, a != b,
                  psp_emul_stripe(ea) == psp_emul_stripe(128));
            break;
        }
    }
    for (int k = 0; k < 4; k++) {
        psp_emul_line_lock(g_coll_ea[k]);
        v0[k] = psp_emul_version(g_coll_ea[k]);
        psp_emul_line_unlock(g_coll_ea[k]);
    }
    for (int k = 0; k < 4; k++)
        pthread_create(&th[k], NULL, coll_thread, (void *)(uintptr_t)k);
    for (int k = 0; k < 4; k++)
        pthread_join(th[k], NULL);
    for (int k = 0; k < 4; k++) {
        uint64_t want = v0[k], v;
        for (int j = 0; j < 4; j++)
            if (psp_emul_stripe(g_coll_ea[j]) == psp_emul_stripe(g_coll_ea[k]))
                want += COLL_N;
        psp_emul_line_lock(g_coll_ea[k]);
        v = psp_emul_version(g_coll_ea[k]);
        psp_emul_line_unlock(g_coll_ea[k]);
        CHECK(v == want, "line %llu: version %llu, want %llu (no lost bump)", (unsigned long long)g_coll_ea[k],
              (unsigned long long)v, (unsigned long long)want);
    }
    printf("COLLISION stripes %u %u %u %u\n", psp_emul_stripe(g_coll_ea[0]), psp_emul_stripe(g_coll_ea[1]),
           psp_emul_stripe(g_coll_ea[2]), psp_emul_stripe(g_coll_ea[3]));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    g_case = argv[1];
    g_out = argv[2];
    if (!strcmp(g_case, "basic-thread"))
        basic(PS3DK_PROF_MODE_THREAD, 3, "i2-basic-thread");
    else if (!strcmp(g_case, "basic-app"))
        basic(PS3DK_PROF_MODE_APP, 2, "i2-basic-app");
    else if (!strcmp(g_case, "basic-thread-drop-ma")) {
        psp_emul_drop_line_a = 1; /* the TSan negative control: line-A stores without their emulation lock */
        basic(PS3DK_PROF_MODE_THREAD, 2, "i2-basic-drop-ma");
    } else if (!strcmp(g_case, "seal-live"))
        seal_live();
    else if (!strcmp(g_case, "reentry"))
        reentry();
    else if (!strcmp(g_case, "app-other-thread"))
        app_other_thread();
    else if (!strcmp(g_case, "held-sink"))
        held_sink();
    else if (!strcmp(g_case, "sink-failure"))
        sink_failure();
    else if (!strcmp(g_case, "clock"))
        clock_cases();
    else if (!strcmp(g_case, "unlock-step3b"))
        unlock_site(1);
    else if (!strcmp(g_case, "unlock-seal"))
        unlock_site(2);
    else if (!strcmp(g_case, "unlock-end-final"))
        unlock_site(3);
    else if (!strcmp(g_case, "corrupt-slot"))
        corrupt_case(0);
    else if (!strcmp(g_case, "corrupt-line"))
        corrupt_case(1);
    else if (!strcmp(g_case, "corrupt-seal"))
        corrupt_case(2);
    else if (!strcmp(g_case, "emul-collision"))
        emul_collision();
    else if (!strncmp(g_case, "fault-", 6) && strcmp(g_case, "fault-drainer"))
        fault(g_case + 6);
    else if (!strcmp(g_case, "fault-drainer"))
        fault_drainer();
    else if (!strncmp(g_case, "race-", 5))
        race(g_case[5] - '0');
    else if (!strcmp(g_case, "near-wrap-ext"))
        near_wrap(1);
    else if (!strcmp(g_case, "near-wrap-v1"))
        near_wrap(0);
    else if (!strcmp(g_case, "gen-zero-fresh-slot"))
        gen_zero_fresh_slot();
    else if (!strcmp(g_case, "threads-sink-failure"))
        threads_sink_failure();
    else if (!strcmp(g_case, "owner-retired"))
        owner_retires_after_bind();
    else if (!strcmp(g_case, "chunk-seq-ext"))
        chunk_seq(1);
    else if (!strcmp(g_case, "chunk-seq-v1"))
        chunk_seq(0);
    else if (!strcmp(g_case, "reservation-loss"))
        reservation_loss();
    else if (!strcmp(g_case, "arena-exact"))
        arena(1);
    else if (!strcmp(g_case, "arena-short"))
        arena(0);
    else if (!strcmp(g_case, "alloc-lanes"))
        alloc_lanes(0);
    else if (!strcmp(g_case, "alloc-lanes-control"))
        alloc_lanes(1);
    else {
        printf("I2 %s FAIL unknown case\n", g_case);
        return 1;
    }
    if (g_bad)
        return 1;
    printf("I2 %s PASS\n", g_case);
    return 0;
}
