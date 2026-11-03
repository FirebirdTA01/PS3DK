/* P3.3 R4a: the REAL GCM port (gpu_port_gcm.c) inside the REAL runtime (host PAL), over fake GCM memory and a fake
 * command buffer whose words a small executor decodes (ONLY the chosen encoding; anything else is a test failure).
 * Host model, not RSX evidence: the executor is ours, Q1/Q2 stay hardware questions, A1/A2 stay assumptions.
 *
 * Mode S (built WITHOUT GPP_HOST_DEVMEM: the port's real volatile access path): APP mode, the ctx owner IS the
 * designated drainer, and the executor runs inside the fake ctx callback / flush on that same thread.  Every device
 * memory access is on one thread.
 * Mode C (built WITH GPP_HOST_DEVMEM, for TSan): THREAD mode; every timer/label access - the port's (through the
 * gpp_dev seam), the executor's and the test's own setup/checks - is an SC atomic; the callback/flush hand COPIES of
 * the published words to the executor thread through a mutex-protected queue.  R4A_PLAIN_EXEC: the NEGATIVE control
 * (the executor stores plainly: TSan must report).
 *
 * One case per process.  Prints "R4A <case> PASS" or "R4A <case> FAIL ...".  usage: r4a <case> <out-dir> */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "gpu_port_gcm.h"
#include "prg.h"
#include "prof_gpu.h"
#include "prof_spu.h"
#include "prof_test_hooks.h"

uint32_t ps3dk_prof__test_gpu_users(unsigned i);
uint32_t ps3dk_prof__test_gpu_state(unsigned i);
uint32_t ps3dk_prof__test_gpu_gpp_ready(unsigned i);
void ps3dk_prof__test_gpu_ledger_exhaust(int on);

static const char *g_case, *g_out;
static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("R4A %s FAIL line %d: ", g_case, __LINE__);                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---- fake device memory: two banks (A, B), each a report area and a label array ---- */
typedef struct rep_ent {
    uint64_t timer;
    uint32_t value, zero;
} rep_ent;
typedef struct bank {
    rep_ent rep[GPP_REPORTS];
    uint32_t lab[GPP_LABEL_LAST + 1];
} bank;
static bank BK[2] __attribute__((aligned(16)));
#define FILL_T(b, i) (0x5100000000000000ull | ((uint64_t)(b) << 40) | (i))
#define FILL_L(b, i) (0x7700u | ((uint32_t)(b) << 12) | (i))

#ifdef GPP_HOST_DEVMEM
#define DM_LD64(a) __atomic_load_n((a), __ATOMIC_SEQ_CST)
#define DM_ST64(a, v) __atomic_store_n((a), (v), __ATOMIC_SEQ_CST)
#define DM_LD32(a) __atomic_load_n((a), __ATOMIC_SEQ_CST)
#define DM_ST32(a, v) __atomic_store_n((a), (v), __ATOMIC_SEQ_CST)
#ifdef R4A_PLAIN_EXEC
#define EX_ST64(a, v) (*(uint64_t *)(a) = (v)) /* the NEGATIVE control: a plain store racing the port's atomics */
#define EX_ST32(a, v) (*(uint32_t *)(a) = (v))
#else
#define EX_ST64(a, v) DM_ST64(a, v)
#define EX_ST32(a, v) DM_ST32(a, v)
#endif
#else
#define DM_LD64(a) (*(a))
#define DM_ST64(a, v) (*(a) = (v))
#define DM_LD32(a) (*(a))
#define DM_ST32(a, v) (*(a) = (v))
#define EX_ST64(a, v) DM_ST64(a, v)
#define EX_ST32(a, v) DM_ST32(a, v)
#endif

static void mem_fill(int b)
{
    for (uint32_t i = 0; i < GPP_REPORTS; i++) {
        DM_ST64(&BK[b].rep[i].timer, FILL_T(b, i));
        DM_ST32(&BK[b].rep[i].value, 0xAAAA0000u | i);
        DM_ST32(&BK[b].rep[i].zero, 0xBBBB0000u | i);
    }
    for (uint32_t i = 0; i <= GPP_LABEL_LAST; i++)
        DM_ST32(&BK[b].lab[i], FILL_L(b, i));
}
/* 1 when bank b is untouched outside reports [first, first+count) and label `label` (count 0: untouched entirely) */
static int mem_untouched_outside(int b, uint32_t first, uint32_t count, uint32_t label)
{
    for (uint32_t i = 0; i < GPP_REPORTS; i++) {
        if (DM_LD32(&BK[b].rep[i].value) != (0xAAAA0000u | i) || DM_LD32(&BK[b].rep[i].zero) != (0xBBBB0000u | i))
            return 0; /* adjacent fields are never written */
        if ((i < first || i >= first + count) && DM_LD64(&BK[b].rep[i].timer) != FILL_T(b, i))
            return 0;
    }
    for (uint32_t i = 0; i <= GPP_LABEL_LAST; i++)
        if ((count == 0 || i != label) && DM_LD32(&BK[b].lab[i]) != FILL_L(b, i))
            return 0;
    return 1;
}
static int poisoned(int b, uint32_t first, uint32_t count, uint32_t label)
{
    for (uint32_t i = first; i < first + count; i++)
        if (DM_LD64(&BK[b].rep[i].timer) != PRG_POISON)
            return 0;
    return DM_LD32(&BK[b].lab[label]) == 0;
}

/* accessors (two banks), with a failure injection and an optional re-entry probe */
static int64_t g_fail_index = -1;
static int g_fail_label, g_reenter;
static atomic_int g_reenter_rc;
static atomic_uint g_accessor_calls;
static void *rep_at(int b, uint32_t i)
{
    atomic_fetch_add(&g_accessor_calls, 1);
    if (g_reenter) {
        uint32_t h;
        g_reenter = 0;
        atomic_store(&g_reenter_rc, ps3dk_prof_gpu_attach((ps3dk_prof_gcm_ctx *)(void *)&BK[1], 0, 2, 64, &h));
    }
    if ((int64_t)i == g_fail_index || i >= GPP_REPORTS)
        return NULL;
    return &BK[b].rep[i];
}
static void *lab_at(int b, uint32_t i)
{
    atomic_fetch_add(&g_accessor_calls, 1);
    return g_fail_label || i > GPP_LABEL_LAST ? NULL : &BK[b].lab[i];
}
static void *rep_a(uint32_t i) { return rep_at(0, i); }
static void *rep_b(uint32_t i) { return rep_at(1, i); }
static void *lab_a(uint32_t i) { return lab_at(0, i); }
static void *lab_b(uint32_t i) { return lab_at(1, i); }

/* ---- the fake command buffers and the executor ---- */
#define CBUF 600u /* 100 pairs */
typedef struct fctx {
    gpp_ctx c; /* first member: the runtime and the port see &fctx.c */
    int bank;
    uint32_t first, count, label; /* what this context's words may name */
    uint32_t buf[CBUF] __attribute__((aligned(16)));
    int refuse;              /* the callback refuses (FIFO refusal) */
    void (*pause)(void);     /* runs inside the callback before it hands over the words */
    uint32_t *end_after_cb;  /* nonzero: the callback restores end to it (a shrunk buffer's wrap) */
} fctx;
static fctx FX[2];
static atomic_int g_exec_bad;
static atomic_ullong g_exec_pairs;
static uint64_t g_clock = 1000;

static uint32_t unperm(uint32_t v) { return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16); }
/* the executor's own log, per context, in execution order: (label value, the timer it wrote).  The per-domain truth
 * (dtruth) takes each domain's raw timers from it.  g_lm orders the executor's appends with the test's reads. */
typedef struct xlog {
    uint32_t value;
    uint64_t clock;
} xlog;
static pthread_mutex_t g_lm = PTHREAD_MUTEX_INITIALIZER;
static xlog g_xl[2][4096];
static uint32_t g_nxl[2];
/* decode one 6-word pair; only the chosen encoding is accepted */
static void exec_pair(const fctx *f, const uint32_t *w)
{
    uint32_t r = (w[1] & 0x00ffffffu) / 16u, l = w[3] / 16u;
    if (w[0] != 0x00041800u || (w[1] >> 24) != 1u || (w[1] & 15u) || w[2] != 0x00041d6cu || (w[3] & 15u) ||
        w[4] != 0x00041d70u || r < f->first || r >= f->first + f->count || l != f->label) {
        atomic_fetch_add(&g_exec_bad, 1);
        return;
    }
    g_clock += 37; /* executor-private */
    EX_ST64(&BK[f->bank].rep[r].timer, g_clock); /* the report before its label (the A1 order, by construction) */
    EX_ST32(&BK[f->bank].lab[l], unperm(w[5]));
    pthread_mutex_lock(&g_lm);
    if (g_nxl[f - FX] < 4096)
        g_xl[f - FX][g_nxl[f - FX]++] = (xlog){unperm(w[5]), g_clock};
    pthread_mutex_unlock(&g_lm);
    atomic_fetch_add(&g_exec_pairs, 1);
}

#ifdef GPP_HOST_DEVMEM
/* Mode C: the queue (the FIFO publication edge is its mutex) */
typedef struct qrec {
    int ctxi;
    uint32_t w[6];
} qrec;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static qrec g_q[1u << 14];
static uint32_t g_qh, g_qt;
static atomic_int g_exec_stop;
/* completion accounting: a packet is DONE only after its device writes (exec_pair returned); the emulator is
 * quiescent when every submitted packet (g_qh, under g_qm) is done.  Emulator quiescence, not a hardware claim. */
static atomic_uint g_exec_done;
static atomic_int g_xpause, g_xgo; /* 1: the executor pauses AFTER a dequeue, BEFORE its device writes (stage 2) */
static int exec_quiesced(void)
{
    pthread_mutex_lock(&g_qm);
    uint32_t submitted = g_qh;
    pthread_mutex_unlock(&g_qm);
    return atomic_load(&g_exec_done) == submitted;
}
static void handover(fctx *f)
{
    uint32_t n = (uint32_t)(f->c.current - f->c.begin);
    if (n % 6u)
        atomic_fetch_add(&g_exec_bad, 1);
    pthread_mutex_lock(&g_qm);
    for (uint32_t k = 0; k + 6 <= n; k += 6) {
        if (g_qh - g_qt >= (1u << 14))
            atomic_fetch_add(&g_exec_bad, 1);
        g_q[g_qh & ((1u << 14) - 1)].ctxi = (int)(f - FX);
        memcpy(g_q[g_qh & ((1u << 14) - 1)].w, f->c.begin + k, sizeof(uint32_t) * 6); /* an immutable copy */
        g_qh++;
    }
    pthread_mutex_unlock(&g_qm);
    f->c.current = f->c.begin;
}
static void *executor(void *a)
{
    (void)a;
    for (;;) {
        qrec r;
        int have = 0;
        pthread_mutex_lock(&g_qm);
        if (g_qt != g_qh) {
            r = g_q[g_qt & ((1u << 14) - 1)];
            g_qt++;
            have = 1;
        }
        pthread_mutex_unlock(&g_qm);
        if (have) {
            if (atomic_load(&g_xpause) == 1) { /* dequeued, not yet written */
                atomic_store(&g_xpause, 2);
                while (!atomic_load(&g_xgo))
                    usleep(50);
            }
            exec_pair(&FX[r.ctxi], r.w);
            atomic_fetch_add(&g_exec_done, 1); /* after the device writes */
        } else if (atomic_load(&g_exec_stop))
            return NULL;
        else
            usleep(20);
    }
}
static uint64_t sl64(volatile uint64_t *a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }
static void ss64(volatile uint64_t *a, uint64_t v) { __atomic_store_n(a, v, __ATOMIC_SEQ_CST); }
static uint32_t sl32(volatile uint32_t *a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }
static void ss32(volatile uint32_t *a, uint32_t v) { __atomic_store_n(a, v, __ATOMIC_SEQ_CST); }
#define SEAM , sl64, ss64, sl32, ss32
#else
/* Mode S: execute in place on the calling thread */
static void handover(fctx *f)
{
    uint32_t n = (uint32_t)(f->c.current - f->c.begin);
    if (n % 6u)
        atomic_fetch_add(&g_exec_bad, 1);
    for (uint32_t k = 0; k + 6 <= n; k += 6)
        exec_pair(f, f->c.begin + k);
    f->c.current = f->c.begin;
}
#define SEAM
#endif

static int32_t fcb(gpp_ctx *c, uint32_t count)
{
    fctx *f = (fctx *)(void *)c;
    (void)count;
    if (f->pause)
        f->pause();
    if (f->refuse)
        return -1;
    handover(f);
    if (f->end_after_cb) {
        f->c.end = f->end_after_cb;
        f->end_after_cb = NULL;
    }
    return 0;
}
static void fflush_(gpp_ctx *c) { handover((fctx *)(void *)c); } /* the app's / the core's flush */
static const gpp_dev DEV_A = {rep_a, lab_a, fflush_ SEAM};
static const gpp_dev DEV_B __attribute__((unused)) = {rep_b, lab_b, fflush_ SEAM}; /* Mode S cases only */

static void fx_init(int i, int bank, uint32_t first, uint32_t count, uint32_t label)
{
    fctx *f = &FX[i];
    memset(f, 0, sizeof *f);
    f->bank = bank;
    f->first = first;
    f->count = count;
    f->label = label;
    f->c.begin = f->c.current = f->buf;
    f->c.end = f->buf + CBUF;
    f->c.callback = fcb;
}
#define CTXP(i) ((ps3dk_prof_gcm_ctx *)(void *)&FX[i].c)

/* ---- the runtime ---- */
typedef struct tr {
    uint8_t kind, depth;
    const char *desc;
    uint32_t dom;
} tr;
static tr g_tr[100000];
static int g_ntr, g_depth;
static void note(uint8_t kind, const char *desc, uint32_t dom)
{
    if (kind == 2)
        g_depth--;
    g_tr[g_ntr++] = (tr){kind, (uint8_t)g_depth, desc, dom};
    if (kind == 1)
        g_depth++;
}
/* the per-domain truth for a multi-domain capture: <name>.dtruth, for r4a_domains.py.  doms[] in ATTACH order with
 * the context each used; each domain's raw timers are the next entries of that context's executor log (its pairs
 * were all executed before the next domain on the same context attached), whose label values must run 1..n.
 * Returns 0, or -1 when the executor's log does not match the notes (a harness failure). */
typedef struct dspec {
    uint32_t h;
    int ctxi;
} dspec;
static int write_dtruth(const char *name, const dspec *doms, int nd)
{
    char path[512];
    uint32_t cur[2] = {0, 0};
    int rc = 0;
    snprintf(path, sizeof path, "%s/%s.dtruth", g_out, name);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    pthread_mutex_lock(&g_lm);
    for (int k = 0; k < nd; k++) {
        uint32_t n = 0;
        for (int i = 0; i < g_ntr; i++)
            n += g_tr[i].dom == doms[k].h;
        fprintf(f, "domain %u events %u lost 0 final_state 1\n", doms[k].h, n);
        for (int i = 0, s = 0; i < g_ntr; i++) {
            if (g_tr[i].dom != doms[k].h)
                continue;
            const xlog *x = &g_xl[doms[k].ctxi][cur[doms[k].ctxi]++];
            if (cur[doms[k].ctxi] > g_nxl[doms[k].ctxi] || x->value != (uint32_t)s + 1u)
                rc = -1;
            fprintf(f, "ev %u %d %u %u %s %llu\n", doms[k].h, s, g_tr[i].kind, g_tr[i].depth, g_tr[i].desc,
                    (unsigned long long)x->clock);
            s++;
        }
    }
    for (int c = 0; c < 2; c++)
        if (cur[c] != g_nxl[c])
            rc = -1; /* every executed pair belongs to exactly one noted event */
    pthread_mutex_unlock(&g_lm);
    fclose(f);
    return rc;
}
static ps3dk_prof_ext_config ext(uint32_t gpus, uint32_t slots)
{
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.gpu_domains_max = gpus;
    x.gpu_report_slots_max = slots;
    return x;
}
static int start(uint32_t mode, ps3dk_prof_ext_config x, const gpp_dev *gdev)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = mode;
    c.app = "r4a";
    c.build_id = "r4a";
    c.drain_interval_us = 1000;
    c.quiesce_timeout_us = 100000;
    ps3dk_prof_test_hooks.gpu_gpp_dev = gdev;
    if (ps3dk_prof_set_sink_path(g_out) != 0)
        return -1;
    return ps3dk_prof_init_ex(&c, &x);
}
static int take_capture(const char *name)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "cd '%s' && f=$(ls r4a-*.prof 2>/dev/null | head -1) && [ -n \"$f\" ] && mv \"$f\" '%s.prof'", g_out, name);
    return system(cmd) == 0 ? 0 : -1;
}
static void write_truth(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.truth", g_out, name);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < g_ntr; i++)
        fprintf(f, "%d %u %u %s 0 0\n", i, g_tr[i].kind, g_tr[i].depth, g_tr[i].desc);
    fclose(f);
}
static void finish_expect(const char *name, int exit_code, uint64_t events, uint64_t lost, int final_state,
                          const char *extra)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.expect", g_out, name);
    FILE *e = fopen(path, "w");
    fprintf(e, "exit %d\nevents %llu\nlost %llu\nfinal_state %d\nfinal_present 1\n%s", exit_code,
            (unsigned long long)events, (unsigned long long)lost, final_state, extra ? extra : "");
    fclose(e);
}
static ps3dk_prof_desc D_A = {"gpu_a", "", "r4a_int.c", 1, 2, 0, 0}, D_B = {"gpu_b", "", "r4a_int.c", 2, 2, 0, 0};
static int B(uint32_t h, int ci, ps3dk_prof_gpu_zone *z)
{
    int rc = ps3dk_prof_gpu_begin(h, CTXP(ci), &D_A, z);
    if (rc == PS3DK_PROF_OK)
        note(1, "gpu_a", h);
    return rc;
}
static int E(uint32_t h, int ci, ps3dk_prof_gpu_zone *z)
{
    int rc = ps3dk_prof_gpu_end(h, CTXP(ci), z);
    if (rc == PS3DK_PROF_OK)
        note(2, "gpu_a", h);
    return rc;
}
static int M(uint32_t h, int ci)
{
    int rc = ps3dk_prof_gpu_mark(h, CTXP(ci), &D_B);
    if (rc == PS3DK_PROF_OK)
        note(3, "gpu_b", h);
    return rc;
}
static int frames(uint32_t h, int ci, int n, int drain)
{
    for (int i = 0; i < n; i++) {
        ps3dk_prof_gpu_zone z;
        CHECK(B(h, ci, &z) == 0 && M(h, ci) == 0 && E(h, ci, &z) == 0, "frame %d", i);
        fflush_(&FX[ci].c); /* the app's flush */
        if (drain)
            CHECK(ps3dk_prof_drain() == 0, "drain %d", i);
    }
    return 0;
}

/* ================================================================ Mode S cases */
#ifndef GPP_HOST_DEVMEM
static int s_e2e(void)
{
    uint32_t h;
    ps3dk_prof_gpu_stats st;
    mem_fill(0);
    fx_init(0, 0, 100, 64, 200);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 100, 64, 200, &h) == 0 && h == 1026, "attach through the GCM port");
    CHECK(ps3dk_prof__test_gpu_gpp_ready(0) == 1, "the slot's port is prepared");
    CHECK(poisoned(0, 100, 64, 200) && mem_untouched_outside(0, 100, 64, 200),
          "attach poisoned exactly the lent range and zeroed the label, through the port");
    frames(h, 0, 50, 1);
    CHECK(ps3dk_prof_gpu_calibrate(h, CTXP(0), 1000000) == 0, "calibrate (the core's flush runs the executor)");
    CHECK(ps3dk_prof_gpu_detach(h, CTXP(0), 2000000) == 0, "detach (inline drains)");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.retired && st.final_state == 1 && st.events == 150 &&
              st.lost == 0 && st.brackets == 1,
          "FINALIZED 150 (events %llu lost %llu)", (unsigned long long)st.events, (unsigned long long)st.lost);
    CHECK(atomic_load(&g_exec_bad) == 0 && atomic_load(&g_exec_pairs) == 151, "151 pairs, every word valid (%llu)",
          (unsigned long long)atomic_load(&g_exec_pairs));
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    CHECK(mem_untouched_outside(0, 100, 64, 200), "nothing outside the lent range, ever");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-s-e2e") == 0, "capture");
    write_truth("r4a-s-e2e");
    finish_expect("r4a-s-e2e", 0, 150, 0, 1, "brackets 1\n");
    return 0;
}
/* two domains, two banks, two contexts: every access lands only in its own domain's memory */
static int s_identity(void)
{
    uint32_t h1, h2;
    mem_fill(0);
    mem_fill(1);
    fx_init(0, 0, 0, 32, 64);
    fx_init(1, 1, 512, 32, 70);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(2, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 32, 64, &h1) == 0, "attach A (bank A)");
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_B;
    CHECK(ps3dk_prof_gpu_attach(CTXP(1), 512, 32, 70, &h2) == 0, "attach B (bank B)");
    for (int i = 0; i < 20; i++) {
        frames(h1, 0, 1, 0);
        frames(h2, 1, 1, 1);
    }
    CHECK(ps3dk_prof_gpu_detach(h1, CTXP(0), 2000000) == 0 && ps3dk_prof_gpu_detach(h2, CTXP(1), 2000000) == 0,
          "both detach");
    {
        ps3dk_prof_gpu_stats s1, s2;
        CHECK(ps3dk_prof_gpu_get_stats(h1, &s1) == 0 && ps3dk_prof_gpu_get_stats(h2, &s2) == 0 && s1.events == 60 &&
                  s2.events == 60 && s1.lost == 0 && s2.lost == 0 && s1.final_state == 1 && s2.final_state == 1,
              "each domain delivered its own domain's 60 events (A %llu/%llu, B %llu/%llu)",
              (unsigned long long)s1.events, (unsigned long long)s1.lost, (unsigned long long)s2.events,
              (unsigned long long)s2.lost);
    }
    CHECK(atomic_load(&g_exec_bad) == 0, "each context's words named only its own domain's reports and label");
    CHECK(mem_untouched_outside(0, 0, 32, 64) && mem_untouched_outside(1, 512, 32, 70),
          "each bank touched only inside its own domain's range and label");
    CHECK(ps3dk_prof_gpu_release(h1, NULL) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "release");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-s-identity") == 0, "capture");
    {
        const dspec ds[2] = {{h1, 0}, {h2, 1}};
        CHECK(write_dtruth("r4a-s-identity", ds, 2) == 0, "the executor log matches the notes");
    }
    finish_expect("r4a-s-identity", 0, 120, 0, 1, "domains 2\n");
    return 0;
}
/* a refused preparation: EINVAL, zero device writes, nothing visible changed; then the same slot works */
static int s_prepare_fail(const char *where)
{
    uint32_t h = 0;
    char name[64];
    snprintf(name, sizeof name, "r4a-s-prepare-fail-%s", where);
    mem_fill(0);
    fx_init(0, 0, 40, 16, 90);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    if (!strcmp(where, "label"))
        g_fail_label = 1;
    else if (!strcmp(where, "reserved"))
        ; /* label 255: inside the runtime's range, refused by the port's predicate */
    else
        g_fail_index = !strcmp(where, "first") ? 40 : !strcmp(where, "middle") ? 47 : 55;
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 40, 16, !strcmp(where, "reserved") ? 255 : 90, &h) == PS3DK_PROF_EINVAL,
          "attach refused");
    CHECK(mem_untouched_outside(0, 0, 0, 0), "zero device writes");
    CHECK(ps3dk_prof__test_gpu_gpp_ready(0) == 0 && ps3dk_prof__test_gpu_state(0) == 0 &&
              ps3dk_prof__test_gpu_users(0) == 0,
          "the slot: port zeroed, FREE, unpinned");
    g_fail_index = -1;
    g_fail_label = 0;
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 40, 16, 90, &h) == 0 && h == 1026, "the same slot attaches: id 1026 "
                                                                               "(lifetime unchanged), got %u", h);
    frames(h, 0, 4, 1);
    CHECK(ps3dk_prof_gpu_detach(h, CTXP(0), 2000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "cycle");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture(name) == 0, "capture");
    write_truth(name);
    finish_expect(name, 0, 12, 0, 1, NULL);
    return 0;
}
/* prepare succeeds, prg_attach then refuses (its own ledger credit check): no device write, no credit, no id */
static int s_prg_attach_fail(void)
{
    uint32_t h = 0;
    mem_fill(0);
    fx_init(0, 0, 0, 16, 64);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    ps3dk_prof__test_gpu_ledger_exhaust(1);
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 16, 64, &h) == PS3DK_PROF_EINVAL, "prg_attach refused");
    CHECK(ps3dk_prof__test_gpu_gpp_ready(0) == 1, "the port WAS prepared (prepare ran first)");
    CHECK(mem_untouched_outside(0, 0, 0, 0), "but nothing was written: the refusal precedes the poison");
    CHECK(ps3dk_prof__test_gpu_state(0) == 0, "the slot stays FREE");
    ps3dk_prof__test_gpu_ledger_exhaust(0);
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 16, 64, &h) == 0 && h == 1026, "re-selected and re-prepared: id 1026");
    CHECK(poisoned(0, 0, 16, 64), "now poisoned");
    frames(h, 0, 2, 1);
    CHECK(ps3dk_prof_gpu_detach(h, CTXP(0), 2000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "cycle");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-s-prg-attach-fail") == 0, "capture");
    write_truth("r4a-s-prg-attach-fail");
    finish_expect("r4a-s-prg-attach-fail", 0, 6, 0, 1, NULL);
    return 0;
}
/* hook precedence and the re-entry guard during preparation */
static prg_port g_legacy; /* never called: the conflict is refused first */
static int s_hooks(void)
{
    uint32_t h = 0;
    mem_fill(0);
    mem_fill(1);
    fx_init(0, 0, 0, 16, 64);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    ps3dk_prof_test_hooks.gpu_port = &g_legacy;
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 16, 64, &h) == PS3DK_PROF_EINVAL, "both hooks: EINVAL");
    ps3dk_prof_test_hooks.gpu_gpp_dev = NULL;
    ps3dk_prof_test_hooks.gpu_port = NULL;
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 16, 64, &h) == PS3DK_PROF_ENOTSUP, "neither: ENOTSUP");
    CHECK(mem_untouched_outside(0, 0, 0, 0) && atomic_load(&g_accessor_calls) == 0, "no accessor call, no write");
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_A;
    g_reenter = 1;
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 16, 64, &h) == 0 && h == 1026, "attach (an accessor re-entered)");
    CHECK(atomic_load(&g_reenter_rc) == PS3DK_PROF_EBUSY, "the re-entrant API call from inside prepare: EBUSY (%d)",
          atomic_load(&g_reenter_rc));
    CHECK(mem_untouched_outside(1, 0, 0, 0), "the re-entrant call wrote nothing");
    CHECK(ps3dk_prof_gpu_detach(h, CTXP(0), 2000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0, "cycle");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-s-hooks") == 0, "capture");
    write_truth("r4a-s-hooks");
    finish_expect("r4a-s-hooks", 0, 0, 0, 1, NULL);
    return 0;
}
/* logical release keeps the prepared state; the next attach re-prepares (here: onto bank B) */
static int s_release_keeps(void)
{
    uint32_t h1, h2;
    mem_fill(0);
    mem_fill(1);
    fx_init(0, 0, 0, 16, 64);
    fx_init(1, 1, 300, 16, 80);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 16, 64, &h1) == 0, "attach on bank A");
    frames(h1, 0, 3, 1);
    CHECK(ps3dk_prof_gpu_detach(h1, CTXP(0), 2000000) == 0 && ps3dk_prof_gpu_release(h1, NULL) == 0, "cycle 1");
    CHECK(ps3dk_prof__test_gpu_gpp_ready(0) == 1 && ps3dk_prof__test_gpu_state(0) == 0,
          "released and FREE, the prepared state kept");
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_B;
    CHECK(ps3dk_prof_gpu_attach(CTXP(1), 300, 16, 80, &h2) == 0 && h2 == 1027, "the same slot, re-prepared on bank B");
    CHECK(poisoned(1, 300, 16, 80) && mem_untouched_outside(1, 300, 16, 80), "bank B poisoned in its range only");
    frames(h2, 1, 3, 1);
    CHECK(mem_untouched_outside(0, 0, 16, 64), "bank A untouched after the re-preparation");
    CHECK(ps3dk_prof_gpu_detach(h2, CTXP(1), 2000000) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "cycle 2");
    CHECK(atomic_load(&g_exec_bad) == 0, "valid words");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-s-release-keeps") == 0, "capture");
    {
        const dspec ds[2] = {{h1, 0}, {h2, 1}};
        CHECK(write_dtruth("r4a-s-release-keeps", ds, 2) == 0, "the executor log matches the notes");
    }
    finish_expect("r4a-s-release-keeps", 0, 18, 0, 1, "domains 2\n");
    return 0;
}
/* attach succeeds, its closing unlock fails: ESTOPPED with the diagnostic id, QUARANTINED, port kept */
static int s_unlock_fail(void)
{
    uint32_t h = 0;
    mem_fill(0);
    fx_init(0, 0, 0, 16, 64);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    ps3dk_prof_test_hooks.spu_unlock_fail_site = 4; /* U_GPU_ATTACH */
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 16, 64, &h) == PS3DK_PROF_ESTOPPED && h == 1026, "ESTOPPED, id 1026");
    CHECK(ps3dk_prof__test_gpu_state(0) == 2 && ps3dk_prof__test_gpu_gpp_ready(0) == 1,
          "QUARANTINED for life, the prepared port kept");
    CHECK(poisoned(0, 0, 16, 64), "the attach's poison happened (the resources stay borrowed)");
    ps3dk_prof_thread_end();
    (void)ps3dk_prof_shutdown();
    CHECK(take_capture("r4a-s-unlock-fail") == 0, "the capture file is named (no claims about its content)");
    return 0;
}
/* the FIFO refuses: the port's refusal is the core's refusal (LOSS) */
static int s_fifo_refusal(void)
{
    uint32_t h;
    ps3dk_prof_gpu_stats st;
    mem_fill(0);
    fx_init(0, 0, 0, 64, 64);
    CHECK(start(PS3DK_PROF_MODE_APP, ext(1, 64), &DEV_A) == 0, "start");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h) == 0, "attach");
    FX[0].c.end = FX[0].buf + 12; /* room for two pairs */
    FX[0].refuse = 1;
    for (int i = 0; i < 5; i++)
        CHECK(M(h, 0) == 0, "mark %d (numbered; refused ones are lost)", i);
    FX[0].refuse = 0;
    FX[0].c.end = FX[0].buf + CBUF;
    fflush_(&FX[0].c);
    CHECK(ps3dk_prof_drain() == 0, "drain");
    CHECK(ps3dk_prof_gpu_detach(h, CTXP(0), 2000000) == 0, "detach");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.lost_fifo == 3 && st.events == 2 && st.lost == 3,
          "2 events, 3 FIFO losses (events %llu lost %llu lost_fifo %llu)", (unsigned long long)st.events,
          (unsigned long long)st.lost, (unsigned long long)st.lost_fifo);
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-s-fifo-refusal") == 0, "capture");
    write_truth("r4a-s-fifo-refusal");
    finish_expect("r4a-s-fifo-refusal", 4, 2, 3, 1, NULL); /* the reader: lossy coverage */
    return 0;
}
#endif

/* ================================================================ Mode C cases (threaded, the SC seam) */
#ifdef GPP_HOST_DEVMEM
static pthread_t g_exec;
static void exec_start(void) { pthread_create(&g_exec, NULL, executor, NULL); }
static void exec_finish(void)
{
    atomic_store(&g_exec_stop, 1);
    pthread_join(g_exec, NULL);
}
static void wait_exec_idle(void) /* the app's Finish: every submitted packet executed */
{
    while (!exec_quiesced())
        usleep(50);
}
static int c_e2e(void)
{
    uint32_t h;
    ps3dk_prof_gpu_stats st;
    mem_fill(0);
    fx_init(0, 0, 100, 128, 200);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(1, 128), &DEV_A) == 0, "start");
    exec_start();
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 100, 128, 200, &h) == 0, "attach");
    for (int i = 0; i < 300; i++) {
        frames(h, 0, 1, 0);
        if (i % 30 == 29)
            usleep(3000);
    }
    CHECK(ps3dk_prof_gpu_calibrate(h, CTXP(0), 2000000) == 0, "calibrate");
    wait_exec_idle(); /* the app's Finish */
    CHECK(ps3dk_prof_gpu_detach(h, CTXP(0), 5000000) == 0, "detach");
    CHECK(ps3dk_prof_gpu_get_stats(h, &st) == 0 && st.final_state == 1 && st.events == 900 && st.lost == 0 &&
              st.brackets == 1,
          "FINALIZED 900 (events %llu lost %llu)", (unsigned long long)st.events, (unsigned long long)st.lost);
    CHECK(ps3dk_prof_gpu_release(h, NULL) == 0, "release");
    exec_finish();
    CHECK(atomic_load(&g_exec_bad) == 0 && atomic_load(&g_exec_pairs) == 901, "901 valid pairs");
    CHECK(mem_untouched_outside(0, 100, 128, 200), "nothing outside the lent range");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-c-e2e") == 0, "capture");
    write_truth("r4a-c-e2e");
    finish_expect("r4a-c-e2e", 0, 900, 0, 1, "brackets 1\n");
    return 0;
}
/* the app's Finish while the executor holds a DEQUEUED packet whose device writes have not happened: the Finish stays
 * pending (the queue is already empty) and release is not attempted until the packet completes (emulator
 * quiescence, not a hardware claim) */
static int c_finish_pending(void)
{
    uint32_t h;
    mem_fill(0);
    fx_init(0, 0, 0, 64, 64);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(1, 64), &DEV_A) == 0, "start");
    exec_start();
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h) == 0, "attach");
    atomic_store(&g_xpause, 1);
    CHECK(M(h, 0) == 0, "mark");
    fflush_(&FX[0].c);
    while (atomic_load(&g_xpause) != 2)
        usleep(50);
    pthread_mutex_lock(&g_qm);
    int queue_empty = g_qt == g_qh;
    pthread_mutex_unlock(&g_qm);
    CHECK(queue_empty, "the packet was dequeued (the queue is empty)");
    CHECK(!exec_quiesced(), "the Finish is pending while a dequeued packet is unexecuted");
    CHECK(DM_LD32(&BK[0].lab[64]) == 0 && DM_LD64(&BK[0].rep[0].timer) == PRG_POISON,
          "its report and label are not written yet");
    atomic_store(&g_xgo, 1);
    wait_exec_idle();
    CHECK(exec_quiesced() && DM_LD32(&BK[0].lab[64]) == 1 && DM_LD64(&BK[0].rep[0].timer) != PRG_POISON,
          "after completion: quiescent, report and label written");
    CHECK(ps3dk_prof_gpu_detach(h, CTXP(0), 3000000) == 0 && ps3dk_prof_gpu_release(h, NULL) == 0,
          "detach + release only after the Finish completed");
    exec_finish();
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-c-finish-pending") == 0, "capture");
    write_truth("r4a-c-finish-pending");
    finish_expect("r4a-c-finish-pending", 0, 1, 0, 1, NULL);
    return 0;
}
/* release lands between the ack store and the publisher's unpin: the slot is not re-selected until that unpin */
static atomic_int g_ack_hold, g_ack_holding, g_ack_go;
static void ack_pause(void)
{
    if (atomic_exchange(&g_ack_hold, 0)) {
        atomic_store(&g_ack_holding, 1);
        while (!atomic_load(&g_ack_go))
            usleep(200);
    }
}
static int c_ack_release(void)
{
    uint32_t h1, h2 = 0, rel = 9;
    mem_fill(0);
    fx_init(0, 0, 0, 64, 64);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(1, 64), &DEV_A) == 0, "start");
    exec_start();
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h1) == 0, "attach");
    frames(h1, 0, 2, 0);
    wait_exec_idle();
    usleep(20000);
    ps3dk_prof_test_hooks.gpu_after_ack_store = ack_pause;
    atomic_store(&g_ack_hold, 1);
    CHECK(ps3dk_prof_gpu_detach(h1, CTXP(0), 3000000) == 0, "detach OK: the ack is stored");
    for (int i = 0; i < 2000 && !atomic_load(&g_ack_holding); i++)
        usleep(500);
    CHECK(atomic_load(&g_ack_holding), "the publisher is paused after its store, still pinned");
    CHECK(ps3dk_prof_gpu_release(h1, &rel) == 0 && rel == 1, "release succeeds while the publisher is pinned");
    CHECK(ps3dk_prof__test_gpu_users(0) >= 1, "the publisher's pin is visible");
    for (uint32_t i = 0; i < 64; i++)
        DM_ST64(&BK[0].rep[i].timer, 0x1234); /* a marker: the re-preparation's poison must overwrite it */
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h2) == PS3DK_PROF_EFULL, "not re-selected while pinned");
    CHECK(DM_LD64(&BK[0].rep[0].timer) == 0x1234, "and not touched");
    atomic_store(&g_ack_go, 1);
    for (int i = 0; i < 2000 && ps3dk_prof__test_gpu_users(0) != 0; i++)
        usleep(500);
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h2) == 0 && h2 == 1027, "re-selected after the unpin");
    CHECK(poisoned(0, 0, 64, 64), "re-prepared: the new domain poisoned its range");
    frames(h2, 0, 2, 0);
    wait_exec_idle();
    CHECK(ps3dk_prof_gpu_detach(h2, CTXP(0), 3000000) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "cycle 2");
    exec_finish();
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-c-ack-release") == 0, "capture");
    {
        const dspec ds[2] = {{h1, 0}, {h2, 0}};
        CHECK(write_dtruth("r4a-c-ack-release", ds, 2) == 0, "the executor log matches the notes");
    }
    finish_expect("r4a-c-ack-release", 0, 12, 0, 1, "domains 2\n");
    return 0;
}
/* a foreign caller pinned across the release keeps the slot from being re-selected; it then misses the id */
static atomic_int g_pin_hold, g_pin_holding, g_pin_go, g_foreign_rc;
static uint32_t g_h;
static void pin_pause(void)
{
    if (atomic_exchange(&g_pin_hold, 0)) {
        atomic_store(&g_pin_holding, 1);
        while (!atomic_load(&g_pin_go))
            usleep(200);
    }
}
static void *foreign(void *a)
{
    (void)a;
    atomic_store(&g_foreign_rc, ps3dk_prof_gpu_mark(g_h, CTXP(0), &D_B));
    return NULL;
}
static int c_stale_pin(void)
{
    uint32_t h2 = 0;
    pthread_t th;
    mem_fill(0);
    fx_init(0, 0, 0, 64, 64);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(1, 64), &DEV_A) == 0, "start");
    exec_start();
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &g_h) == 0, "attach");
    frames(g_h, 0, 2, 0);
    wait_exec_idle();
    ps3dk_prof_test_hooks.gpu_after_pin_inc = pin_pause;
    atomic_store(&g_pin_hold, 1);
    pthread_create(&th, NULL, foreign, NULL);
    while (!atomic_load(&g_pin_holding))
        usleep(100);
    ps3dk_prof_test_hooks.gpu_after_pin_inc = NULL;
    CHECK(ps3dk_prof_gpu_detach(g_h, CTXP(0), 3000000) == 0 && ps3dk_prof_gpu_release(g_h, NULL) == 0,
          "detach + release while the foreign pin is held");
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h2) == PS3DK_PROF_EFULL, "not re-selected under the foreign pin");
    atomic_store(&g_pin_go, 1);
    pthread_join(th, NULL);
    CHECK(atomic_load(&g_foreign_rc) == PS3DK_PROF_EINVAL, "the foreign call misses the cleared id (got %d)",
          atomic_load(&g_foreign_rc));
    CHECK(ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h2) == 0 && h2 == 1027, "re-selected after the pin drained");
    CHECK(ps3dk_prof_gpu_detach(h2, CTXP(0), 3000000) == 0 && ps3dk_prof_gpu_release(h2, NULL) == 0, "cycle 2");
    exec_finish();
    CHECK(atomic_load(&g_exec_bad) == 0, "valid words");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(take_capture("r4a-c-stale-pin") == 0, "capture");
    {
        const dspec ds[2] = {{g_h, 0}, {h2, 0}};
        CHECK(write_dtruth("r4a-c-stale-pin", ds, 2) == 0, "the executor log matches the notes");
    }
    finish_expect("r4a-c-stale-pin", 0, 6, 0, 1, "domains 2\n");
    return 0;
}
/* the seal while an admitted producer is paused INSIDE the port's reserve (the context callback) */
static atomic_int g_cb_stage, g_cb_go, g_sh_mark_rc = -100, g_sh_release_rc = -100;
static void cb_pause(void)
{
    if (atomic_load(&g_cb_stage) == 1) {
        atomic_store(&g_cb_stage, 2);
        while (!atomic_load(&g_cb_go))
            usleep(50);
    }
}
static void *sh_owner(void *a)
{
    uint32_t h;
    (void)a;
    if (ps3dk_prof_thread_begin("render") != 0 || ps3dk_prof_gpu_attach(CTXP(0), 0, 64, 64, &h) != 0)
        return NULL;
    if (M(h, 0) != 0) /* number 0, also registers the descriptor before the claim */
        return NULL;
    fflush_(&FX[0].c);
    usleep(20000);
    FX[0].end_after_cb = FX[0].c.end;
    FX[0].c.end = FX[0].c.current + 5; /* five words of room: the next reserve must call the callback */
    FX[0].pause = cb_pause;
    atomic_store(&g_cb_stage, 1);
    atomic_store(&g_sh_mark_rc, M(h, 0));
    FX[0].pause = NULL;
    fflush_(&FX[0].c);
    wait_exec_idle(); /* the app's Finish */
    atomic_store(&g_sh_release_rc, ps3dk_prof_gpu_release(h, NULL));
    return NULL;
}
static int c_seal_in_reserve(void)
{
    pthread_t th;
    mem_fill(0);
    fx_init(0, 0, 0, 64, 64);
    CHECK(start(PS3DK_PROF_MODE_THREAD, ext(1, 64), &DEV_A) == 0, "start");
    exec_start();
    pthread_create(&th, NULL, sh_owner, NULL);
    while (atomic_load(&g_cb_stage) != 2)
        usleep(50);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_ENOTQUIESCED, "the seal while the caller is paused in reserve");
    atomic_store(&g_cb_go, 1);
    pthread_join(th, NULL);
    exec_finish();
    CHECK(atomic_load(&g_sh_mark_rc) == PS3DK_PROF_OK, "the admitted call completes after the seal (%d)",
          atomic_load(&g_sh_mark_rc));
    CHECK(atomic_load(&g_sh_release_rc) == PS3DK_PROF_OK, "release after the app's Finish (%d)",
          atomic_load(&g_sh_release_rc));
    CHECK(atomic_load(&g_exec_bad) == 0, "valid words (the paused pair completed into the buffer)");
    CHECK(take_capture("r4a-c-seal-in-reserve") == 0, "capture");
    g_ntr = 1; /* number 0 only: the paused call lies beyond the frozen frontier */
    write_truth("r4a-c-seal-in-reserve");
    finish_expect("r4a-c-seal-in-reserve", 1, 1, 0, 1, "not_quiesced 1\n");
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    g_case = argv[1];
    g_out = argv[2];
#ifndef GPP_HOST_DEVMEM
    if (!strcmp(g_case, "s-e2e"))
        s_e2e();
    else if (!strcmp(g_case, "s-identity"))
        s_identity();
    else if (!strncmp(g_case, "s-prepare-fail-", 15))
        s_prepare_fail(g_case + 15);
    else if (!strcmp(g_case, "s-prg-attach-fail"))
        s_prg_attach_fail();
    else if (!strcmp(g_case, "s-hooks"))
        s_hooks();
    else if (!strcmp(g_case, "s-release-keeps"))
        s_release_keeps();
    else if (!strcmp(g_case, "s-unlock-fail"))
        s_unlock_fail();
    else if (!strcmp(g_case, "s-fifo-refusal"))
        s_fifo_refusal();
#else
    if (!strcmp(g_case, "c-e2e"))
        c_e2e();
    else if (!strcmp(g_case, "c-finish-pending"))
        c_finish_pending();
    else if (!strcmp(g_case, "c-ack-release"))
        c_ack_release();
    else if (!strcmp(g_case, "c-stale-pin"))
        c_stale_pin();
    else if (!strcmp(g_case, "c-seal-in-reserve"))
        c_seal_in_reserve();
#endif
    else {
        printf("R4A %s FAIL unknown case\n", g_case);
        return 2;
    }
    if (g_bad)
        return 1;
    printf("R4A %s PASS\n", g_case);
    return 0;
}
