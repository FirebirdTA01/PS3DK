/* Capture RSX frame S2: the runtime harness.  The REAL runtime (prof_core, prof_drain, ext_runtime, gpu_runtime,
 * rsxcap) and the REAL GCM port (gpu_port_gcm.c) over fake GCM memory and fake command buffers whose words a small
 * executor decodes in place (the R4a Mode S pattern: APP mode, the designated drain thread is main; device memory is
 * touched on that thread only).  The production submit path: runtime entry -> prg.c submit -> the port's
 * reserve_pair writing words into the fake buffer.  No case calls submit() or fabricates a receipt.  Other threads
 * stand in for the agent (S4), non-owner librsx hooks (S3) and the stopper.
 *
 * One case per process.  Prints "RT <case> PASS" or "RT <case> FAIL ...".  A case that writes a capture also writes
 * <name>.prof and <name>.expect.  usage: rsxcap_rt <case> <out-dir> */
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
#include "rsxcap.h"
#include "rsxcap_steps.h"

uint32_t ps3dk_prof__test_gpu_users(unsigned i);
uint32_t ps3dk_prof__test_gpu_state(unsigned i);
void ps3dk_prof__test_gpu_ledger_exhaust(int on);
void ps3dk_prof__test_rsxcap_rec(uint32_t cap, rsxcap_rec *out);
void ps3dk_prof__test_rsxcap_stop(void);
void ps3dk_prof__test_gpu_dom(unsigned i, uint64_t *seq, uint64_t *ticket, uint32_t *depth);
void ps3dk_prof__test_gpu_set_users(unsigned i, uint32_t v);
uint32_t ps3dk_prof__test_rsxcap_capable(unsigned i);
uint32_t ps3dk_prof__test_rsxcap_eligible(void);

static const char *g_out;
static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("RT %s FAIL line %d: ", g_case_name, __LINE__);                                                    \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---------------------------------------------------------------- fake device memory, buffers, executor */
typedef struct rep_ent {
    uint64_t timer;
    uint32_t value, zero;
} rep_ent;
typedef struct bank {
    rep_ent rep[GPP_REPORTS];
    uint32_t lab[GPP_LABEL_LAST + 1];
} bank;
static bank BK[2] __attribute__((aligned(16)));
static void *rep_a(uint32_t i) { return i < GPP_REPORTS ? &BK[0].rep[i] : NULL; }
static void *rep_b(uint32_t i) { return i < GPP_REPORTS ? &BK[1].rep[i] : NULL; }
static void *lab_a(uint32_t i) { return i <= GPP_LABEL_LAST ? &BK[0].lab[i] : NULL; }
static void *lab_b(uint32_t i) { return i <= GPP_LABEL_LAST ? &BK[1].lab[i] : NULL; }

#define CBUF 600u
typedef struct fctx {
    gpp_ctx c;
    int bank;
    uint32_t first, count, label;
    uint32_t buf[CBUF] __attribute__((aligned(16)));
    int refuse;
} fctx;
static fctx FX[2];
static atomic_int g_exec_bad;
static atomic_ullong g_exec_pairs[2];
static uint64_t g_clock = 1000;
static uint32_t unperm(uint32_t v) { return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16); }
static void exec_pair(const fctx *f, const uint32_t *w)
{
    uint32_t r = (w[1] & 0x00ffffffu) / 16u, l = w[3] / 16u;
    if (w[0] != 0x00041800u || (w[1] >> 24) != 1u || w[2] != 0x00041d6cu || w[4] != 0x00041d70u || r < f->first ||
        r >= f->first + f->count || l != f->label) {
        atomic_fetch_add(&g_exec_bad, 1);
        return;
    }
    g_clock += 37;
#ifdef GPP_HOST_DEVMEM /* the TSan build: every device-memory access is an SC atomic (the R4a Mode C seam) */
    __atomic_store_n(&BK[f->bank].rep[r].timer, g_clock, __ATOMIC_SEQ_CST);
    __atomic_store_n(&BK[f->bank].lab[l], unperm(w[5]), __ATOMIC_SEQ_CST);
#else
    BK[f->bank].rep[r].timer = g_clock;
    BK[f->bank].lab[l] = unperm(w[5]);
#endif
    atomic_fetch_add(&g_exec_pairs[f - FX], 1);
}
static void handover(fctx *f)
{
    uint32_t n = (uint32_t)(f->c.current - f->c.begin);
    for (uint32_t k = 0; k + 6 <= n; k += 6)
        exec_pair(f, f->c.begin + k);
    f->c.current = f->c.begin;
}
static int32_t fcb(gpp_ctx *c, uint32_t count)
{
    fctx *f = (fctx *)(void *)c;
    (void)count;
    if (f->refuse)
        return -1;
    handover(f);
    return 0;
}
static void fflush_(gpp_ctx *c) { handover((fctx *)(void *)c); }
#ifdef GPP_HOST_DEVMEM
static uint64_t sl64(volatile uint64_t *a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }
static void ss64(volatile uint64_t *a, uint64_t v) { __atomic_store_n(a, v, __ATOMIC_SEQ_CST); }
static uint32_t sl32(volatile uint32_t *a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }
static void ss32(volatile uint32_t *a, uint32_t v) { __atomic_store_n(a, v, __ATOMIC_SEQ_CST); }
#define SEAM , sl64, ss64, sl32, ss32
#else
#define SEAM
#endif
static const gpp_dev DEV_A = {rep_a, lab_a, fflush_ SEAM};
static const gpp_dev DEV_B = {rep_b, lab_b, fflush_ SEAM};
typedef struct dsnap {
    uint64_t seq, ticket;
    uint32_t depth;
} dsnap;
static dsnap dom_snap(unsigned i)
{
    dsnap s;
    ps3dk_prof__test_gpu_dom(i, &s.seq, &s.ticket, &s.depth);
    return s;
}
static int dom_same(unsigned i, const dsnap *a, int depth_delta)
{
    dsnap s = dom_snap(i);
    return s.seq == a->seq && s.ticket == a->ticket && (int)s.depth == (int)a->depth + depth_delta;
}
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
#define CTX(i) ((ps3dk_prof_gcm_ctx *)(void *)&FX[i].c)
/* a word copy of a command buffer and its executed pairs (R60: no GPU word after the latch) */
typedef struct wsnap {
    uint32_t buf[CBUF];
    ptrdiff_t cur;
    unsigned long long pairs;
} wsnap;
static void wsnap_take(int i, wsnap *s)
{
    memcpy(s->buf, FX[i].buf, sizeof s->buf);
    s->cur = FX[i].c.current - FX[i].c.begin;
    s->pairs = atomic_load(&g_exec_pairs[i]);
}
static int wsnap_same(int i, const wsnap *s)
{
    return !memcmp(s->buf, FX[i].buf, sizeof s->buf) && s->cur == FX[i].c.current - FX[i].c.begin &&
           s->pairs == atomic_load(&g_exec_pairs[i]);
}

/* ---------------------------------------------------------------- the step seams */
static void rt_step(int point) { step_hit(point); }
#define PRG_PT(n) (40 + (n)) /* the runtime points use 1..39 */
void prg_test_step(struct prg_dom *d, int point)
{
    (void)d;
    step_hit(PRG_PT(point));
}

/* ---------------------------------------------------------------- the runtime */
static uint32_t H[2];
static ps3dk_prof_desc D_APP = {"app.zone", "", "rsxcap_rt.c", 1, 2, 0, 0};
static ps3dk_prof_ext_config ext(uint32_t gpus, uint32_t slots)
{
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.gpu_domains_max = gpus;
    x.gpu_report_slots_max = slots;
    return x;
}
static uint32_t g_maxd; /* 0: default */
static int g_thread_mode; /* THREAD mode: the real drain thread runs concurrently (the device-memory seam only) */
static int start(uint32_t gpus, uint32_t slots)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = g_thread_mode ? PS3DK_PROF_MODE_THREAD : PS3DK_PROF_MODE_APP;
    c.app = "rsxcap";
    c.build_id = "rsxcap";
    c.drain_interval_us = 1000;
    c.quiesce_timeout_us = 100000;
    if (g_maxd)
        c.max_descriptors = g_maxd;
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_A;
    ps3dk_prof_test_hooks.rsxcap_step = rt_step;
    if (ps3dk_prof_set_sink_path(g_out) != 0)
        return -1;
    ps3dk_prof_ext_config x = ext(gpus, slots);
    return ps3dk_prof_init_ex(&c, &x);
}
static uint32_t g_slots = 256;
/* init (APP), enable, register main as "render", attach n domains (A: ctx 0 bank A; B: ctx 1 bank B) */
static void setup_n(int enable, int n)
{
    fx_init(0, 0, 0, g_slots, 64);
    fx_init(1, 1, 1024, g_slots, 70);
    CHECK(start(2, g_slots) == 0, "start");
    if (enable)
        CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_OK, "enable");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread_begin");
    if (n >= 1)
        CHECK(ps3dk_prof_gpu_attach(CTX(0), 0, g_slots, 64, &H[0]) == 0 && H[0] == 1026, "attach A");
    if (n >= 2) {
        ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_B;
        CHECK(ps3dk_prof_gpu_attach(CTX(1), 1024, g_slots, 70, &H[1]) == 0, "attach B");
        ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_A;
    }
}
static void setup(void) { setup_n(1, 1); }
static ps3dk_prof_token tokn(void)
{
    ps3dk_prof_frame();
    return ps3dk_prof_frame_token();
}
static const ps3dk_prof_token NONE_TOK = {0, 0, 0};
static int FR(int d) { return ps3dk_prof_gpu_frame(H[d], tokn()); }
static int FRN(int d) { return ps3dk_prof_gpu_frame(H[d], NONE_TOK); }
static void drain(void)
{
    fflush_(&FX[0].c);
    fflush_(&FX[1].c);
    if (!g_thread_mode) /* THREAD mode: the drain thread drains */
        CHECK(ps3dk_prof_drain() == 0, "drain");
}
static uint64_t SW(void) { return __atomic_load_n(&ps3dk_prof__rsxcap_slot, __ATOMIC_SEQ_CST); }
#define ST(w) RSXCAP_STATE(w)
#define EP(w) RSXCAP_EPOCH(w)
static uint32_t g_reason;
static int REQ(uint32_t dom, uint32_t *cap) { return ps3dk_prof__rsxcap_request(1, 1, dom, cap, &g_reason); }
static rsxcap_outcome Q(uint32_t cap)
{
    rsxcap_outcome o;
    memset(&o, 0, sizeof o);
    if (ps3dk_prof__rsxcap_query(cap, &o) != PS3DK_PROF_OK)
        o.outcome = 0xFFu;
    return o;
}
/* n draws of `kind` on domain d's ctx (the stand-in for librsx's hooks), draining every 200 */
static uint64_t g_draw_opened;
static void draws(int d, uint32_t n, uint32_t kind)
{
    for (uint32_t i = 0; i < n; i++) {
        rsxcap_draw dr;
        if (ps3dk_prof__rsxcap_draw_begin(CTX(d), kind, &dr)) {
            g_draw_opened++;
            ps3dk_prof__rsxcap_draw_end(&dr);
        }
        if (i % 200 == 199)
            drain();
    }
}
static void arm_on(int d, uint32_t *cap)
{
    CHECK(REQ(H[d], cap) == RSXCAP_ACCEPTED, "request (reason %u)", g_reason);
    CHECK(FR(d) == 0, "the arming frame");
    CHECK(ST(SW()) == RSXCAP_ARMED && EP(SW()) == *cap, "ARMED(%u) (word %llx)", *cap, (unsigned long long)SW());
}
static int outcome_is(uint32_t cap, uint32_t out, uint32_t rsn)
{
    rsxcap_outcome o = Q(cap);
    if (o.outcome != out || o.reason != rsn) {
        printf("  (cap %u: outcome %u reason %u state %u word %llx)\n", cap, o.outcome, o.reason, o.state,
               (unsigned long long)o.word);
        return 0;
    }
    return 1;
}

/* the capture file and its .expect */
static FILE *g_exp;
static char g_name[128];
static void expect_open(int exit_code)
{
    char path[512];
    snprintf(g_name, sizeof g_name, "rsxcap-%s", g_case_name);
    snprintf(path, sizeof path, "%s/%s.expect", g_out, g_name);
    g_exp = fopen(path, "w");
    fprintf(g_exp, "exit %d\n", exit_code);
}
static void expect_cap(uint32_t cap, const char *line, uint32_t ndraws, uint32_t nckpts)
{
    fprintf(g_exp, "cap %u = %s\ndraws %u %u\nckpts %u %u\n", cap, line, cap, ndraws, cap, nckpts);
}
static void expect_capx(uint32_t cap, const char *state, const char *has, uint32_t ndraws, uint32_t nckpts)
{
    fprintf(g_exp, "cap %u ~ %s\n", cap, state);
    if (has)
        fprintf(g_exp, "has %u %s\n", cap, has);
    fprintf(g_exp, "draws %u %u\nckpts %u %u\n", cap, ndraws, cap, nckpts);
}
static void expect_capx_has(uint32_t cap, const char *has) { fprintf(g_exp, "has %u %s\n", cap, has); }
static void expect_nocap(uint32_t cap) { fprintf(g_exp, "nocap %u\n", cap); }
static void expect_line(const char *l) { fprintf(g_exp, "%s\n", l); }
static int take_capture(void)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "cd '%s' && f=$(ls rsxcap-*.prof 2>/dev/null | grep -v '^rsxcap-%s.prof$' | head -1) && [ -n \"$f\" ] && "
             "mv \"$f\" '%s.prof'",
             g_out, g_case_name, g_name);
    return system(cmd) == 0 ? 0 : -1;
}
/* detach + release every attached domain, end the thread, shut down, name the capture */
static void finish_n(int n, int anomalies)
{
    for (int d = 0; d < n; d++) {
        fflush_(&FX[d].c);
        CHECK(ps3dk_prof_gpu_detach(H[d], CTX(d), 3000000) == 0, "detach %d", d);
        CHECK(ps3dk_prof_gpu_release(H[d], NULL) == 0, "release %d", d);
    }
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    fprintf(g_exp, "anomalies %d\n", anomalies);
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
    CHECK(atomic_load(&g_exec_bad) == 0, "every executed word valid");
}
static void finish(void) { finish_n(1, 0); }
/* a run whose domain cannot finalize (a fault): shutdown only */
static void finish_faulted(int anomalies)
{
    ps3dk_prof_thread_end();
    (void)ps3dk_prof_shutdown();
    fprintf(g_exp, "anomalies %d\n", anomalies);
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}

/* a helper thread: wait for the armed pause, act, release */
typedef struct actor {
    pthread_t t;
    void (*fn)(void);
} actor;
static void *actor_main(void *a)
{
    actor *x = a;
    step_wait_paused();
    x->fn();
    step_go();
    return NULL;
}
static void actor_start(actor *a, void (*fn)(void))
{
    a->fn = fn;
    pthread_create(&a->t, NULL, actor_main, a);
}
static void actor_join(actor *a) { pthread_join(a->t, NULL); }

static uint32_t C1, C2;
static void act_cancel_c1(void) { CHECK(ps3dk_prof__rsxcap_cancel(C1) == PS3DK_PROF_OK, "cancel %u", C1); }
static void act_cancel_reclaim(void)
{
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == PS3DK_PROF_OK, "cancel");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED && C2 == C1 + 1, "re-claim %u", C2);
}
static void act_stop(void) { ps3dk_prof__test_rsxcap_stop(); }

/* ================================================================ receipts (R1-R3) */
static void c_receipt_event(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 1, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.outcome == RSXCAP_O_DONE && o.end.c.ev == 2 && o.end.hooks == 1 && o.end.c.lost == 0, "EVENT x2 (ev %u)",
          o.end.c.ev);
    expect_open(0);
    expect_cap(C1, "FINALIZED", 1, 0);
    finish();
}
static void c_receipt_lost_fifo(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 1, RSXCAP_K_ARRAYS); /* control: EVENT */
    drain();
    FX[0].refuse = 1;
    FX[0].c.end = FX[0].c.current + 3; /* no room: the callback runs and refuses */
    draws(0, 1, RSXCAP_K_INDEX);
    FX[0].refuse = 0;
    FX[0].c.end = FX[0].buf + CBUF;
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.c.ev == 2 && o.end.c.lost == 2 && o.outcome == RSXCAP_O_DONE, "ev 2 lost 2 (%u %u), DONE", o.end.c.ev,
          o.end.c.lost);
    ps3dk_prof_gpu_stats st;
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &st) == 0 && st.lost_fifo == 2, "lost_fifo 2");
    expect_open(4);
    expect_capx(C1, "PARTIAL", "LOSS inside", 1, 0);
    finish();
}
static void c_receipt_lost_ring(void)
{
    g_slots = 2;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    CHECK(FR(0) == 0, "arm: BEGIN is ticket 0");
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "the 2nd submission (control): EVENT");
    ps3dk_prof__rsxcap_draw_end(&dr); /* the 3rd: the ring is full, LOST */
    ps3dk_prof_gpu_stats st;
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &st) == 0 && st.lost_ring == 1, "lost_ring 1 (%llu)",
          (unsigned long long)st.lost_ring);
    drain();
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.c.ev == 1 && o.end.c.lost == 1, "ev 1 lost 1 (%u %u)", o.end.c.ev, o.end.c.lost);
    expect_open(4);
    expect_capx(C1, "PARTIAL", "LOSS inside", 0, 0);
    finish();
}
static void lost_records(int n, int want)
{
    g_slots = 64;
    setup();
    arm_on(0, &C1);
    for (int i = 0; i < n + 1; i++) { /* each draw: BEGIN LOST (FIFO), END EVENT closes the loss into a record */
        rsxcap_draw dr;
        FX[0].refuse = 1;
        FX[0].c.end = FX[0].c.current + 3;
        (void)ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr);
        FX[0].refuse = 0;
        FX[0].c.end = FX[0].buf + CBUF;
        ps3dk_prof__rsxcap_draw_end(&dr);
    }
    ps3dk_prof_gpu_stats st;
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &st) == 0 && (int)st.lost_records == want, "lost_records %d (%llu)", want,
          (unsigned long long)st.lost_records);
    drain();
    CHECK(FR(0) == 0, "close");
    expect_open(4);
    expect_capx(C1, "PARTIAL", "LOSS inside", 0, 0);
    finish();
}
static void c_receipt_suppressed(int depth)
{
    ps3dk_prof_gpu_zone z[64];
    setup();
    arm_on(0, &C1);
    for (int i = 0; i < depth; i++)
        CHECK(ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z[i]) == 0, "app zone %d", i);
    rsxcap_draw dr;
    int open = ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr);
    CHECK(open == 1, "zone open (SUPPRESSED keeps its token)");
    ps3dk_prof__rsxcap_draw_end(&dr);
    for (int i = depth; i--;)
        CHECK(ps3dk_prof_gpu_end(H[0], CTX(0), &z[i]) == 0, "app end %d", i);
    drain();
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    if (depth == 64)
        CHECK(o.end.c.sup == 2 && o.end.c.ev == 0, "both SUPPRESSED (sup %u ev %u)", o.end.c.sup, o.end.c.ev);
    else
        CHECK(o.end.c.ev == 2 && o.end.c.sup == 0, "control at depth 63: EVENT");
    expect_open(0);
    if (depth == 64)
        expect_capx(C1, "PARTIAL", "suppressed 2", 0, 0);
    else
        expect_cap(C1, "FINALIZED", 1, 0);
    finish();
}
static void c_receipt_refused_faulted(void)
{
    setup();
    arm_on(0, &C1);
    ps3dk_prof_test_hooks.gpu_snap_ops = 0;
    ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1; /* the draw BEGIN's publication: the domain faults */
    draws(0, 1, RSXCAP_K_ARRAYS);
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_FAULT), "settled ABANDONED(fault) in the faulting call (R80)");
    ps3dk_prof_gpu_zone z;
    CHECK(ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z) == PS3DK_PROF_ESTOPPED, "later calls refused");
    expect_open(4);
    finish_faulted(0);
}

/* ================================================================ the slot table (2.4.1) */
static void c_plain_path_identity(void)
{
    setup_n(1, 2);
    int fr0 = step_hits(PRG_PT(PRG_SP_FRAME)), fc0 = step_hits(PRG_PT(PRG_SP_FC));
    for (int i = 0; i < 3; i++)
        CHECK(FR(0) == 0, "IDLE frame");
    CHECK(REQ(H[1], &C1) == RSXCAP_ACCEPTED, "REQUESTED for B");
    for (int i = 0; i < 3; i++)
        CHECK(FR(0) == 0, "frame on A while B is REQUESTED");
    CHECK(ST(SW()) == RSXCAP_REQUESTED, "still REQUESTED");
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "cancel");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED, "request A");
    CHECK(FR(0) == 0 && FRN(0) == 0 && ST(SW()) == RSXCAP_TERMINAL, "armed and closed");
    int fr1 = step_hits(PRG_PT(PRG_SP_FRAME)), fc1 = step_hits(PRG_PT(PRG_SP_FC));
    for (int i = 0; i < 3; i++)
        CHECK(FR(0) == 0, "TERMINAL frame");
    CHECK(fc1 - fc0 == 2 && step_hits(PRG_PT(PRG_SP_FC)) == fc1, "capture entry only at the 2 boundaries");
    CHECK(fr1 - fr0 == 6 && step_hits(PRG_PT(PRG_SP_FRAME)) - fr1 == 3, "plain prg_frame = every other call");
    expect_open(0);
    expect_cap(C2, "FINALIZED", 0, 0);
    finish_n(2, 0);
}
static void c_close_done(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 10, RSXCAP_K_INDEX);
    uint32_t old = ps3dk_prof__test_gpu_users(0);
    (void)old;
    CHECK(FR(0) == 0, "close");
    CHECK(outcome_is(C1, RSXCAP_O_DONE, 0), "DONE");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.id.cap == C1 && o.end.id.dom == H[0] && o.end.id.conn == 1 && o.end.id.wire == 1 && o.end.id.owner,
          "END identity = BEGIN identity");
    CHECK(o.end.hooks == 10 && o.end.c.ev == 20 && o.end_r.kind == PRG_R_EVENT && o.begin_r.kind == PRG_R_EVENT,
          "hooks 10, ev 20");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 10, 0);
    finish();
}
static void c_close_none_token(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 3, RSXCAP_K_CLEAR);
    CHECK(FRN(0) == 0, "NONE closes");
    CHECK(outcome_is(C1, RSXCAP_O_DONE, 0), "DONE, no BEGIN");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 3, 0);
    finish();
}
static void c_arm_basic(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED && C1 == 1, "cap 1");
    CHECK(ST(SW()) == RSXCAP_REQUESTED, "REQUESTED");
    CHECK(FR(0) == 0 && ST(SW()) == RSXCAP_ARMED, "ARMED");
    draws(0, 5, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0 && outcome_is(C1, RSXCAP_O_DONE, 0), "DONE");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 5, 0);
    finish();
}
static void c_arm_none_token(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    uint64_t w = SW();
    CHECK(FRN(0) == 0 && SW() == w, "NONE: REQUESTED unchanged");
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(C1, &r);
    CHECK(r.bound == 0, "no owner binding");
    CHECK(FR(0) == 0 && ST(SW()) == RSXCAP_ARMED, "then a frame token arms");
    CHECK(FR(0) == 0, "close");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_arm_other_domain(void)
{
    setup_n(1, 2);
    CHECK(REQ(H[1], &C1) == RSXCAP_ACCEPTED, "request names B");
    uint64_t w = SW();
    CHECK(FR(0) == 0 && SW() == w, "A's frame: unchanged");
    CHECK(FR(1) == 0 && ST(SW()) == RSXCAP_ARMED, "B arms");
    draws(1, 2, RSXCAP_K_ARRAYS);
    draws(0, 2, RSXCAP_K_ARRAYS); /* the owner draws on another ctx: other_ctx */
    CHECK(FR(1) == 0 && outcome_is(C1, RSXCAP_O_DONE, 0), "DONE");
    CHECK(Q(C1).end.other_ctx == 2, "other_ctx 2");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 2, 0); /* other_ctx is reported, not a completion reason */
    finish_n(2, 0);
}
static void c_slot_claim_basic(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED && C1 == 1, "cap 1");
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(1, &r);
    CHECK(r.conn == 1 && r.wire == 1 && r.dom == H[0], "agent fields");
    CHECK(SW() == RSXCAP_WORD(RSXCAP_REQUESTED, 0, 1, 0, 0), "REQUESTED(1)");
    expect_open(0);
    finish();
}
static void c_slot_capacity_16_17(void)
{
    setup();
    for (uint32_t i = 1; i <= 16; i++) {
        CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED && C1 == i, "request %u accepted (cap %u)", i, C1);
        CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "cancel %u", i);
    }
    uint64_t w = SW();
    CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && C2 == 0 && g_reason == RSXCAP_R_CAPACITY && SW() == w,
          "the 17th: REFUSED(capacity), cap 0, word unchanged");
    expect_open(0);
    finish();
}
static void c_slot_req_refusals(void)
{
    rsxcap_rec before[16], after;
    setup_n(1, 1);
    for (uint32_t i = 1; i <= 16; i++)
        ps3dk_prof__test_rsxcap_rec(i, &before[i - 1]);
    uint64_t w = SW();
    CHECK(REQ(9999, &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_BAD_DOMAIN, "bad domain");
    CHECK(SW() == w, "word unchanged");
    /* domain faulted */
    ps3dk_prof_test_hooks.gpu_snap_ops = 0;
    ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1;
    ps3dk_prof_gpu_zone z;
    (void)ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z);
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_DOMAIN_FAULTED, "domain faulted (%u)",
          g_reason);
    CHECK(SW() == w, "word unchanged");
    for (uint32_t i = 1; i <= 16; i++) {
        ps3dk_prof__test_rsxcap_rec(i, &after);
        CHECK(!memcmp(&before[i - 1], &after, sizeof after), "no record written (%u)", i);
    }
    expect_open(4);
    finish_faulted(0);
}
static void c_slot_req_no_capability(void)
{
    setup_n(0, 1); /* not enabled */
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_NO_CAPABILITY, "no capability");
    CHECK(SW() == 0, "word untouched");
    expect_open(0);
    finish();
}
static void c_slot_req_busy(void)
{
    /* REQUESTED, ARMED and TERMINAL here; CLAIMING / ARMING / CLOSING at their step points below */
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && C2 == 0 && g_reason == RSXCAP_R_BUSY, "REQUESTED: busy");
    CHECK(FR(0) == 0, "arm");
    CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && C2 == 0 && g_reason == RSXCAP_R_BUSY, "ARMED: busy");
    CHECK(FR(0) == 0, "close");
    CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && C2 == 0 && g_reason == RSXCAP_R_BUSY, "TERMINAL: busy");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void act_req_busy(void)
{
    uint32_t c;
    CHECK(REQ(H[0], &c) == RSXCAP_REFUSED && c == 0 && g_reason == RSXCAP_R_BUSY, "busy at a step (%u)", g_reason);
}
static void c_slot_req_busy_steps(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(RT_AFTER_CLAIM, 1); /* ARMING */
    actor_start(&a, act_req_busy);
    CHECK(FR(0) == 0, "arm");
    actor_join(&a);
    step_arm(RT_AFTER_FREEZE, 1); /* CLOSING */
    actor_start(&a, act_req_busy);
    CHECK(FR(0) == 0, "close");
    actor_join(&a);
    CHECK(outcome_is(C1, RSXCAP_O_DONE, 0), "DONE");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_slot_cancel_before_arm(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0 && SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, C1, 0, 0), "IDLE(e)");
    CHECK(FR(0) == 0 && ST(SW()) == RSXCAP_IDLE, "no BEGIN");
    CHECK(Q(C1).outcome == RSXCAP_O_CANCELLED, "query: cancelled");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
static void c_slot_domain0(void)
{
    setup_n(1, 1);
    CHECK(REQ(0, &C1) == RSXCAP_ACCEPTED, "one domain: accepted");
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(C1, &r);
    CHECK(r.dom == H[0], "the resolved id is stored (%u)", r.dom);
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "cancel");
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_B;
    CHECK(ps3dk_prof_gpu_attach(CTX(1), 1024, g_slots, 70, &H[1]) == 0, "attach B");
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_A;
    uint64_t w = SW();
    CHECK(REQ(0, &C2) == RSXCAP_REFUSED && C2 == 0 && g_reason == RSXCAP_R_BAD_DOMAIN && SW() == w,
          "two domains: REFUSED(bad domain), cap 0");
    expect_open(0);
    finish_n(2, 0);
}
static void c_recycle(void)
{
    setup();
    arm_on(0, &C1);
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == PS3DK_PROF_EINVAL, "recycle refused until TERMINAL");
    CHECK(FR(0) == 0, "close");
    uint64_t w = SW();
    CHECK(RSXCAP_OUTCOME(w) == RSXCAP_O_DONE, "outcome bits in TERMINAL");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0 && SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, C1, 0, 0),
          "IDLE(e): outcome bits cleared");
    CHECK(Q(C1).outcome == RSXCAP_O_DONE, "the cached result");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED && C2 == C1 + 1, "next cap");
    CHECK(Q(C2).outcome == 0 && Q(C2).state == RSXCAP_REQUESTED, "the new claim inherits no outcome (carry 3)");
    CHECK(Q(C1).outcome == RSXCAP_O_DONE && Q(C1).cached == 1, "the old cap: its own cached result, not the new word");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_recycle_wrong_cap(void)
{
    setup();
    arm_on(0, &C1);
    CHECK(FR(0) == 0, "close");
    uint64_t w = SW();
    CHECK(ps3dk_prof__rsxcap_recycle(C1 + 1) == PS3DK_PROF_EINVAL && ps3dk_prof__rsxcap_cancel(C1 + 1) ==
          PS3DK_PROF_EINVAL && SW() == w, "another cap: refused, unchanged");
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == PS3DK_PROF_EINVAL && SW() == w, "cancel of TERMINAL: refused");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_cancel_stale(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED && ps3dk_prof__rsxcap_cancel(C1) == 0, "cancelled");
    uint64_t w = SW();
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == PS3DK_PROF_EINVAL && SW() == w, "stale cancel: refused");
    expect_open(0);
    finish();
}
static void c_two_conn_wire1(void)
{
    setup();
    uint32_t r;
    CHECK(ps3dk_prof__rsxcap_request(1, 1, H[0], &C1, &r) == RSXCAP_ACCEPTED, "conn 1 wire 1");
    CHECK(FR(0) == 0, "arm");
    draws(0, 3, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0 && ps3dk_prof__rsxcap_recycle(C1) == 0, "close + recycle");
    CHECK(ps3dk_prof__rsxcap_request(2, 1, H[0], &C2, &r) == RSXCAP_ACCEPTED && C2 == 2, "conn 2 wire 1: cap 2");
    CHECK(FR(0) == 0, "arm");
    draws(0, 4, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0, "close");
    CHECK(Q(1).conn == 1 && Q(2).conn == 2, "conns kept");
    expect_open(0);
    expect_cap(1, "FINALIZED", 3, 0);
    expect_cap(2, "FINALIZED", 4, 0);
    finish();
}
static void c_record_immutable(void)
{
    rsxcap_rec snap[4], now;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    ps3dk_prof__test_rsxcap_rec(C1, &snap[0]); /* after CLAIMING's release */
    CHECK(FR(0) == 0, "arm");
    ps3dk_prof__test_rsxcap_rec(C1, &snap[1]); /* after ARMED's release */
    CHECK(!memcmp(&snap[0], &snap[1], offsetof(rsxcap_rec, bound)), "agent fields unchanged at ARMED");
    draws(0, 4, RSXCAP_K_ARRAYS);
    ps3dk_prof__test_rsxcap_rec(C1, &now);
    CHECK(!memcmp(&snap[1], &now, RSXCAP_REC_IMMUTABLE_BYTES), "unchanged while ARMED (owner counters are private)");
    CHECK(FR(0) == 0, "close");
    ps3dk_prof__test_rsxcap_rec(C1, &snap[2]); /* after TERMINAL's release */
    CHECK(!memcmp(&snap[1], &snap[2], offsetof(rsxcap_rec, end_r)), "binding unchanged at TERMINAL");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0, "recycle");
    for (int k = 0; k < 3; k++) {
        uint32_t c;
        CHECK(REQ(H[0], &c) == RSXCAP_ACCEPTED, "claim %d", k);
        CHECK(FR(0) == 0 && FR(0) == 0 && ps3dk_prof__rsxcap_recycle(c) == 0, "cycle %d", k);
        ps3dk_prof__test_rsxcap_rec(C1, &now);
        CHECK(!memcmp(&snap[2], &now, RSXCAP_REC_IMMUTABLE_BYTES), "rec[%u] byte-identical after later caps", C1);
    }
    expect_open(0);
    for (uint32_t c = 1; c <= 4; c++)
        expect_cap(c, "FINALIZED", c == 1 ? 4 : 0, 0);
    finish();
}
static void c_terminal_record_immutable(void)
{
    rsxcap_rec t, now;
    setup();
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0, "close");
    ps3dk_prof__test_rsxcap_rec(C1, &t);
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0, "recycle");
    ps3dk_prof__test_rsxcap_rec(C1, &now);
    CHECK(!memcmp(&t, &now, RSXCAP_REC_IMMUTABLE_BYTES), "after recycle");
    for (uint32_t k = 2; k <= 16; k++) {
        uint32_t c;
        CHECK(REQ(H[0], &c) == RSXCAP_ACCEPTED && c == k, "cap %u", k);
        CHECK(ps3dk_prof__rsxcap_cancel(c) == 0, "cancel");
    }
    ps3dk_prof__test_rsxcap_rec(C1, &now);
    CHECK(!memcmp(&t, &now, RSXCAP_REC_IMMUTABLE_BYTES), "after 15 further caps");
    CHECK(Q(C1).outcome == RSXCAP_O_DONE, "query still names cap 1's own result");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 2, 0);
    finish();
}

/* ================================================================ PREPARE failures F1-F7 (R15) */
/* F1: gpu_frame from inside a GPU port callback (in_gpu): EBUSY before anything */
static ps3dk_prof_token g_cb_tok;
static int g_cb_frame_rc = -100;
static int32_t fcb_frame(gpp_ctx *c, uint32_t count)
{
    g_cb_frame_rc = ps3dk_prof_gpu_frame(H[0], g_cb_tok);
    c->end = ((fctx *)(void *)c)->buf + CBUF; /* the callback makes room */
    return fcb(c, count);
}
/* F2: gpu_frame after the shutdown claim (from the claim hook, before STOP and the seal) */
static uint64_t g_f2_w0, g_f2_w1;
static void f2_hook(void)
{
    g_f2_w0 = SW();
    g_cb_frame_rc = ps3dk_prof_gpu_frame(H[0], g_cb_tok);
    g_f2_w1 = SW();
}
static void c_prepare_fail(int f, int armed)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    if (armed)
        CHECK(FR(0) == 0 && ST(SW()) == RSXCAP_ARMED, "armed");
    uint64_t w = SW();
    int rc = 0, want = 0;
    ps3dk_prof_token t = tokn();
    switch (f) {
    case 1: {
        ps3dk_prof_gpu_zone z;
        g_cb_tok = t;
        FX[0].c.callback = fcb_frame;
        FX[0].c.end = FX[0].c.current + 3;
        (void)ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z);
        FX[0].c.callback = fcb;
        FX[0].c.end = FX[0].buf + CBUF;
        (void)ps3dk_prof_gpu_end(H[0], CTX(0), &z);
        rc = g_cb_frame_rc;
        want = PS3DK_PROF_EBUSY;
        break;
    }
    case 2:
        g_cb_tok = t;
        ps3dk_prof_test_hooks.after_shutdown_claim = f2_hook;
        drain(); /* the BEGIN drained while its owner thread is live */
        ps3dk_prof_thread_end();
        expect_open(0);
        if (armed)
            expect_capx(C1, "PARTIAL", "missing END", 0, 0);
        else
            expect_nocap(C1);
        CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
        CHECK(g_cb_frame_rc == PS3DK_PROF_ESTOPPED, "F2: ESTOPPED after the claim (%d)", g_cb_frame_rc);
        CHECK(g_f2_w0 == w && g_f2_w1 == w, "F2: no capture transition");
        fprintf(g_exp, "anomalies 0\n");
        fclose(g_exp);
        CHECK(take_capture() == 0, "capture");
        return;
    case 3:
        t.reserved = 1;
        rc = ps3dk_prof_gpu_frame(H[0], t);
        want = PS3DK_PROF_EINVAL;
        break;
    case 4:
        ps3dk_prof_test_hooks.p_lock_ops = 0;
        ps3dk_prof_test_hooks.p_lock_fail_at = 1;
        rc = ps3dk_prof_gpu_frame(H[0], t);
        ps3dk_prof_test_hooks.p_lock_fail_at = 0;
        want = PS3DK_PROF_ESTOPPED;
        break;
    case 5:
        t.frame += 1000; /* not a frame the thread began */
        rc = ps3dk_prof_gpu_frame(H[0], t);
        want = PS3DK_PROF_EINVAL;
        break;
    case 6: { /* no reclaimable entry: every entry retired from installation */
        extern void ps3dk_prof__test_gpu_set_tags(unsigned i, uint32_t tag);
        drain();
        ps3dk_prof_thread_end();
        CHECK(ps3dk_prof_thread_begin("render2") == 0, "a new incarnation");
        ps3dk_prof__test_gpu_set_tags(0, 0x3FFFFFFEu);
        rc = ps3dk_prof_gpu_frame(H[0], tokn());
        want = PS3DK_PROF_EBUSY;
        break;
    }
    case 7:
        ps3dk_prof_test_hooks.p_unlock_ops = 0;
        ps3dk_prof_test_hooks.p_unlock_fail_at = 1;
        rc = ps3dk_prof_gpu_frame(H[0], t);
        ps3dk_prof_test_hooks.p_unlock_fail_at = 0;
        want = PS3DK_PROF_ESTOPPED;
        break;
    }
    CHECK(rc == want, "F%d rc %d want %d", f, rc, want);
    CHECK(SW() == w, "F%d: no capture transition (%llx -> %llx)", f, (unsigned long long)w, (unsigned long long)SW());
    if (f == 4 || f == 7) { /* a terminal PAL fault (lock_failed): the capture fails closed */
        expect_open(1);
        finish_faulted(0);
        return;
    }
    if (f == 6) { /* every entry leased or retired: the capture stays as it was to the seal */
        expect_open(0);
        if (armed)
            expect_capx(C1, "PARTIAL", "missing END", 0, 0);
        else
            expect_nocap(C1);
        finish_faulted(0);
        return;
    }
    /* the control: the same call without the injection transitions */
    CHECK(FR(0) == 0 && SW() != w, "control: transitions");
    CHECK(armed ? outcome_is(C1, RSXCAP_O_DONE, 0) : ST(SW()) == RSXCAP_ARMED, "control outcome");
    if (!armed)
        CHECK(FR(0) == 0, "close");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}

/* ================================================================ admission refusals and descriptors */
static void c_arm_admission_refused(int armed)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    if (armed)
        CHECK(FR(0) == 0, "arm");
    /* a drainer anomaly: a label outside the window stops admission (sticky) */
    __atomic_store_n(&BK[0].lab[64], 0x7FFFFFF0u, __ATOMIC_SEQ_CST);
    CHECK(ps3dk_prof_drain() == 0, "drain sees the anomaly");
    __atomic_store_n(&BK[0].lab[64], 0u, __ATOMIC_SEQ_CST);
    CHECK(FR(0) == PS3DK_PROF_ESTOPPED, "ESTOPPED");
    CHECK(armed ? outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_DOMAIN_STOPPED)
                : outcome_is(C1, RSXCAP_O_REFUSED, RSXCAP_R_DOMAIN_STOPPED),
          "TERMINAL(%s, domain stopped)", armed ? "ABANDONED" : "REFUSED");
    expect_open(4);
    finish_faulted(0);
}
static void desc_case(int role, int exact)
{
    /* registered: 5 kinds, then BEGIN (6), END (7) */
    g_maxd = role == 0 ? (exact ? 7 : 5) : (exact ? 7 : 6);
    setup();
    CHECK(ps3dk_prof__test_rsxcap_capable(0) == 1, "capable");
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    CHECK(FR(0) == 0, "boundary");
    if (role == 0 && !exact) {
        CHECK(outcome_is(C1, RSXCAP_O_REFUSED, RSXCAP_R_DESCRIPTOR), "REFUSED(descriptor)");
        expect_open(0);
        expect_nocap(C1);
        finish();
        return;
    }
    CHECK(ST(SW()) == RSXCAP_ARMED, "armed");
    draws(0, 2, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0, "close");
    if (!exact) {
        CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_END_DESCRIPTOR), "ABANDONED(end descriptor)");
        expect_open(0);
        expect_capx(C1, "PARTIAL", "missing END", 2, 0);
    } else {
        CHECK(outcome_is(C1, RSXCAP_O_DONE, 0), "DONE (exact)");
        expect_open(0);
        expect_cap(C1, "FINALIZED", 2, 0);
    }
    finish();
}
static void c_end_lock_fault(void)
{
    setup();
    arm_on(0, &C1);
    ps3dk_prof_test_hooks.p_lock_ops = 0;
    ps3dk_prof_test_hooks.p_lock_fail_at = 2; /* gpu_frame's thread_mutex is 1, the END registration's desc_mutex 2 */
    CHECK(FR(0) == PS3DK_PROF_ESTOPPED, "ESTOPPED");
    ps3dk_prof_test_hooks.p_lock_fail_at = 0;
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "TERMINAL(ABANDONED, stopping)");
    expect_open(1);
    finish_faulted(0);
}
static void c_stop_before_cas(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    ps3dk_prof__test_rsxcap_stop();
    CHECK(FR(0) == 0, "frame");
    CHECK(outcome_is(C1, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING) && (SW() & RSXCAP_F_STOP), "TERMINAL(REFUSED, stopping)");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
static void c_claim_lost_to_reclaim(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request e");
    rsxcap_rec before;
    ps3dk_prof__test_rsxcap_rec(C1, &before);
    step_arm(RT_AFTER_SLOT_LOAD, 1);
    actor_start(&a, act_cancel_reclaim);
    CHECK(FR(0) == 0, "the frame (its claim CAS loses)");
    actor_join(&a);
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(C1, &r);
    CHECK(!memcmp(&before, &r, RSXCAP_REC_IMMUTABLE_BYTES), "rec[e] unchanged");
    ps3dk_prof__test_rsxcap_rec(C2, &r);
    CHECK(r.bound == 0 && r.conn == 1, "rec[e+1] holds agent fields only");
    CHECK(SW() == RSXCAP_WORD(RSXCAP_REQUESTED, 0, C2, 0, 0), "e+1 untouched: REQUESTED");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
static void c_inspect_then_lose(void)
{
    actor a;
    ps3dk_prof_gpu_stats s0, s1;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &s0) == 0, "stats");
    uint32_t users = ps3dk_prof__test_gpu_users(0);
    step_arm(RT_AFTER_SLOT_LOAD, 1);
    actor_start(&a, act_cancel_c1);
    CHECK(FR(0) == 0, "frame");
    actor_join(&a);
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &s1) == 0 && !memcmp(&s0, &s1, sizeof s0), "domain stats identical");
    CHECK(ps3dk_prof__test_gpu_users(0) == users, "pins identical");
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, C1, 0, 0), "IDLE(e), no BEGIN");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
static void c_stop_before_armed_pub(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(RT_BEFORE_ARMED_PUB, 1);
    actor_start(&a, act_stop);
    CHECK(FR(0) == 0, "frame");
    actor_join(&a);
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "TERMINAL(ABANDONED, stopping)");
    CHECK(Q(C1).begin_r.kind == PRG_R_EVENT, "the BEGIN was enqueued");
    expect_open(0);
    expect_capx(C1, "PARTIAL", "missing END", 0, 0);
    finish();
}
static void c_stop_between_cas_and_begin(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(RT_AFTER_CLAIM, 1);
    actor_start(&a, act_stop);
    CHECK(FR(0) == 0, "frame");
    actor_join(&a);
    CHECK(Q(C1).begin_r.kind == PRG_R_EVENT, "BEGIN emitted");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "then TERMINAL(ABANDONED, stopping)");
    expect_open(0);
    expect_capx(C1, "PARTIAL", "missing END", 0, 0);
    finish();
}
static void c_cancel_during_arming(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(RT_AFTER_CLAIM, 1);
    actor_start(&a, act_cancel_c1);
    CHECK(FR(0) == 0, "frame");
    actor_join(&a);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_ARMED, RSXCAP_F_CANCEL, C1, 0, 0), "ARMED+CANCEL published (flags kept)");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == PS3DK_PROF_EINVAL, "recycle refused until TERMINAL");
    draws(0, 2, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0 && outcome_is(C1, RSXCAP_O_DONE, 0), "closes normally");
    CHECK(SW() & RSXCAP_F_CANCEL, "CANCEL kept in TERMINAL");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0 && SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_CANCEL, C1, 0, 0),
          "recycle after TERMINAL keeps CANCEL");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 2, 0);
    finish();
}
static void receipt_case(int begin, int which)
{
    /* the BEGIN or END marker's receipt not EVENT: 0 LOST (FIFO), 1 SUPPRESSED-depth (MARK_DEPTH refusal), 2 REFUSED
     * (the domain's admission stopped would be ESTOPPED: here the MARK depth refusal stands for REFUSED) */
    ps3dk_prof_gpu_zone z[64];
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    if (!begin)
        CHECK(FR(0) == 0, "arm");
    if (which == 0) {
        FX[0].refuse = 1;
        FX[0].c.end = FX[0].c.current + 3;
    } else {
        for (int i = 0; i < 64; i++)
            CHECK(ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z[i]) == 0, "app zone %d", i);
    }
    CHECK(FR(0) == 0, "boundary");
    if (which == 0) {
        FX[0].refuse = 0;
        FX[0].c.end = FX[0].buf + CBUF;
    } else {
        for (int i = 64; i--;)
            (void)ps3dk_prof_gpu_end(H[0], CTX(0), &z[i]);
    }
    rsxcap_outcome o = Q(C1);
    const prg_receipt *r = begin ? &o.begin_r : &o.end_r;
    CHECK(which == 0 ? r->kind == PRG_R_LOST : r->kind == PRG_R_REFUSED, "receipt %u", r->kind);
    CHECK(begin ? outcome_is(C1, RSXCAP_O_REFUSED, RSXCAP_R_BEGIN_NOT_ENQUEUED)
                : outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_END_NOT_ENQUEUED),
          "outcome");
    expect_open(which == 0 ? 4 : 0);
    if (begin)
        expect_nocap(C1);
    else
        expect_capx(C1, "PARTIAL", "missing END", 0, 0);
    finish();
}

/* ================================================================ detach, stop, cancel */
static void c_detach_requested(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    expect_open(0);
    fflush_(&FX[0].c);
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0, "detach");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_DETACH), "TERMINAL(ABANDONED, detach)");
    CHECK(ps3dk_prof_gpu_release(H[0], NULL) == 0, "release");
    expect_nocap(C1);
    finish_n(0, 0);
}
static void c_detach_armed(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 3, RSXCAP_K_ARRAYS);
    expect_open(0);
    fflush_(&FX[0].c);
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0, "detach");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_DETACH), "TERMINAL(ABANDONED, detach), no END");
    CHECK(ps3dk_prof_gpu_release(H[0], NULL) == 0, "release");
    expect_capx(C1, "PARTIAL", "missing END", 3, 0);
    finish_n(0, 0);
}
static void c_domain_released_while_requested(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    fflush_(&FX[0].c);
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0 && ps3dk_prof_gpu_release(H[0], NULL) == 0, "detach+rel");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_DETACH), "settled by the detach");
    CHECK(ps3dk_prof_gpu_attach(CTX(0), 0, g_slots, 64, &H[0]) == 0 && H[0] == 1027, "reattach: a new id");
    uint64_t w = SW();
    CHECK(FR(0) == 0 && SW() == w, "the new id never matches the old cap");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
static void stop_armed(int how)
{
    setup();
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    ps3dk_prof__test_rsxcap_stop();
    uint64_t opened = g_draw_opened;
    expect_open(0);
    if (how == 0) {
        draws(0, 1, RSXCAP_K_ARRAYS);
        CHECK(g_draw_opened == opened, "no further draw instrumented");
    } else if (how == 1) {
        CHECK(FR(0) == 0, "frame");
    } else {
        fflush_(&FX[0].c);
        CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0, "detach");
    }
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "TERMINAL(ABANDONED, stopping), no END");
    expect_capx(C1, "PARTIAL", "missing END", 2, 0);
    if (how == 2) {
        CHECK(ps3dk_prof_gpu_release(H[0], NULL) == 0, "release");
        finish_n(0, 0);
    } else {
        finish();
    }
}
static void c_stop_no_owner(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    drain();
    ps3dk_prof__test_rsxcap_stop();
    CHECK(ST(SW()) == RSXCAP_ARMED && (SW() & RSXCAP_F_STOP), "stays ARMED+STOP");
    uint64_t w = SW();
    CHECK(Q(C1).outcome == RSXCAP_O_ARMED_STOPPED && SW() == w, "query: ARMED_STOPPED, nothing mutated");
    expect_open(0);
    expect_capx(C1, "PARTIAL", "missing END", 2, 0);
    /* shutdown with the capture still armed (no detach): the seal finalizes the domain */
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
static void c_stop_mid_zone(void)
{
    setup();
    arm_on(0, &C1);
    rsxcap_draw dr;
    wsnap s;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "a zone open");
    fflush_(&FX[0].c);
    wsnap_take(0, &s);
    dsnap d0 = dom_snap(0);
    ps3dk_prof__test_rsxcap_stop(); /* the stopper's flag alone: the core is not sealed */
    ps3dk_prof__rsxcap_draw_end(&dr);
    CHECK(wsnap_same(0, &s), "R38: STOP observed before the END: no GPU word");
    CHECK(dom_same(0, &d0, -1), "no seq or ticket consumed; the zone popped (the stack stays exact)");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "settled TERMINAL(ABANDONED, stopping), no END");
    ps3dk_prof_gpu_zone z;
    CHECK(ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z) == 0 && ps3dk_prof_gpu_end(H[0], CTX(0), &z) == 0,
          "an app zone afterwards pairs normally (the stack is exact)");
    expect_open(4); /* the abandoned draw's BEGIN stays unpaired in the file: the extension is not clean */
    expect_capx(C1, "PARTIAL", "missing END", 0, 0);
    finish();
}
static void c_cancel_armed(void)
{
    setup();
    arm_on(0, &C1);
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0 && (SW() & RSXCAP_F_CANCEL), "CANCEL set");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == PS3DK_PROF_EINVAL, "recycle refused");
    draws(0, 2, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0 && outcome_is(C1, RSXCAP_O_DONE, 0), "DONE");
    CHECK(Q(C1).conn == 1, "the original conn");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 2, 0);
    finish();
}
static void c_cancel_closing(void)
{
    actor a;
    setup();
    arm_on(0, &C1);
    step_arm(RT_AFTER_FREEZE, 1);
    actor_start(&a, act_cancel_c1);
    CHECK(FR(0) == 0, "close");
    actor_join(&a);
    CHECK(outcome_is(C1, RSXCAP_O_DONE, 0) && (SW() & RSXCAP_F_CANCEL), "flag kept, outcome by END");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_cancel_armed_recycle_next(void)
{
    setup();
    arm_on(0, &C1);
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "cancel");
    CHECK(FR(0) == 0 && SW() == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_CANCEL, C1, RSXCAP_O_DONE, 0), "TERMINAL+CANCEL");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0 && SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_CANCEL, C1, 0, 0),
          "IDLE+CANCEL");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED && SW() == RSXCAP_WORD(RSXCAP_REQUESTED, 0, C2, 0, 0),
          "the next claim drops CANCEL");
    CHECK(FR(0) == 0 && SW() == RSXCAP_WORD(RSXCAP_ARMED, 0, C2, 0, 0), "arms without CANCEL");
    draws(0, 3, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0 && outcome_is(C2, RSXCAP_O_DONE, 0), "FINALIZES");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    expect_cap(C2, "FINALIZED", 3, 0);
    finish();
}
static void c_shutdown_sets_stop(void)
{
    setup();
    CHECK(ps3dk_prof__rsxcap_cancel(1) == PS3DK_PROF_EINVAL, "nothing armed");
    expect_open(0);
    fflush_(&FX[0].c);
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0 && ps3dk_prof_gpu_release(H[0], NULL) == 0, "cycle");
    ps3dk_prof_thread_end();
    CHECK((SW() & RSXCAP_F_STOP) == 0, "no STOP before shutdown");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    CHECK(SW() & RSXCAP_F_STOP, "STOP set by the shutdown (IDLE included)");
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
static void c_stop_idle_then_request(void)
{
    setup();
    ps3dk_prof__test_rsxcap_stop();
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, 0, 0, 0), "IDLE+STOP");
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_STOPPING, "REFUSED(stopping), cap 0");
    expect_open(0);
    finish();
}
static void c_stop_idle_vs_request(void)
{
    actor a;
    setup();
    step_arm(AG_BEFORE_CLAIM_CAS, 1);
    actor_start(&a, act_stop);
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_STOPPING, "validated, then STOP: refused");
    actor_join(&a);
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(1, &r);
    CHECK(r.conn == 0 && r.dom == 0, "no record written");
    expect_open(0);
    finish();
}
static void stop_claiming(int point)
{
    actor a;
    setup();
    step_arm(point, 1);
    actor_start(&a, act_stop);
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 1 && g_reason == RSXCAP_R_STOPPING, "REFUSED(stopping), cap e");
    actor_join(&a);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_STOP, 1, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING),
          "TERMINAL(e)+STOP, never stranded");
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(1, &r);
    CHECK(r.conn == 1 && r.dom == H[0], "fields written");
    CHECK(ps3dk_prof__rsxcap_recycle(1) == 0 && SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, 1, 0, 0),
          "recycle keeps STOP");
    CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && C2 == 0, "every later request refused");
    expect_open(0);
    expect_nocap(1);
    finish();
}
static void c_stop_vs_cancel(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(AG_CANCEL_LOAD, 1);
    actor_start(&a, act_stop);
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "cancel retries");
    actor_join(&a);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, C1, 0, 0), "IDLE(e)+STOP");
    expect_open(0);
    finish();
}
static void c_stop_vs_recycle(void)
{
    actor a;
    setup();
    arm_on(0, &C1);
    CHECK(FR(0) == 0, "close");
    step_arm(AG_BEFORE_RECYCLE, 1);
    actor_start(&a, act_stop);
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0, "recycle retries");
    actor_join(&a);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, C1, 0, 0), "IDLE(e)+STOP");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void stop_new_epoch(int variant)
{
    actor a;
    setup();
    arm_on(0, &C1);
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0 && FR(0) == 0 && ps3dk_prof__rsxcap_recycle(C1) == 0, "IDLE+CANCEL");
    if (variant == 0) {
        step_arm(AG_BEFORE_CLAIM_CAS, 1);
        actor_start(&a, act_stop);
        CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && C2 == 0 && g_reason == RSXCAP_R_STOPPING, "refused, cap 0");
        actor_join(&a);
        CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_CANCEL | RSXCAP_F_STOP, C1, 0, 0), "IDLE+CANCEL+STOP");
    } else {
        step_arm(AG_CLAIMING, 1);
        actor_start(&a, act_stop);
        CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && C2 == C1 + 1 && g_reason == RSXCAP_R_STOPPING, "refused, cap e+1");
        actor_join(&a);
        CHECK(SW() == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_STOP, C1 + 1, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING),
              "TERMINAL(e+1)+STOP");
    }
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_stop_after_publication(int after_check)
{
    actor a;
    setup();
    step_arm(after_check ? 0 : AG_BEFORE_CHECK, 1);
    if (!after_check)
        actor_start(&a, act_stop);
    int rc = REQ(H[0], &C1);
    if (!after_check) {
        actor_join(&a);
        CHECK(rc == RSXCAP_REFUSED && C1 == 1 && g_reason == RSXCAP_R_STOPPING, "the check withdraws: cap e");
        CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, 1, 0, 0), "IDLE(e)+STOP");
    } else {
        CHECK(rc == RSXCAP_ACCEPTED, "ACCEPTED");
        ps3dk_prof__test_rsxcap_stop();
        CHECK(SW() == RSXCAP_WORD(RSXCAP_REQUESTED, RSXCAP_F_STOP, 1, 0, 0), "REQUESTED+STOP until the next query");
        rsxcap_outcome o = Q(1);
        CHECK(o.outcome == RSXCAP_O_WITHDRAWN && o.reason == RSXCAP_R_STOPPING, "query withdraws");
        CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, 1, 0, 0), "IDLE(e)+STOP");
    }
    CHECK(Q(1).outcome == RSXCAP_O_WITHDRAWN, "WITHDRAWN(stopping)");
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(1, &r);
    CHECK(r.bound == 0 && r.frozen == 0, "rec unchanged by the service");
    expect_open(0);
    expect_nocap(1);
    finish();
}
static void c_stop_withdraw_vs_owner_after_stop(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    ps3dk_prof__test_rsxcap_stop();
    CHECK(FR(0) == 0, "owner first");
    CHECK(outcome_is(C1, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING), "TERMINAL(REFUSED, stopping) (R18)");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
static void c_query_armed_stop_no_mutation(void)
{
    rsxcap_rec a, b;
    setup();
    arm_on(0, &C1);
    ps3dk_prof__test_rsxcap_stop();
    uint64_t w = SW();
    ps3dk_prof__test_rsxcap_rec(C1, &a);
    for (int i = 0; i < 3; i++)
        CHECK(Q(C1).outcome == RSXCAP_O_ARMED_STOPPED, "ARMED_STOPPED");
    ps3dk_prof__test_rsxcap_rec(C1, &b);
    CHECK(SW() == w && !memcmp(&a, &b, sizeof a), "nothing mutated");
    CHECK(FR(0) == 0, "the owner settles");
    expect_open(0);
    expect_capx(C1, "PARTIAL", "missing END", 0, 0);
    finish();
}
static void c_stop_after_arming(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(RT_AFTER_DESC, 1);
    actor_start(&a, act_stop);
    CHECK(FR(0) == 0, "frame");
    actor_join(&a);
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "ARMING+STOP -> TERMINAL(ABANDONED, stopping)");
    expect_open(0);
    expect_capx(C1, "PARTIAL", "missing END", 0, 0);
    finish();
}
static void act_stop_then_withdraw(void)
{
    rsxcap_outcome o;
    ps3dk_prof__test_rsxcap_stop();
    (void)ps3dk_prof__rsxcap_query(C1, &o); /* the agent's service wins */
}
static void stop_stale_owner(int agent_wins)
{
    actor a;
    rsxcap_rec r0, r1;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    ps3dk_prof__test_rsxcap_rec(C1, &r0);
    step_arm(RT_AFTER_SLOT_LOAD, 1);
    actor_start(&a, agent_wins ? act_stop_then_withdraw : act_stop);
    CHECK(FR(0) == 0, "frame");
    actor_join(&a);
    if (!agent_wins) {
        CHECK(outcome_is(C1, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING), "owner's STOP settlement wins");
        rsxcap_outcome o;
        CHECK(ps3dk_prof__rsxcap_query(C1, &o) == 0 && o.state == RSXCAP_TERMINAL, "agent's later service: no write");
    } else {
        CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, C1, 0, 0), "the agent's withdrawal wins");
        CHECK(Q(C1).outcome == RSXCAP_O_WITHDRAWN, "WITHDRAWN(stopping)");
    }
    ps3dk_prof__test_rsxcap_rec(C1, &r1);
    CHECK(!memcmp(&r0, &r1, RSXCAP_REC_IMMUTABLE_BYTES), "rec bytes unchanged by both");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
/* the four close cutoffs (R83) */
static void stop_cut(int point)
{
    actor a;
    setup();
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    step_arm(point, 1);
    actor_start(&a, act_stop);
    CHECK(FR(0) == 0, "boundary");
    actor_join(&a);
    if (point == RT_BEFORE_CLOSING) {
        CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "before the CAS: ABANDONED(stopping), no END");
        expect_open(0);
        expect_capx(C1, "PARTIAL", "missing END", 2, 0);
    } else {
        CHECK(outcome_is(C1, RSXCAP_O_DONE, 0) && (SW() & RSXCAP_F_STOP), "after the CAS: DONE, STOP kept");
        expect_open(0);
        expect_cap(C1, "FINALIZED", 2, 0);
    }
    finish();
}
static void c_stale_owner_after_reclaim(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request e");
    step_arm(RT_AFTER_SLOT_LOAD, 1);
    actor_start(&a, act_cancel_reclaim);
    CHECK(FR(0) == 0, "the stale owner's CAS (epoch e) fails");
    actor_join(&a);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_REQUESTED, 0, C2, 0, 0), "e+1 untouched");
    CHECK(FR(0) == 0 && ST(SW()) == RSXCAP_ARMED && EP(SW()) == C2, "a fresh call arms e+1");
    CHECK(FR(0) == 0, "close");
    expect_open(0);
    expect_nocap(C1);
    expect_cap(C2, "FINALIZED", 0, 0);
    finish();
}
static void c_race_arm_cancel(int cancel_first)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(cancel_first ? RT_AFTER_SLOT_LOAD : RT_AFTER_CLAIM, 1);
    actor_start(&a, act_cancel_c1);
    CHECK(FR(0) == 0, "frame");
    actor_join(&a);
    if (cancel_first) {
        CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, C1, 0, 0), "cancel first: IDLE, no BEGIN");
        expect_open(0);
        expect_nocap(C1);
    } else {
        CHECK(SW() == RSXCAP_WORD(RSXCAP_ARMED, RSXCAP_F_CANCEL, C1, 0, 0), "arm first: ARMED+CANCEL");
        CHECK(FR(0) == 0, "close");
        expect_open(0);
        expect_cap(C1, "FINALIZED", 0, 0);
    }
    finish();
}

/* ================================================================ detach / fault vs publication (R78) */
/* The owner on a worker thread O (not the designated drain thread: its detach waits by sleeping, never by an inline
 * drain that would need the extension mutex an agent may hold), main the designated drainer and controller, the
 * agent on its own thread.  O runs one command at a time; main drains while it waits for O. */
static atomic_int o_seq, o_done, o_quit, o_rc;
static void (*volatile o_fn)(void);
static pthread_t o_thr;
static void *owner_loop(void *a)
{
    (void)a;
    if (ps3dk_prof_thread_begin("render") != 0)
        return NULL;
    for (;;) {
        SPIN_UNTIL(atomic_load(&o_seq) != atomic_load(&o_done) || atomic_load(&o_quit), "owner idle");
        if (atomic_load(&o_quit))
            break;
        o_fn();
        atomic_fetch_add(&o_done, 1);
    }
    ps3dk_prof_thread_end();
    return NULL;
}
static void own_post(void (*fn)(void))
{
    o_fn = fn;
    atomic_fetch_add(&o_seq, 1);
}
static void own_wait(void) /* drains while O works (bounded) */
{
    long i;
    for (i = 0; atomic_load(&o_done) != atomic_load(&o_seq); i++) {
        if (i >= 1000000)
            harness_stall("waiting for the owner");
        (void)ps3dk_prof_drain();
        sched_yield();
    }
}
static void own_do(void (*fn)(void))
{
    own_post(fn);
    own_wait();
}
static void o_attach_a(void) { atomic_store(&o_rc, ps3dk_prof_gpu_attach(CTX(0), 0, g_slots, 64, &H[0])); }
static void o_attach_b(void)
{
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_B;
    atomic_store(&o_rc, ps3dk_prof_gpu_attach(CTX(1), 1024, g_slots, 70, &H[1]));
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV_A;
}
static void o_detach_short(void) { atomic_store(&o_rc, ps3dk_prof_gpu_detach(H[0], CTX(0), 2000)); }
static void o_detach(void) { atomic_store(&o_rc, ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000)); }
static void o_release(void) { atomic_store(&o_rc, ps3dk_prof_gpu_release(H[0], NULL)); }
static void o_detach_release_b(void)
{
    int rc = ps3dk_prof_gpu_detach(H[1], CTX(1), 3000000);
    atomic_store(&o_rc, rc == 0 ? ps3dk_prof_gpu_release(H[1], NULL) : rc);
}
static void o_fault_a(void)
{
    ps3dk_prof_gpu_zone z;
    ps3dk_prof_test_hooks.gpu_snap_ops = 0;
    ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1; /* the zone's publication faults the domain */
    atomic_store(&o_rc, ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z));
}
static void osetup(int ndom)
{
    fx_init(0, 0, 0, g_slots, 64);
    fx_init(1, 1, 1024, g_slots, 70);
    CHECK(start(2, g_slots) == 0, "start");
    CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_OK, "enable");
    pthread_create(&o_thr, NULL, owner_loop, NULL);
    own_do(o_attach_a);
    CHECK(atomic_load(&o_rc) == 0 && H[0] == 1026, "attach A on O");
    if (ndom == 2) {
        own_do(o_attach_b);
        CHECK(atomic_load(&o_rc) == 0, "attach B on O");
    }
}
static void ofinish(int detach_a, int detach_b)
{
    if (detach_a) {
        own_do(o_detach);
        CHECK(atomic_load(&o_rc) == 0, "detach A (%d)", atomic_load(&o_rc));
        own_do(o_release);
        CHECK(atomic_load(&o_rc) == 0, "release A");
    }
    if (detach_b) {
        own_do(o_detach_release_b);
        CHECK(atomic_load(&o_rc) == 0, "detach+release B");
    }
    atomic_store(&o_quit, 1);
    pthread_join(o_thr, NULL);
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
/* the agent on its own thread */
typedef struct agent {
    pthread_t t;
    uint32_t dom, cap, reason;
    int rc;
} agent;
static void *agent_main(void *a)
{
    agent *x = a;
    x->rc = ps3dk_prof__rsxcap_request(1, 1, x->dom, &x->cap, &x->reason);
    return NULL;
}
static void agent_start(agent *x, uint32_t dom)
{
    x->dom = dom;
    pthread_create(&x->t, NULL, agent_main, x);
}
static void agent_join(agent *x) { pthread_join(x->t, NULL); }

/* detach (its DETACHING CAS) while the agent holds the mutex after its scan / in CLAIMING: the check withdraws */
static void detach_vs_claim(int point, int domain0)
{
    agent ag;
    osetup(1);
    if (!point) { /* the control: no detach */
        agent_start(&ag, domain0 ? 0 : H[0]);
        agent_join(&ag);
        CHECK(ag.rc == RSXCAP_ACCEPTED, "control: ACCEPTED");
        expect_open(0);
        CHECK(ps3dk_prof__rsxcap_cancel(ag.cap) == 0, "cancel");
        ofinish(1, 0);
        return;
    }
    step_arm(point, 1);
    agent_start(&ag, domain0 ? 0 : H[0]);
    step_wait_paused(); /* the agent holds the extension mutex */
    own_post(o_detach_short); /* the DETACHING CAS lands; the finalization cannot (the mutex): ETIMEDOUT */
    SPIN_UNTIL(atomic_load(&o_done) == atomic_load(&o_seq), "the owner's short detach");
    CHECK(atomic_load(&o_rc) == PS3DK_PROF_ETIMEDOUT, "detach pending, returned without waiting on the agent (%d)",
          atomic_load(&o_rc));
    CHECK(ST(SW()) == (point == AG_CLAIMING ? RSXCAP_CLAIMING : RSXCAP_IDLE), "detach left the slot untouched");
    step_go();
    agent_join(&ag);
    CHECK(ag.rc == RSXCAP_REFUSED && ag.cap == 1 && ag.reason == RSXCAP_R_DOMAIN_DETACHED,
          "the check withdraws: REFUSED(domain detached), cap e (rc %d cap %u reason %u)", ag.rc, ag.cap, ag.reason);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, 1, 0, 0), "IDLE(e): never REQUESTED after the owner is gone");
    expect_open(0);
    expect_nocap(1);
    ofinish(1, 0);
}
/* (a) the agent's check after detach's DETACHING CAS and before its slot load: the agent withdraws, detach sees IDLE */
static void c_detach_vs_publication_a(void)
{
    agent ag;
    osetup(1);
    step_arm2(0, AG_AFTER_PUBLICATION, 1);
    agent_start(&ag, H[0]);
    step_wait_paused2(0);                /* REQUESTED published; the agent holds the mutex */
    step_arm2(1, DT_BEFORE_SETTLE, 1);
    own_post(o_detach_short);
    step_wait_paused2(1);                /* the DETACHING CAS landed, the settle's load not yet */
    step_go2(0);
    agent_join(&ag);
    CHECK(ag.rc == RSXCAP_REFUSED && ag.cap == 1 && ag.reason == RSXCAP_R_DOMAIN_DETACHED, "the agent withdraws");
    step_go2(1);
    SPIN_UNTIL(atomic_load(&o_done) == atomic_load(&o_seq), "the owner's detach");
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, 1, 0, 0), "detach's later load saw IDLE and did nothing");
    expect_open(0);
    expect_nocap(1);
    ofinish(1, 0);
}
/* (b) detach's slot load after the publication and before the agent's check: detach settles, the agent's CAS fails */
static void c_detach_vs_publication_b(void)
{
    agent ag;
    osetup(1);
    step_arm(AG_BEFORE_CHECK, 1);
    agent_start(&ag, H[0]);
    step_wait_paused(); /* unlocked, still pinned, before the check */
    own_do(o_detach);
    CHECK(atomic_load(&o_rc) == 0, "detach completes");
    CHECK(RSXCAP_STATE(SW()) == RSXCAP_TERMINAL && RSXCAP_REASON(SW()) == RSXCAP_R_DETACH, "TERMINAL(ABANDONED, detach)");
    uint64_t w = SW();
    step_go();
    agent_join(&ag);
    CHECK(ag.rc == RSXCAP_ACCEPTED && SW() == w, "the agent's withdrawal does not land; no write");
    CHECK(outcome_is(1, RSXCAP_O_ABANDONED, RSXCAP_R_DETACH), "query reports the terminal outcome");
    own_do(o_release);
    expect_open(0);
    expect_nocap(1);
    ofinish(0, 0);
}
/* both paused just before their CAS, released in both orders (two domains, one run each) */
static void c_detach_vs_publication_both(void)
{
    osetup(2);
    for (int order = 0; order < 2; order++) {
        agent ag;
        uint32_t d = order ? H[1] : H[0];
        step_arm2(0, AG_AFTER_PUBLICATION, 1);
        agent_start(&ag, d);
        step_wait_paused2(0);
        step_arm2(1, DT_AFTER_LOAD, 1);
        own_post(order ? o_detach_release_b : o_detach_short);
        step_wait_paused2(1); /* detach loaded REQUESTED */
        step_arm2(0, AG_WITHDRAW_LOAD, 1);
        step_go2(0);
        step_wait_paused2(0); /* the agent saw DETACHING and loaded REQUESTED */
        uint32_t cap = RSXCAP_EPOCH(SW());
        if (order == 0) {
            step_go2(0);
            agent_join(&ag);
            step_go2(1);
        } else {
            step_go2(1);
            SPIN_UNTIL(RSXCAP_STATE(SW()) == RSXCAP_TERMINAL, "detach's CAS");
            step_go2(0);
            agent_join(&ag);
        }
        own_wait();
        uint64_t w = SW();
        if (order == 0) {
            CHECK(w == RSXCAP_WORD(RSXCAP_IDLE, 0, cap, 0, 0) && ag.rc == RSXCAP_REFUSED, "agent first: IDLE, WITHDRAWN");
            CHECK(Q(cap).outcome == RSXCAP_O_WITHDRAWN, "query consistent");
        } else {
            CHECK(RSXCAP_STATE(w) == RSXCAP_TERMINAL && ag.rc == RSXCAP_ACCEPTED, "detach first: TERMINAL");
            CHECK(outcome_is(cap, RSXCAP_O_ABANDONED, RSXCAP_R_DETACH), "query consistent");
            CHECK(ps3dk_prof__rsxcap_recycle(cap) == 0, "recycle");
        }
    }
    expect_open(0);
    ofinish(1, 0);
}
/* R5: the agent paused after its unlock with D pinned; D detaches, releases, a new attach lands in another slot */
static void c_no_stale_slot_reuse(void)
{
    agent ag;
    osetup(1);
    step_arm(AG_BEFORE_CHECK, 1);
    agent_start(&ag, H[0]);
    step_wait_paused();
    own_do(o_detach);
    own_do(o_release);
    CHECK(atomic_load(&o_rc) == 0, "released");
    CHECK(ps3dk_prof__test_gpu_users(0) >= 1, "the agent's pin is still held");
    own_do(o_attach_b);
    CHECK(atomic_load(&o_rc) == 0 && H[1] == 1027 && ps3dk_prof__test_gpu_state(1) == 1, "the new attach: slot 1");
    step_go();
    agent_join(&ag);
    CHECK(RSXCAP_STATE(SW()) == RSXCAP_TERMINAL || SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, 1, 0, 0), "settled");
    uint64_t w = SW();
    H[0] = H[1];
    expect_open(0);
    expect_nocap(1);
    (void)w;
    ofinish(0, 1);
}
static void c_fault_vs_publication(void)
{
    agent ag;
    osetup(1);
    step_arm(AG_BEFORE_CHECK, 1);
    agent_start(&ag, H[0]);
    step_wait_paused();
    /* no drain while the injected call runs: the snap-fault seam's op counter is a plain test-hook counter */
    own_post(o_fault_a);
    SPIN_UNTIL(atomic_load(&o_done) == atomic_load(&o_seq), "the owner's faulting call");
    step_go();
    agent_join(&ag);
    CHECK(ag.rc == RSXCAP_REFUSED && ag.cap == 1 && ag.reason == RSXCAP_R_DOMAIN_FAULTED,
          "REFUSED(domain faulted), cap e (rc %d reason %u)", ag.rc, ag.reason);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, 1, 0, 0), "IDLE(e)");
    expect_open(4);
    expect_nocap(1);
    atomic_store(&o_quit, 1);
    pthread_join(o_thr, NULL);
    (void)ps3dk_prof_shutdown();
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
/* R90: a second attach while the agent holds the mutex after its scan contends and lands after the publication */
static void c_domain0_attach_between_scan_and_claim(void)
{
    agent ag;
    osetup(1);
    step_arm2(0, AG_AFTER_SCAN, 1);
    step_arm2(1, AG_AFTER_PUBLICATION, 1);
    agent_start(&ag, 0);
    step_wait_paused2(0);
    uint64_t c0 = __atomic_load_n(&ps3dk_prof_test_hooks.ext_lock_contended, __ATOMIC_SEQ_CST);
    own_post(o_attach_b);
    SPIN_UNTIL(__atomic_load_n(&ps3dk_prof_test_hooks.ext_lock_contended, __ATOMIC_SEQ_CST) > c0 ||
                   atomic_load(&o_done) == atomic_load(&o_seq),
               "contended or attach returned");
    CHECK(atomic_load(&o_done) != atomic_load(&o_seq), "the attach found the mutex held and waits");
    step_go2(0);
    step_wait_paused2(1);
    CHECK(ps3dk_prof__test_rsxcap_eligible() == 1, "at the publication exactly one capable domain is published");
    step_go2(1);
    agent_join(&ag);
    SPIN_UNTIL(atomic_load(&o_done) == atomic_load(&o_seq), "the second attach");
    CHECK(ag.rc == RSXCAP_ACCEPTED, "ACCEPTED (rc %d reason %u)", ag.rc, ag.reason);
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(ag.cap, &r);
    CHECK(r.dom == H[0], "names the first domain");
    CHECK(ps3dk_prof__rsxcap_cancel(ag.cap) == 0, "cancel");
    expect_open(0);
    ofinish(1, 1);
}
static void c_domain0_vs_attach_a(void)
{
    agent ag;
    osetup(2); /* the second attach completed before the agent takes the lock */
    agent_start(&ag, 0);
    agent_join(&ag);
    CHECK(ag.rc == RSXCAP_REFUSED && ag.cap == 0 && ag.reason == RSXCAP_R_BAD_DOMAIN, "REFUSED(bad domain), cap 0");
    expect_open(0);
    ofinish(1, 1);
}
static void o_detach_b_only(void) { atomic_store(&o_rc, ps3dk_prof_gpu_detach(H[1], CTX(1), 3000000)); }
static void o_release_b(void) { atomic_store(&o_rc, ps3dk_prof_gpu_release(H[1], NULL)); }
/* B detached and retired, not released: excluded before and after its release, both lock orders: ACCEPTED A */
static void c_domain0_release_vs_resolution(void)
{
    osetup(2);
    own_do(o_detach_b_only);
    CHECK(atomic_load(&o_rc) == 0, "B retired");
    for (int order = 0; order < 2; order++) {
        agent ag;
        if (order == 0) { /* the agent first: B's release contends with its hold */
            step_arm(AG_AFTER_SCAN, 1);
            agent_start(&ag, 0);
            step_wait_paused();
            own_post(o_release_b);
            step_go();
            agent_join(&ag);
            own_wait();
        } else { /* the release first */
            agent_start(&ag, 0);
            agent_join(&ag);
        }
        CHECK(ag.rc == RSXCAP_ACCEPTED, "order %d: ACCEPTED (reason %u)", order, ag.reason);
        rsxcap_rec r;
        ps3dk_prof__test_rsxcap_rec(ag.cap, &r);
        CHECK(r.dom == H[0], "names A");
        CHECK(ps3dk_prof__rsxcap_cancel(ag.cap) == 0, "cancel");
    }
    expect_open(0);
    ofinish(1, 0);
}
/* enable against attach, forced at the mutex in both orders */
static void c_enable_vs_attach(int enable_first)
{
    fx_init(0, 0, 0, g_slots, 64);
    CHECK(start(2, g_slots) == 0, "start");
    pthread_create(&o_thr, NULL, owner_loop, NULL);
    if (enable_first) {
        CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_OK, "enable first");
        own_do(o_attach_a);
        CHECK(atomic_load(&o_rc) == 0 && ps3dk_prof__test_rsxcap_capable(0) == 1, "a capable domain");
    } else {
        own_do(o_attach_a);
        CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_EBUSY, "attach first: EBUSY");
        CHECK(ps3dk_prof__test_rsxcap_capable(0) == 0, "a non-capable domain");
    }
    expect_open(0);
    ofinish(1, 0);
}

/* ================================================================ counters (2.5) */
static void c_draw_basic(void)
{
    setup();
    arm_on(0, &C1);
    for (uint32_t k = 0; k < RSXCAP_KINDS; k++)
        draws(0, 2, k);
    CHECK(FR(0) == 0 && outcome_is(C1, RSXCAP_O_DONE, 0), "DONE");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.hooks == 10 && o.end.c.ev == 20, "10 draws, 20 receipts");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 10, 0);
    finish();
}
static void sat16_case(int immediate)
{
    setup_n(1, 2);
    arm_on(0, &C1);
    for (uint32_t i = 0; i < 65535; i++) {
        if (immediate)
            ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_IMMEDIATE);
        else {
            rsxcap_draw dr;
            (void)ps3dk_prof__rsxcap_draw_begin(CTX(1), RSXCAP_K_ARRAYS, &dr);
        }
    }
    CHECK(FR(0) == 0, "close at 65535");
    rsxcap_outcome o = Q(C1);
    CHECK(immediate ? (o.end.immediate == 65535 && o.end.immediate_sat == 0)
                    : (o.end.other_ctx == 65535 && o.end.other_ctx_sat == 0),
          "65535: sat 0");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0 && REQ(H[0], &C2) == RSXCAP_ACCEPTED && FR(0) == 0, "cap 2 armed");
    for (uint32_t i = 0; i < 65536; i++) {
        if (immediate)
            ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_IMMEDIATE);
        else {
            rsxcap_draw dr;
            (void)ps3dk_prof__rsxcap_draw_begin(CTX(1), RSXCAP_K_ARRAYS, &dr);
        }
    }
    CHECK(FR(0) == 0, "close at 65536");
    o = Q(C2);
    CHECK(immediate ? (o.end.immediate == 65535 && o.end.immediate_sat == 1)
                    : (o.end.other_ctx == 65535 && o.end.other_ctx_sat == 1),
          "65536: value 65535, sat 1");
    expect_open(0);
    if (immediate) {
        expect_capx(C1, "PARTIAL", "65535 immediate-mode", 0, 0);
        expect_capx(C2, "PARTIAL", "65535+ (saturated) immediate-mode", 0, 0);
    } else {
        expect_cap(C1, "FINALIZED", 0, 0);
        expect_cap(C2, "FINALIZED", 0, 0);
    }
    finish_n(2, 0);
}
static void c_sat_five(void)
{
    setup();
    for (uint32_t which = 0; which < 5; which++) {
        uint32_t c;
        CHECK(REQ(H[0], &c) == RSXCAP_ACCEPTED && FR(0) == 0, "arm %u", which);
        if (which == RSXCAP_C_MISMATCH) {
            /* mismatch is incremented only by draw_abandon (a fault): its saturation is the side_inc rule shared with
             * the other four; here it is driven through count() */
        }
        for (uint32_t i = 0; i < 262144; i++)
            ps3dk_prof__rsxcap_count(CTX(0), which);
        CHECK(FR(0) == 0, "close at 262144");
        rsxcap_outcome o = Q(c);
        const uint32_t *v = &o.end.unsafe_draws;
        CHECK(v[which] == 262144 && o.end.counts_sat == 0, "%u: 262144 sat-free", which);
        CHECK(ps3dk_prof__rsxcap_recycle(c) == 0, "recycle");
        CHECK(REQ(H[0], &c) == RSXCAP_ACCEPTED && FR(0) == 0, "arm again");
        for (uint32_t i = 0; i < 262145; i++)
            ps3dk_prof__rsxcap_count(CTX(0), which);
        CHECK(FR(0) == 0, "close at 262145");
        o = Q(c);
        v = &o.end.unsafe_draws;
        CHECK(v[which] == 262144 && o.end.counts_sat == 1, "%u: 262145 latches counts_sat", which);
        for (uint32_t k = 0; k < 5; k++)
            if (k != which)
                CHECK(v[k] == 0, "the other four unchanged");
        CHECK(ps3dk_prof__rsxcap_recycle(c) == 0, "recycle");
    }
    expect_open(0);
    {
        static const char *word[5] = {"262144 draw(s) through Unsafe calls", "262144 draw(s) not instrumented (no space",
                                      "262144 reentrant draw(s)", "262144 invalid draw call(s)",
                                      "262144 draw(s) emitted a different word count"};
        for (uint32_t k = 0; k < 10; k++) {
            expect_capx(k + 1, "PARTIAL", word[k / 2], 0, 0);
            if (k % 2)
                expect_capx_has(k + 1, "a side counter saturated (counts_sat=1)");
        }
    }
    finish();
}
static void cap_case(uint32_t n)
{
    g_slots = 1024;
    setup();
    arm_on(0, &C1);
    draws(0, n, RSXCAP_K_ARRAYS);
    drain();
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.hooks == 262144 && o.end.capped == (n > 262144), "hooks 262144, capped %d", n > 262144);
    CHECK(o.end.block == 63, "END block 63");
    expect_open(0);
    if (n == 262144)
        expect_cap(C1, "FINALIZED", 262144, 63);
    else
        expect_capx(C1, "PARTIAL", "draw cap reached", 262144, 63);
    finish();
}
static void block_case(uint32_t n)
{
    g_slots = 1024;
    setup();
    arm_on(0, &C1);
    draws(0, n, RSXCAP_K_ARRAYS);
    drain();
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.hooks == n, "hooks");
    if (n == 4096)
        CHECK(o.end.block == 0 && o.end.c.ev == 8192, "no checkpoint, END block 0 ev 8192");
    else if (n == 4097)
        CHECK(o.end.block == 1 && o.end.c.ev == 2, "END block 1 ev 2");
    else
        CHECK(o.end.block == 0 && o.end.c.ev == 0, "hooks 0: block 0, zero counts");
    expect_open(0);
    expect_cap(C1, "FINALIZED", n, n > 4096 ? 1 : 0);
    finish();
}
static void c_draw_begin_refused_block(void)
{
    ps3dk_prof_gpu_zone z[128];
    g_slots = 1024;
    setup();
    arm_on(0, &C1);
    draws(0, 4097, RSXCAP_K_ARRAYS); /* block 0, its checkpoint, the first draw of block 1 */
    drain();
    /* 64 app zones plus 64 suppressed levels: the next draw BEGIN is REFUSED(suppress overflow) */
    for (int i = 0; i < 128; i++)
        CHECK(ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z[i]) == 0, "app zone %d", i);
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 0, "draw 4097 refused (INERT)");
    for (int i = 128; i--;)
        CHECK(ps3dk_prof_gpu_end(H[0], CTX(0), &z[i]) == 0, "app end %d", i);
    draws(0, 10, RSXCAP_K_ARRAYS);
    drain();
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.c.ref == 1 && o.end.hooks == 4108, "ref 1 in the END's block (hooks %u ref %u)", o.end.hooks,
          o.end.c.ref);
    expect_open(0);
    expect_capx(C1, "PARTIAL", "block 1 UNKNOWN", 4107, 1);
    finish();
}
static void c_ckpt_mark_depth(void)
{
    ps3dk_prof_gpu_zone z[64];
    g_slots = 1024;
    setup();
    arm_on(0, &C1);
    draws(0, 4096, RSXCAP_K_ARRAYS);
    drain();
    for (int i = 0; i < 64; i++)
        CHECK(ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z[i]) == 0, "app zone %d", i);
    draws(0, 1, RSXCAP_K_ARRAYS); /* its checkpoint MARK at depth 64: REFUSED */
    for (int i = 64; i--;)
        (void)ps3dk_prof_gpu_end(H[0], CTX(0), &z[i]);
    drain();
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.ckpt_missing == 1, "ckpt_missing 1 (%u)", o.end.ckpt_missing);
    expect_open(0);
    expect_capx(C1, "PARTIAL", "missing checkpoints", 4096, 0); /* the 4097th draw: SUPPRESSED, no pair */
    finish();
}
static void c_counts_request_local(void)
{
    setup_n(1, 2);
    arm_on(0, &C1);
    draws(0, 3, RSXCAP_K_ARRAYS);
    draws(1, 2, RSXCAP_K_ARRAYS);
    ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_UNSAFE);
    ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_IMMEDIATE);
    CHECK(FR(0) == 0 && ps3dk_prof__rsxcap_recycle(C1) == 0, "cap 1 closed");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED && FR(0) == 0 && FR(0) == 0, "cap 2");
    rsxcap_outcome o = Q(C2);
    CHECK(o.end.hooks == 0 && o.end.other_ctx == 0 && o.end.unsafe_draws == 0 && o.end.immediate == 0,
          "cap 2 starts at zero");
    expect_open(0);
    expect_capx(C1, "PARTIAL", NULL, 3, 0);
    expect_cap(C2, "FINALIZED", 0, 0);
    finish_n(2, 0);
}
static void c_after_cap_counting(void)
{
    g_slots = 1024;
    setup_n(1, 2);
    arm_on(0, &C1);
    draws(0, 262145, RSXCAP_K_ARRAYS);
    drain();
    for (uint32_t w = 0; w < 4; w++)
        ps3dk_prof__rsxcap_count(CTX(0), w == 0 ? RSXCAP_C_INVALID : w == 1 ? RSXCAP_C_NO_SPACE : w == 2 ? RSXCAP_C_REENTRANT : RSXCAP_C_MISMATCH);
    ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_UNSAFE);
    ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_IMMEDIATE);
    draws(1, 1, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.invalid == 0 && o.end.no_space == 0 && o.end.reentrant == 0 && o.end.mismatch == 0,
          "safe classifications not collected after capped");
    CHECK(o.end.unsafe_draws == 1 && o.end.immediate == 1 && o.end.other_ctx == 1, "unsafe, immediate, other_ctx count");
    expect_open(0);
    expect_capx(C1, "PARTIAL", "draw cap reached", 262144, 63);
    finish_n(2, 0);
}

/* ================================================================ the other-thread counter (r8 3) */
typedef struct oth {
    pthread_t t;
    uint32_t n;
    int ctx;
} oth;
static void *other_main(void *a)
{
    oth *o = a;
    for (uint32_t i = 0; i < o->n; i++) {
        rsxcap_draw dr;
        (void)ps3dk_prof__rsxcap_draw_begin(CTX(o->ctx), RSXCAP_K_ARRAYS, &dr);
    }
    return NULL;
}
static void other_run(uint32_t n)
{
    oth o = {0, n, 0};
    pthread_create(&o.t, NULL, other_main, &o);
    pthread_join(o.t, NULL);
}
static void c_other_thread_n(uint32_t n)
{
    setup();
    arm_on(0, &C1);
    other_run(n);
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.other_thread == 65535 || o.end.other_thread == n, "count");
    CHECK(n == 65535 ? (o.end.other_thread == 65535 && o.end.other_thread_sat == 0)
                     : (o.end.other_thread == 65535 && o.end.other_thread_sat == 1),
          "%u increments: count %u sat %u", n, o.end.other_thread, o.end.other_thread_sat);
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_other_thread_contention(void)
{
    setup();
    arm_on(0, &C1);
    ps3dk_prof_test_hooks.rsxcap_other_cas_fail = 4;
    other_run(1);
    CHECK(FR(0) == 0, "close");
    rsxcap_outcome o = Q(C1);
    CHECK(o.end.other_thread_sat == 1 && o.end.other_thread == 4, "past 4 attempts: sat 1, count %u below truth 5",
          o.end.other_thread);
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_other_thread_freeze_race(int increment_first)
{
    oth o = {0, 1, 0};
    setup();
    arm_on(0, &C1);
    if (increment_first) { /* the hook's CAS lands before the freeze */
        other_run(1);
        CHECK(FR(0) == 0, "close");
        CHECK(Q(C1).end.other_thread == 1, "counted");
    } else { /* the hook pauses before its CAS; the owner freezes; the CAS fails on FROZEN */
        step_arm(HK_OTHER_BEFORE_CAS, 1);
        pthread_create(&o.t, NULL, other_main, &o);
        step_wait_paused();
        CHECK(FR(0) == 0, "close (freeze)");
        step_go();
        pthread_join(o.t, NULL);
        rsxcap_rec r;
        ps3dk_prof__test_rsxcap_rec(C1, &r);
        CHECK(Q(C1).end.other_thread == 0 && (r.other & RSXCAP_OT_FROZEN) && (r.other & 0xffffu) == 0,
              "not counted; FROZEN intact");
    }
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_other_thread_stale_cas(void)
{
    oth o = {0, 1, 0};
    setup();
    arm_on(0, &C1);
    other_run(65535); /* count at 65535: the next increment is a SAT CAS */
    step_arm(HK_OTHER_BEFORE_CAS, 1);
    pthread_create(&o.t, NULL, other_main, &o);
    step_wait_paused();
    CHECK(FR(0) == 0, "close (freeze)");
    step_go();
    pthread_join(o.t, NULL);
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(C1, &r);
    CHECK((r.other & RSXCAP_OT_FROZEN) && (r.other & 0xffffu) == 65535 && !(r.other & RSXCAP_OT_SAT),
          "a stale pre-freeze CAS cannot clear FROZEN (word %x)", r.other);
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
static void c_other_thread_old_cap(void)
{
    oth o = {0, 1, 0};
    setup();
    arm_on(0, &C1);
    step_arm(HK_OTHER_BEFORE_CAS, 1);
    pthread_create(&o.t, NULL, other_main, &o);
    step_wait_paused(); /* the hook saw ARMED(N), before its CAS */
    CHECK(FR(0) == 0 && ps3dk_prof__rsxcap_recycle(C1) == 0, "N closed and recycled");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED && FR(0) == 0, "N+1 ARMED");
    step_go();
    pthread_join(o.t, NULL);
    rsxcap_rec r;
    ps3dk_prof__test_rsxcap_rec(C2, &r);
    CHECK(r.other == 0, "N+1 unchanged by the old-cap hook");
    CHECK(FR(0) == 0, "close N+1");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    expect_cap(C2, "FINALIZED", 0, 0);
    finish();
}
static void c_hook_reads_old_record(void)
{
    oth o = {0, 1, 0};
    setup();
    arm_on(0, &C1);
    step_arm(HK_AFTER_SLOT_LOAD, 1);
    pthread_create(&o.t, NULL, other_main, &o);
    step_wait_paused(); /* the non-owner hook saw ARMED(N), before its record read */
    CHECK(FR(0) == 0 && ps3dk_prof__rsxcap_recycle(C1) == 0, "N closed and recycled");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED, "the agent claims N+1 (writes rec[N+1])");
    step_go();
    pthread_join(o.t, NULL);
    CHECK(FR(0) == 0 && FR(0) == 0, "N+1 armed and closed");
    CHECK(Q(C1).end.other_thread == 0 && Q(C2).end.other_thread == 0, "neither counted it");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    expect_cap(C2, "FINALIZED", 0, 0);
    finish();
}

/* ================================================================ enable (R82) */
static void c_enable_before_init(void)
{
    CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_ENOTINIT, "ENOTINIT");
}
static void c_enable_no_gpu(void)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = PS3DK_PROF_MODE_APP;
    c.app = "rsxcap";
    c.build_id = "rsxcap";
    CHECK(ps3dk_prof_set_sink_path(g_out) == 0, "sink");
    ps3dk_prof_ext_config x = ext(0, 0);
    CHECK(ps3dk_prof_init_ex(&c, &x) == 0, "init without GPU domains");
    CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_ENOTSUP, "ENOTSUP");
    expect_open(0);
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
static void c_enable_late(void)
{
    setup_n(0, 1);
    fflush_(&FX[0].c);
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0 && ps3dk_prof_gpu_release(H[0], NULL) == 0, "cycle");
    CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_EBUSY, "late enable: EBUSY");
    CHECK(ps3dk_prof_gpu_attach(CTX(0), 0, g_slots, 64, &H[0]) == 0, "reattach");
    CHECK(ps3dk_prof__test_rsxcap_capable(0) == 0, "no kinds");
    expect_open(0);
    finish();
}
static void c_enable_twice(void)
{
    setup();
    CHECK(ps3dk_prof__rsxcap_enable() == PS3DK_PROF_OK, "again: OK");
    CHECK(ps3dk_prof__test_rsxcap_capable(0) == 1, "capable");
    expect_open(0);
    finish();
}
static int g_enable_rc = -100;
static void enable_in_claim(void) { g_enable_rc = ps3dk_prof__rsxcap_enable(); }
static void c_enable_after_shutdown(void)
{
    setup_n(0, 0);
    ps3dk_prof_test_hooks.after_shutdown_claim = enable_in_claim;
    ps3dk_prof_thread_end();
    expect_open(0);
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
    CHECK(g_enable_rc == PS3DK_PROF_ESTOPPED, "after the shutdown claim: ESTOPPED (%d)", g_enable_rc);
}
static void c_enable_registers_kinds(void)
{
    setup();
    CHECK(ps3dk_prof__test_rsxcap_capable(0) == 1, "capable");
    expect_open(0);
    expect_line("kinds 5");
    finish();
}
static void c_kinds_refused_not_capable(void)
{
    g_maxd = 4; /* one short of the 5 kinds */
    setup();
    CHECK(ps3dk_prof__test_rsxcap_capable(0) == 0, "not capable");
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_NO_CAPABILITY, "REFUSED(no capability)");
    expect_open(0);
    finish();
}
static void c_not_enabled(void)
{
    setup_n(0, 1);
    CHECK(ps3dk_prof__test_rsxcap_capable(0) == 0, "not capable");
    for (int i = 0; i < 3; i++)
        CHECK(FR(0) == 0, "frames unchanged");
    expect_open(0);
    expect_line("kinds 0");
    finish();
}

/* ================================================================ hook faults (2.7, R62, R80) */
static void c_fault_settles_now(int control)
{
    wsnap s;
    setup_n(1, 2);
    arm_on(0, &C1);
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "a zone open");
    fflush_(&FX[0].c);
    wsnap_take(0, &s);
    if (control) {
        ps3dk_prof__rsxcap_draw_end(&dr);
        CHECK(ST(SW()) == RSXCAP_ARMED, "control: A stays ARMED until its boundary");
        CHECK(FR(0) == 0, "boundary");
        expect_open(0);
        expect_cap(C1, "FINALIZED", 1, 0);
        finish_n(2, 0);
        return;
    }
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    CHECK(wsnap_same(0, &s), "no GPU word in the abandon call");
    /* ZERO further calls on domain A */
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_HOOK_FAULT), "TERMINAL(ABANDONED, hook fault) when it returns");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0, "recycle succeeds");
    CHECK(REQ(H[1], &C2) == RSXCAP_ACCEPTED && FR(1) == 0, "B arms");
    draws(1, 10, RSXCAP_K_ARRAYS);
    CHECK(FR(1) == 0 && outcome_is(C2, RSXCAP_O_DONE, 0), "B closes DONE");
    CHECK(wsnap_same(0, &s), "A's buffer word-equal to its copy at the fault");
    expect_open(4);
    expect_cap(C2, "FINALIZED", 10, 0);
    fflush_(&FX[1].c);
    CHECK(ps3dk_prof_gpu_detach(H[1], CTX(1), 3000000) == 0 && ps3dk_prof_gpu_release(H[1], NULL) == 0, "B cycle");
    finish_faulted(0);
}
/* the CANCEL variant on A, the STOP variant on B: flags kept, the reason is the hook fault */
static void c_fault_settles_with_flags(void)
{
    rsxcap_draw dr;
    setup_n(1, 2);
    arm_on(0, &C1);
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "A: open");
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "CANCEL");
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_HOOK_FAULT) && (SW() & RSXCAP_F_CANCEL), "CANCEL kept");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0, "recycle");
    arm_on(1, &C2);
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(1), RSXCAP_K_ARRAYS, &dr) == 1, "B: open");
    ps3dk_prof__test_rsxcap_stop();
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    CHECK(outcome_is(C2, RSXCAP_O_ABANDONED, RSXCAP_R_HOOK_FAULT) && (SW() & RSXCAP_F_STOP), "STOP kept");
    expect_open(4);
    finish_faulted(0);
}
static void c_fault_sticky_calls(void)
{
    wsnap s;
    setup();
    arm_on(0, &C1);
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "open");
    fflush_(&FX[0].c);
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    wsnap_take(0, &s);
    ps3dk_prof_gpu_zone z;
    CHECK(ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z) == PS3DK_PROF_ESTOPPED && wsnap_same(0, &s), "begin");
    CHECK(ps3dk_prof_gpu_mark(H[0], CTX(0), &D_APP) == PS3DK_PROF_ESTOPPED && wsnap_same(0, &s), "mark");
    CHECK(ps3dk_prof_gpu_calibrate(H[0], CTX(0), 1000) == PS3DK_PROF_ESTOPPED && wsnap_same(0, &s), "calibrate");
    CHECK(FR(0) == PS3DK_PROF_ESTOPPED && wsnap_same(0, &s), "gpu_frame");
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 0 && wsnap_same(0, &s), "draw zone");
    expect_open(4);
    finish_faulted(0);
}
static void c_fault_no_recovery(void)
{
    setup();
    arm_on(0, &C1);
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "open");
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0, "recycle");
    for (int i = 0; i < 100; i++)
        (void)FR(0);
    CHECK(REQ(H[0], &C2) == RSXCAP_REFUSED && g_reason == RSXCAP_R_DOMAIN_FAULTED, "REFUSED(domain faulted)");
    ps3dk_prof_gpu_stats st;
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &st) == 0 && st.fault == 1 && st.pad == PRG_FAULT_HOOK && st.retired == 0,
          "stats: fault 1, reason hook, retired 0");
    expect_open(4);
    finish_faulted(0);
}
static void c_fault_then_detach(void)
{
    setup();
    arm_on(0, &C1);
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "open");
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    uint64_t w = SW();
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 1000) == PS3DK_PROF_ESTOPPED && SW() == w, "detach EFAULT, slot unchanged");
    uint32_t rel = 9;
    CHECK(ps3dk_prof_gpu_release(H[0], &rel) == PS3DK_PROF_EBUSY && rel == 0, "release EBUSY");
    expect_open(4);
    finish_faulted(0);
}
static void c_fault_never_drained(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED && FR(0) == 0, "armed");
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "one draw");
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_HOOK_FAULT), "live outcome");
    expect_open(4);
    expect_nocap(C1);
    finish_faulted(0);
}
static void c_fault_drained_prefix(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 3, RSXCAP_K_ARRAYS);
    drain(); /* BEGIN and 3 draws drained */
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "open");
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    expect_open(4);
    expect_capx(C1, "PARTIAL", "missing END", 3, 0);
    finish_faulted(0);
}
static void c_ckpt_publish_fault_settles(void)
{
    g_slots = 1024;
    setup();
    arm_on(0, &C1);
    draws(0, 4096, RSXCAP_K_ARRAYS);
    drain();
    ps3dk_prof_test_hooks.gpu_snap_ops = 0;
    ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1; /* the checkpoint MARK's publication */
    draws(0, 1, RSXCAP_K_ARRAYS);
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_FAULT), "TERMINAL(ABANDONED, fault)");
    CHECK(Q(C1).end.ckpt_missing == 0, "ckpt_missing not incremented");
    expect_open(4);
    expect_capx(C1, "PARTIAL", "missing END", 4096, 0);
    finish_faulted(0);
}

/* ================================================================ seal / stop at the points (R66, R67) */
static void c_stop_point(int pt)
{
    /* END-only at P0-P4 (the ARMED boundary), BEGIN-only at P1, P5, P6 (the arming boundary) */
    actor a;
    int end_side = pt <= 4;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    if (end_side) {
        CHECK(FR(0) == 0, "arm");
        draws(0, 2, RSXCAP_K_ARRAYS);
    }
    step_arm(pt == 0 ? RT_BEFORE_CLOSING : PRG_PT(PRG_SP_P1 + pt - 1), 1);
    actor_start(&a, act_stop);
    CHECK(FR(0) == 0, "boundary");
    actor_join(&a);
    if (end_side) {
        if (pt == 0)
            CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "P0: ABANDONED(stopping)");
        else
            CHECK(outcome_is(C1, RSXCAP_O_DONE, 0), "P%d: the admitted END keeps its receipt outcome", pt);
    } else {
        CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "P%d: ABANDONED(stopping) at the ARMED publication",
              pt);
    }
    uint64_t opened = g_draw_opened;
    draws(0, 1, RSXCAP_K_ARRAYS);
    CHECK(g_draw_opened == opened, "no further draw instrumented");
    expect_open(0);
    if (end_side && pt > 0)
        expect_cap(C1, "FINALIZED", 2, 0);
    else
        expect_capx(C1, "PARTIAL", "missing END", end_side ? 2 : 0, 0);
    finish();
}

/* ================================================================ the agent: reentry, non-owner, unlock fault */
static int g_cb_req_rc = -100;
static int32_t fcb_request(gpp_ctx *c, uint32_t count)
{
    uint32_t cap, r;
    g_cb_req_rc = ps3dk_prof__rsxcap_request(1, 1, H[0], &cap, &r);
    c->end = ((fctx *)(void *)c)->buf + CBUF; /* the callback makes room */
    return fcb(c, count);
}
static void c_request_reentry_refused(void)
{
    setup();
    uint64_t w = SW();
    uint32_t users = ps3dk_prof__test_gpu_users(0);
    FX[0].c.callback = fcb_request;
    FX[0].c.end = FX[0].c.current + 3;
    ps3dk_prof_gpu_zone z;
    (void)ps3dk_prof_gpu_begin(H[0], CTX(0), &D_APP, &z); /* the port's callback runs inside the GPU call */
    FX[0].c.callback = fcb;
    FX[0].c.end = FX[0].buf + CBUF;
    CHECK(g_cb_req_rc == PS3DK_PROF_EBUSY, "inside a GPU port callback: EBUSY (%d)", g_cb_req_rc);
    CHECK(SW() == w && ps3dk_prof__test_gpu_users(0) == users, "slot and pins unchanged");
    (void)ps3dk_prof_gpu_end(H[0], CTX(0), &z);
    expect_open(0);
    finish();
}
static atomic_int g_nonowner_rc = -100;
static void *nonowner_req(void *a)
{
    (void)a;
    uint32_t c, r;
    atomic_store(&g_nonowner_rc, ps3dk_prof__rsxcap_request(1, 1, H[0], &c, &r));
    return NULL;
}
static void c_request_from_non_owner_ok(void)
{
    pthread_t t;
    setup();
    pthread_create(&t, NULL, nonowner_req, NULL);
    pthread_join(t, NULL);
    CHECK(atomic_load(&g_nonowner_rc) == RSXCAP_ACCEPTED, "ACCEPTED, not EPERM (%d)", atomic_load(&g_nonowner_rc));
    expect_open(0);
    finish();
}
static void c_request_lock_fault(void)
{
    setup();
    uint64_t w = SW();
    ps3dk_prof_test_hooks.spu_ops = 0;
    ps3dk_prof_test_hooks.spu_lock_fail_at = 1;
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0, "REFUSED, cap 0");
    ps3dk_prof_test_hooks.spu_lock_fail_at = 0;
    CHECK(SW() == w, "slot word unchanged");
    expect_open(1);
    finish_faulted(0);
}
static void c_agent_unlock_fail_withdrawable(void)
{
    setup();
    ps3dk_prof_test_hooks.spu_unlock_fail_site = 8; /* U_RSXCAP_REQUEST */
    int rc = REQ(H[0], &C1);
    CHECK(rc == PS3DK_PROF_ESTOPPED && C1 == 1, "ESTOPPED with *cap = e (rc %d cap %u)", rc, C1);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, 0, 1, 0, 0), "withdrawn: IDLE(e)");
    rsxcap_outcome o = Q(1);
    CHECK(o.outcome == RSXCAP_O_WITHDRAWN && o.reason == RSXCAP_R_FAULT, "WITHDRAWN(fault)");
    expect_open(1);
    finish_faulted(0);
}
static atomic_int g_ag_rc = -100;
static uint32_t g_ag_cap;
static void *agent_req(void *a)
{
    (void)a;
    uint32_t r;
    atomic_store(&g_ag_rc, ps3dk_prof__rsxcap_request(1, 1, H[0], &g_ag_cap, &r));
    return NULL;
}
/* the controller: with the owner paused in ARMING (slot 1), let the agent finish (its unlock fails), then the owner */
static void *ctl_release_agent(void *a)
{
    (void)a;
    step_wait_paused2(1);
    step_go2(0);
    SPIN_UNTIL(atomic_load(&g_ag_rc) != -100, "the agent's return");
    CHECK(RSXCAP_STATE(SW()) == RSXCAP_ARMING, "the agent's withdrawal did not land (ARMING kept)");
    step_go2(1);
    return NULL;
}
static void c_agent_unlock_fail_arming_won(int armed)
{
    pthread_t t;
    setup();
    ps3dk_prof_test_hooks.spu_unlock_fail_site = 8;
    step_arm(AG_AFTER_PUBLICATION, 1); /* published, before the failing unlock */
    pthread_create(&t, NULL, agent_req, NULL);
    step_wait_paused();
    if (armed) {
        /* the owner can't arm while the agent holds the mutex? It can: gpu_frame takes no extension mutex */
        CHECK(FR(0) == 0 && ST(SW()) == RSXCAP_ARMED, "the owner armed between publication and unlock");
        step_go();
        pthread_join(t, NULL);
        CHECK(atomic_load(&g_ag_rc) == PS3DK_PROF_ESTOPPED && g_ag_cap == 1, "ESTOPPED, cap e");
        CHECK(ST(SW()) == RSXCAP_ARMED, "ARMED unchanged");
        CHECK(Q(1).outcome == RSXCAP_O_ARMED_FAULTED, "query ARMED_FAULTED");
        CHECK(FR(0) == PS3DK_PROF_ESTOPPED && ST(SW()) == RSXCAP_ARMED, "later owner calls ESTOPPED, no transition");
        {
            wsnap s;
            rsxcap_draw dr;
            uint64_t w = SW();
            fflush_(&FX[0].c);
            wsnap_take(0, &s);
            dsnap d0 = dom_snap(0);
            CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 0 && dr.open == 0,
                  "a fresh draw after ARMED_FAULTED: refused, no zone");
            ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_UNSAFE);
            CHECK(wsnap_same(0, &s) && dom_same(0, &d0, 0) && SW() == w, "no GPU word, seq, ticket or slot change");
        }
    } else {
        /* the owner wins ARMING and pauses; the agent's unlock fails; the owner resumes */
        pthread_t c;
        step_arm2(1, RT_AFTER_CLAIM, 1);
        pthread_create(&c, NULL, ctl_release_agent, NULL);
        CHECK(FR(0) == 0, "the owner's boundary");
        pthread_join(c, NULL);
        pthread_join(t, NULL);
        CHECK(atomic_load(&g_ag_rc) == PS3DK_PROF_ESTOPPED && g_ag_cap == 1, "ESTOPPED, cap e");
        CHECK(outcome_is(1, RSXCAP_O_ABANDONED, RSXCAP_R_FAULT), "at the ARMED publication: TERMINAL(ABANDONED, fault)");
    }
    expect_open(1);
    finish_faulted(0);
}

/* carry 1: the agent's unlock fails and STOP lands between its withdrawal load and CAS, no owner progress: the
 * withdrawal retries on the flag change and settles (never REQUESTED stranded), both flags kept, no record write */
static void act_stop_cancel_flag(void) { ps3dk_prof__test_rsxcap_stop(); }
static void c_agent_unlock_fail_stop_at_withdraw(void)
{
    actor a;
    rsxcap_rec r0, r1;
    setup();
    ps3dk_prof__test_rsxcap_rec(1, &r0);
    ps3dk_prof_test_hooks.spu_unlock_fail_site = 8;
    step_arm(AG_WITHDRAW_LOAD, 1);
    actor_start(&a, act_stop_cancel_flag);
    int rc = REQ(H[0], &C1);
    actor_join(&a);
    CHECK(rc == PS3DK_PROF_ESTOPPED && C1 == 1, "ESTOPPED, cap e");
    CHECK(SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, 1, 0, 0), "settled IDLE(e)+STOP, not REQUESTED (%llx)",
          (unsigned long long)SW());
    rsxcap_outcome o = Q(1);
    CHECK(o.outcome == RSXCAP_O_WITHDRAWN && o.reason == RSXCAP_R_FAULT, "WITHDRAWN(fault)");
    ps3dk_prof__test_rsxcap_rec(1, &r1);
    CHECK(r1.bound == 0 && r1.frozen == 0, "no owner fields; the agent's withdrawal wrote nothing");
    (void)r0;
    expect_open(1);
    finish_faulted(0);
}
/* carry 2: direct REQUESTED -> TERMINAL paths publish the initialized zero telemetry and write no record */
static void c_early_terminal_query(void)
{
    rsxcap_rec r0, r1;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    ps3dk_prof__test_rsxcap_rec(C1, &r0);
    ps3dk_prof__test_rsxcap_stop();
    CHECK(FR(0) == 0, "the owner's STOP settlement (R18)");
    ps3dk_prof__test_rsxcap_rec(C1, &r1);
    CHECK(!memcmp(&r0, &r1, RSXCAP_REC_IMMUTABLE_BYTES), "no record write on the contested path");
    rsxcap_outcome o = Q(C1);
    CHECK(o.outcome == RSXCAP_O_REFUSED && o.reason == RSXCAP_R_STOPPING, "REFUSED(stopping)");
    CHECK(o.begin_r.kind == PRG_R_NONE && o.end_r.kind == PRG_R_NONE && o.end.hooks == 0 && o.end.c.ev == 0 &&
              o.end.other_thread == 0 && o.end.id.cap == 0,
          "initialized absent / zero telemetry (never armed)");
    expect_open(0);
    expect_nocap(C1);
    finish();
}
/* carry 3: a stale cap with no cached result is refused, never answered from a later cap's word */
static void c_query_stale_refused(void)
{
    rsxcap_outcome o;
    setup();
    arm_on(0, &C1);
    CHECK(FR(0) == 0 && ps3dk_prof__rsxcap_recycle(C1) == 0, "cap 1 closed, recycled");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED && FR(0) == 0 && FR(0) == 0, "cap 2 TERMINAL(DONE)");
    CHECK(ps3dk_prof__rsxcap_query(5, &o) == PS3DK_PROF_EINVAL, "a never-claimed cap: refused");
    CHECK(ps3dk_prof__rsxcap_query(0, &o) == PS3DK_PROF_EINVAL, "cap 0: refused");
    CHECK(ps3dk_prof__rsxcap_query(C1, &o) == 0 && o.cached && o.cap == C1 && o.outcome == RSXCAP_O_DONE,
          "cap 1: its own cached result, not cap 2's TERMINAL word");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    expect_cap(C2, "FINALIZED", 0, 0);
    finish();
}

/* ================================================================ publication faults at the boundary (R10, R11) */
static void c_begin_publish_fail(void)
{
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    ps3dk_prof_test_hooks.gpu_snap_ops = 0;
    ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1; /* the BEGIN's publication */
    CHECK(FR(0) == PS3DK_PROF_ESTOPPED, "EFAULT (mapped ESTOPPED)");
    CHECK(outcome_is(C1, RSXCAP_O_REFUSED, RSXCAP_R_BEGIN_PUBLISH_FAULT), "TERMINAL(REFUSED, begin publish fault)");
    CHECK(Q(C1).begin_r.kind == PRG_R_EVENT, "the actual receipt is recorded: EVENT");
    expect_open(4);
    finish_faulted(0);
}
static void c_end_publish_fail(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    drain();
    ps3dk_prof_test_hooks.gpu_snap_ops = 0;
    ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at = 1; /* the END's publication */
    CHECK(FR(0) == PS3DK_PROF_ESTOPPED, "EFAULT (mapped ESTOPPED)");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_FAULT), "TERMINAL(ABANDONED, fault), committed 0");
    CHECK(Q(C1).end_r.kind == PRG_R_EVENT, "the END receipt recorded");
    expect_open(4);
    expect_capx(C1, "PARTIAL", "missing END", 2, 0);
    finish_faulted(0);
}
/* a refused abandon (the domain stopped admitting) still takes the fault path and settles */
static void c_fault_abandon_refused(void)
{
    setup();
    arm_on(0, &C1);
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "open");
    __atomic_store_n(&BK[0].lab[64], 0x7FFFFFF0u, __ATOMIC_SEQ_CST);
    CHECK(ps3dk_prof_drain() == 0, "the drainer latches an anomaly: no further admission");
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    ps3dk_prof_gpu_stats st;
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &st) == 0 && st.fault == 1 && st.pad == PRG_FAULT_HOOK, "hook fault latched");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_HOOK_FAULT), "TERMINAL(ABANDONED, hook fault) now");
    expect_open(4);
    finish_faulted(0);
}
/* fail-closed teardown: no FINAL, detach EFAULT, release EBUSY, resources borrowed; the file: missing END, no FINAL */
static void c_fault_resources_retained(void)
{
    setup();
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    drain();
    rsxcap_draw dr;
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "open");
    ps3dk_prof__rsxcap_draw_abandon(&dr);
    ps3dk_prof_gpu_stats st;
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &st) == 0 && st.fault == 1 && st.pad == PRG_FAULT_HOOK && st.retired == 0,
          "stats: fault 1, reason hook, retired 0");
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 1000) == PS3DK_PROF_ESTOPPED, "detach EFAULT");
    uint32_t rel = 9;
    CHECK(ps3dk_prof_gpu_release(H[0], &rel) == PS3DK_PROF_EBUSY && rel == 0, "release EBUSY: borrowed");
    expect_open(4);
    /* an open last segment cannot prove its ordinals: block 0 UNKNOWN (the reader's rule) */
    expect_cap(C1, "PARTIAL: missing END, reason unknown (no domain FINAL); block 0 UNKNOWN", 2, 0);
    finish_faulted(0);
}

/* ================================================================ unforced stress (TSan; fixed counts) */
static atomic_int g_hooks_go;
static void *stress_hook(void *a)
{
    (void)a;
    SPIN_UNTIL(atomic_load(&g_hooks_go), "hooks start");
    for (uint32_t i = 0; i < 100000; i++) {
        rsxcap_draw dr;
        (void)ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr); /* non-owner: other_thread */
    }
    return NULL;
}
/* the agent's step at one owner boundary: recycle a TERMINAL cap, claim the next while caps remain */
static uint32_t g_claims;
static void agent_turn(void)
{
    uint64_t w = SW();
    uint32_t c;
    if (ST(w) == RSXCAP_TERMINAL)
        (void)ps3dk_prof__rsxcap_recycle(EP(w));
    if (ST(SW()) == RSXCAP_IDLE && g_claims < 16 && REQ(H[0], &c) == RSXCAP_ACCEPTED)
        g_claims++;
}
static void c_other_thread_stress(int neg)
{
    pthread_t t[4];
    g_slots = 1024;
    setup();
    ps3dk_prof_test_hooks.rsxcap_neg_plain_other = neg;
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, stress_hook, NULL);
    atomic_store(&g_hooks_go, 1);
    for (int f = 0; f < 1000; f++) {
        agent_turn();
        CHECK(FR(0) == 0, "frame %d", f);
        draws(0, 64, RSXCAP_K_INDEX);
        drain();
    }
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    ps3dk_prof_test_hooks.rsxcap_neg_plain_other = 0;
    CHECK(FR(0) == 0, "last boundary");
    CHECK(g_claims == 16, "16 captures (%u)", g_claims);
    expect_open(0);
    expect_line("anycaps");
    finish();
}
/* the owner's frames against an agent thread's 16 claims (cancel or recycle) and 64 refused requests, and a stopper */
static atomic_int g_agent_done;
static void *churn_agent(void *a)
{
    (void)a;
    uint32_t claims = 0, refused = 0;
    for (long it = 0; it < 2000000 && (claims < 16 || refused < 64); it++) {
        uint64_t w = SW();
        uint32_t c;
        if (ST(w) == RSXCAP_TERMINAL) {
            (void)ps3dk_prof__rsxcap_recycle(EP(w));
            continue;
        }
        if (claims < 16) {
            if (ST(w) != RSXCAP_IDLE) {
                sched_yield();
                continue;
            }
            int rc = REQ(H[0], &c);
            if (rc == RSXCAP_ACCEPTED) {
                claims++;
                if (claims % 2)
                    (void)ps3dk_prof__rsxcap_cancel(c); /* every other claim cancelled (REQUESTED or ARMED+CANCEL) */
            } else if (g_reason == RSXCAP_R_STOPPING) {
                claims = 16;
            }
        } else if (REQ(H[0], &c) == RSXCAP_REFUSED) {
            refused++;
        }
    }
    atomic_store(&g_agent_done, 1);
    return NULL;
}
static void c_slot_churn(int neg)
{
    pthread_t ag;
    setup();
    ps3dk_prof_test_hooks.rsxcap_neg_late_fields = neg;
    pthread_create(&ag, NULL, churn_agent, NULL);
    for (int f = 0; f < 4000; f++) {
        if (f == 3900)
            ps3dk_prof__test_rsxcap_stop(); /* the stopper at owner frame 3900 */
        (void)FR(0);
        if (f % 4 == 0)
            draws(0, 2, RSXCAP_K_ARRAYS);
        if (f % 50 == 0)
            drain();
        if (atomic_load(&g_agent_done) && f > 3900)
            break;
    }
    pthread_join(ag, NULL);
    ps3dk_prof_test_hooks.rsxcap_neg_late_fields = 0;
    (void)FR(0);
    CHECK(SW() & RSXCAP_F_STOP, "STOP kept");
    expect_open(0);
    expect_line("anycaps");
    finish();
}
/* R77: 16 claims plus 64 refused requests against one shutdown at a fixed agent iteration */
static atomic_int g_shut_at;
static atomic_int g_accepted_after_stop;
static void *rvs_agent(void *a)
{
    (void)a;
    uint32_t c, last = 0;
    for (int it = 0; it < 80; it++) {
        if (it == 40) {
            atomic_store(&g_shut_at, 1);
            SPIN_UNTIL(SW() & RSXCAP_F_STOP, "the shutdown's STOP");
        }
        int stopped = (SW() & RSXCAP_F_STOP) != 0;
        int rc = ps3dk_prof__rsxcap_request(1, 1, H[0], &c, &g_reason);
        if (rc == RSXCAP_ACCEPTED) {
            if (stopped)
                atomic_fetch_add(&g_accepted_after_stop, 1);
            (void)ps3dk_prof__rsxcap_cancel(c);
            last = c;
        } else if (rc == RSXCAP_REFUSED && c) {
            last = c;
        }
    }
    if (last) {
        rsxcap_outcome o;
        (void)ps3dk_prof__rsxcap_query(last, &o); /* the session-end query services REQUESTED + STOP */
    }
    return NULL;
}
static void c_request_vs_shutdown(void)
{
    pthread_t ag;
    setup();
    pthread_create(&ag, NULL, rvs_agent, NULL);
    SPIN_UNTIL(atomic_load(&g_shut_at), "the agent's iteration 40");
    fflush_(&FX[0].c);
    drain();
    ps3dk_prof_thread_end();
    expect_open(0);
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    pthread_join(ag, NULL);
    CHECK(ST(SW()) != RSXCAP_REQUESTED, "no REQUESTED survives (word %llx)", (unsigned long long)SW());
    CHECK(atomic_load(&g_accepted_after_stop) == 0, "no request accepted after STOP");
    fprintf(g_exp, "anycaps\nanomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
/* R89: the agent publishes, checks and queries 16 times against one shutdown; the owner makes no calls */
static rsxcap_rec g_svc_rec[17];
static void *svc_agent(void *a)
{
    (void)a;
    for (int it = 0; it < 16; it++) {
        uint32_t c;
        if (it == 8)
            atomic_store(&g_shut_at, 1);
        int rc = ps3dk_prof__rsxcap_request(1, 1, H[0], &c, &g_reason);
        if (c && rc != PS3DK_PROF_EBUSY) {
            ps3dk_prof__test_rsxcap_rec(c, &g_svc_rec[c]);
            rsxcap_outcome o;
            (void)ps3dk_prof__rsxcap_query(c, &o);
            if (ST(SW()) == RSXCAP_REQUESTED && EP(SW()) == c)
                (void)ps3dk_prof__rsxcap_cancel(c);
            rsxcap_rec now;
            ps3dk_prof__test_rsxcap_rec(c, &now);
            CHECK(!memcmp(&now, &g_svc_rec[c], RSXCAP_REC_IMMUTABLE_BYTES), "rec[%u] unchanged by the service", c);
        }
    }
    uint64_t w = SW();
    if (EP(w)) {
        rsxcap_outcome o;
        (void)ps3dk_prof__rsxcap_query(EP(w), &o);
    }
    return NULL;
}
static void c_stop_service(void)
{
    pthread_t ag;
    setup();
    fflush_(&FX[0].c);
    drain();
    pthread_create(&ag, NULL, svc_agent, NULL);
    SPIN_UNTIL(atomic_load(&g_shut_at), "the agent's iteration 8");
    ps3dk_prof__test_rsxcap_stop(); /* the stopper's fetch_or (the owner makes no calls) */
    pthread_join(ag, NULL);
    uint64_t w = SW();
    CHECK(!(ST(w) == RSXCAP_REQUESTED && (w & RSXCAP_F_STOP)), "no REQUESTED+STOP after the final query (%llx)",
          (unsigned long long)w);
    expect_open(0);
    expect_line("anycaps");
    finish();
}
/* the owner loops attach, 50 frames, detach, release; the agent loops domain-0 requests and cancels */
static atomic_int g_cvd_stop;
static void o_cvd_loop(void)
{
    for (int life = 0; life < 20; life++) {
        if (ps3dk_prof_gpu_attach(CTX(0), 0, g_slots, 64, &H[0]) != 0)
            break;
        for (int f = 0; f < 50; f++)
            (void)FR(0);
        fflush_(&FX[0].c);
        (void)ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000);
        (void)ps3dk_prof_gpu_release(H[0], NULL);
    }
    atomic_store(&g_cvd_stop, 1);
}
static void *cvd_agent(void *a)
{
    (void)a;
    for (long it = 0; it < 100000 && !atomic_load(&g_cvd_stop); it++) {
        uint32_t c, r;
        uint64_t w = SW();
        if (ST(w) == RSXCAP_TERMINAL)
            (void)ps3dk_prof__rsxcap_recycle(EP(w));
        if (ps3dk_prof__rsxcap_request(1, 1, 0, &c, &r) == RSXCAP_ACCEPTED && (it % 3 == 0))
            (void)ps3dk_prof__rsxcap_cancel(c);
        sched_yield();
    }
    return NULL;
}
static void c_claim_vs_detach(void)
{
    pthread_t ag;
    fx_init(0, 0, 0, g_slots, 64);
    CHECK(start(2, g_slots) == 0, "start");
    CHECK(ps3dk_prof__rsxcap_enable() == 0, "enable");
    pthread_create(&o_thr, NULL, owner_loop, NULL);
    pthread_create(&ag, NULL, cvd_agent, NULL);
    own_do(o_cvd_loop);
    pthread_join(ag, NULL);
    uint64_t w = SW();
    CHECK(ST(w) != RSXCAP_REQUESTED, "the slot never ends REQUESTED for a domain past ATTACHED (%llx)",
          (unsigned long long)w);
    expect_open(0);
    expect_line("anycaps");
    atomic_store(&o_quit, 1);
    pthread_join(o_thr, NULL);
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
/* the publication pool tags: written once, never rewritten over a 16-cap run */
static void c_pool_tags_once(void)
{
    uint32_t n;
    static char first[16][2][RSXCAP_END_BYTES];
    const ps3dk_prof_desc *pool = ps3dk_prof__rsxcap_pool(&n);
    setup();
    for (uint32_t c = 1; c <= 16; c++) {
        uint32_t cap;
        CHECK(REQ(H[0], &cap) == RSXCAP_ACCEPTED && cap == c, "cap %u", c);
        CHECK(FR(0) == 0, "arm");
        draws(0, 1, RSXCAP_K_ARRAYS);
        CHECK(FR(0) == 0 && ps3dk_prof__rsxcap_recycle(c) == 0, "close");
        strcpy(first[c - 1][0], pool[(c - 1) * RSXCAP_PER_CAP].logical_tag);
        strcpy(first[c - 1][1], pool[(c - 1) * RSXCAP_PER_CAP + 1].logical_tag);
    }
    for (uint32_t c = 1; c <= 16; c++)
        CHECK(!strcmp(first[c - 1][0], pool[(c - 1) * RSXCAP_PER_CAP].logical_tag) &&
                  !strcmp(first[c - 1][1], pool[(c - 1) * RSXCAP_PER_CAP + 1].logical_tag) && first[c - 1][0][0],
              "cap %u tags equal their first formatting", c);
    expect_open(0);
    for (uint32_t c = 1; c <= 16; c++)
        expect_cap(c, "FINALIZED", 1, 0);
    finish();
}

/* ================================================================ S2 r2: the review's blockers and controls */
/* blocker 1: a pin that fails in the request scan refuses without consuming a cap (two eligible domains) */
static void c_request_pin_saturated(void)
{
    rsxcap_rec r0, r1;
    setup_n(1, 2);
    ps3dk_prof__test_rsxcap_rec(1, &r0);
    uint32_t u0 = ps3dk_prof__test_gpu_users(0);
    uint64_t w = SW();
    ps3dk_prof__test_gpu_set_users(1, 0x80000000u); /* GPU_USERS_MAX: slot 1 cannot be pinned */
    CHECK(REQ(0, &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_BUSY,
          "the inventory is incomplete: REFUSED, cap 0 (never resolves the other domain as unique) (rc reason %u cap %u)",
          g_reason, C1);
    CHECK(SW() == w && ps3dk_prof__test_gpu_users(0) == u0, "slot word unchanged; slot 0's pin released");
    ps3dk_prof__test_rsxcap_rec(1, &r1);
    CHECK(!memcmp(&r0, &r1, sizeof r0), "no record written");
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_BUSY, "an explicit domain too");
    ps3dk_prof__test_gpu_set_users(1, 0);
    CHECK(REQ(0, &C1) == RSXCAP_REFUSED && g_reason == RSXCAP_R_BAD_DOMAIN, "control: two domains, bad domain");
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED && C1 == 1, "control: the first cap is still 1");
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "cancel");
    expect_open(0);
    finish_n(2, 0);
}
static void c_request_pin_contention(void)
{
    setup();
    uint64_t w = SW();
    ps3dk_prof_test_hooks.rsxcap_pin_cas_fail = 3; /* every one of the 3 bounded attempts meets a concurrent pin */
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_BUSY, "past the bound: REFUSED, cap 0");
    CHECK(SW() == w && ps3dk_prof__test_gpu_users(0) == 3, "slot word unchanged; only the 3 foreign pins remain");
    ps3dk_prof__test_gpu_set_users(0, 0); /* the foreign pinners leave */
    ps3dk_prof_test_hooks.rsxcap_pin_cas_fail = 2; /* within the bound: the third attempt pins */
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED && C1 == 1, "within the bound: ACCEPTED, cap 1");
    CHECK(ps3dk_prof__test_gpu_users(0) == 2, "the agent's own pin released after the revalidation");
    ps3dk_prof__test_gpu_set_users(0, 0);
    CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0, "cancel");
    expect_open(0);
    finish();
}
/* blocker 2: an extension fault with a draw already open: the END is popped without a command, nothing submitted */
static void c_ext_fault_open_draw(void)
{
    wsnap s;
    rsxcap_draw dr, d2;
    ps3dk_prof_gpu_stats st;
    setup();
    arm_on(0, &C1);
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "a draw open");
    ps3dk_prof_test_hooks.spu_unlock_fail_site = 7; /* U_GPU_STATS: the extension fault latches */
    CHECK(ps3dk_prof_gpu_get_stats(H[0], &st) == PS3DK_PROF_ESTOPPED, "the extension fault");
    fflush_(&FX[0].c);
    wsnap_take(0, &s);
    dsnap d0 = dom_snap(0);
    uint64_t w = SW();
    ps3dk_prof__rsxcap_draw_end(&dr);
    CHECK(wsnap_same(0, &s) && dom_same(0, &d0, -1), "the open draw: no command, no seq or ticket; popped");
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &d2) == 0 && d2.open == 0, "a fresh draw: refused");
    ps3dk_prof__rsxcap_count(CTX(0), RSXCAP_C_IMMEDIATE);
    CHECK(wsnap_same(0, &s) && dom_same(0, &d0, -1) && SW() == w, "nothing submitted, the slot untouched");
    CHECK(Q(C1).outcome == RSXCAP_O_ARMED_FAULTED, "query: ARMED_FAULTED (no transition)");
    expect_open(1);
    finish_faulted(0);
}
/* blocker 2: a lock fault in the checkpoint registration stops the hook: no MARK, no draw, settled */
static void c_ckpt_lock_fault_stops_hook(void)
{
    wsnap s;
    rsxcap_draw dr;
    g_slots = 1024;
    setup();
    arm_on(0, &C1);
    draws(0, 4096, RSXCAP_K_ARRAYS);
    drain();
    fflush_(&FX[0].c);
    wsnap_take(0, &s);
    dsnap d0 = dom_snap(0);
    ps3dk_prof_test_hooks.p_lock_ops = 0;
    ps3dk_prof_test_hooks.p_lock_fail_at = 1; /* the checkpoint descriptor's desc_mutex */
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 0 && dr.open == 0, "the hook stops");
    ps3dk_prof_test_hooks.p_lock_fail_at = 0;
    CHECK(wsnap_same(0, &s) && dom_same(0, &d0, 0), "no MARK, no draw BEGIN: no GPU word, seq or ticket");
    CHECK(outcome_is(C1, RSXCAP_O_ABANDONED, RSXCAP_R_STOPPING), "TERMINAL(ABANDONED, stopping)");
    CHECK(Q(C1).end.ckpt_missing == 0 && Q(C1).end.hooks == 4096, "not a missing checkpoint; the draw not counted");
    expect_open(1);
    expect_cap(C1, "PARTIAL: missing END, reason unknown (no domain FINAL); block 0 UNKNOWN", 4096, 0);
    finish_faulted(0);
}
/* the bounded publisher CASes (R34): flag changes injected between the load and the CAS, attempts counted */
static int g_inj_point, g_inj_i, g_inj_n;
static int g_inj_kind[3]; /* 1 STOP (the stopper), 2 CANCEL (the agent's cancel CAS) */
static void inj(int point)
{
    (void)point;
    int k = g_inj_kind[g_inj_i++];
    if (k == 1)
        ps3dk_prof__test_rsxcap_stop();
    else
        CHECK(ps3dk_prof__rsxcap_cancel(C1) == PS3DK_PROF_OK, "the injected cancel");
    if (g_inj_i < g_inj_n)
        step_arm(g_inj_point, 1); /* the next attempt of the same publisher */
}
static void inj_arm(int point, int k0, int k1)
{
    g_inj_point = point;
    g_inj_i = 0;
    g_inj_n = k1 ? 2 : 1;
    g_inj_kind[0] = k0;
    g_inj_kind[1] = k1;
    st_inline = inj;
    step_arm(point, 1);
}
static void inj_done(void)
{
    st_inline = NULL;
    step_disarm();
}
#define TRIES(pt, base) (step_hits(pt) - (base))
static void pub_bound(int which)
{
    setup();
    int b = 0, ok = 1;
    uint64_t w = 0;
    const char *name = "?";
    switch (which) {
    case 0: /* the agent's publication: only STOP can change CLAIMING; bound 2 */
        name = "agent publication";
        b = step_hits(AG_PUB_CAS);
        inj_arm(AG_PUB_CAS, 1, 0);
        CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 1 && g_reason == RSXCAP_R_STOPPING, "refused, cap e");
        w = SW();
        ok = TRIES(AG_PUB_CAS, b) == 2 &&
             w == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_STOP, 1, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING);
        break;
    case 1: /* the agent's claim: STOP stops it (no retry into a stopped profiler); bound 2 */
        name = "agent claim";
        b = step_hits(AG_CLAIM_CAS);
        inj_arm(AG_CLAIM_CAS, 1, 0);
        CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_STOPPING, "refused, cap 0");
        w = SW();
        ok = TRIES(AG_CLAIM_CAS, b) == 1 && w == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, 0, 0, 0);
        break;
    case 2: /* the agent's withdrawal (unlock fault): a STOP change is retried, both flags kept */
        name = "agent withdrawal";
        ps3dk_prof_test_hooks.spu_unlock_fail_site = 8;
        b = step_hits(AG_WITHDRAW_CAS);
        inj_arm(AG_WITHDRAW_CAS, 1, 0);
        CHECK(REQ(H[0], &C1) == PS3DK_PROF_ESTOPPED && C1 == 1, "ESTOPPED, cap e");
        w = SW();
        ok = TRIES(AG_WITHDRAW_CAS, b) == 2 && w == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, 1, 0, 0);
        break;
    case 3: /* the agent's cancel of ARMED: a STOP change retried */
        name = "agent cancel";
        arm_on(0, &C1);
        b = step_hits(AG_CANCEL_CAS);
        inj_arm(AG_CANCEL_CAS, 1, 0); /* STOP between this cancel's load and its CAS */
        CHECK(ps3dk_prof__rsxcap_cancel(C1) == PS3DK_PROF_OK, "cancel");
        w = SW();
        ok = TRIES(AG_CANCEL_CAS, b) == 2 &&
             w == RSXCAP_WORD(RSXCAP_ARMED, RSXCAP_F_STOP | RSXCAP_F_CANCEL, C1, 0, 0);
        break;
    case 4: /* the agent's recycle: STOP retried, the same cap's CANCEL kept, outcome bits cleared */
        name = "agent recycle";
        arm_on(0, &C1);
        CHECK(ps3dk_prof__rsxcap_cancel(C1) == 0 && FR(0) == 0, "TERMINAL+CANCEL");
        b = step_hits(AG_RECYCLE_CAS);
        inj_arm(AG_RECYCLE_CAS, 1, 0);
        CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0, "recycle");
        w = SW();
        ok = TRIES(AG_RECYCLE_CAS, b) == 2 &&
             w == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP | RSXCAP_F_CANCEL, C1, 0, 0);
        break;
    case 5: /* the owner's claim: STOP between its load and CAS: the retry settles REFUSED(stopping) */
        name = "owner claim";
        CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
        b = step_hits(RT_CLAIM_CAS);
        inj_arm(RT_CLAIM_CAS, 1, 0);
        CHECK(FR(0) == 0, "frame");
        w = SW();
        ok = TRIES(RT_CLAIM_CAS, b) == 2 &&
             w == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_STOP, C1, RSXCAP_O_REFUSED, RSXCAP_R_STOPPING);
        break;
    case 6: /* the owner's ARMED publication: a CANCEL change retried, CANCEL published */
        name = "owner ARMED publication";
        CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
        b = step_hits(RT_ARMED_CAS);
        inj_arm(RT_ARMED_CAS, 2, 0);
        CHECK(FR(0) == 0, "frame");
        w = SW();
        ok = TRIES(RT_ARMED_CAS, b) == 2 && w == RSXCAP_WORD(RSXCAP_ARMED, RSXCAP_F_CANCEL, C1, 0, 0);
        break;
    case 7: /* the owner's CLOSING: a CANCEL change retried; the close then completes with CANCEL kept */
        name = "owner CLOSING";
        arm_on(0, &C1);
        b = step_hits(RT_CLOSING_CAS);
        inj_arm(RT_CLOSING_CAS, 2, 0);
        CHECK(FR(0) == 0, "close");
        w = SW();
        ok = TRIES(RT_CLOSING_CAS, b) == 2 &&
             w == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_CANCEL, C1, RSXCAP_O_DONE, 0);
        break;
    case 8: /* the owner's TERMINAL publication: CANCEL then STOP: 3 attempts, the R34 owner bound, both kept */
        name = "owner TERMINAL (bound 3)";
        arm_on(0, &C1);
        b = step_hits(RT_TERMINAL_CAS);
        inj_arm(RT_TERMINAL_CAS, 2, 1);
        CHECK(FR(0) == 0, "close");
        w = SW();
        ok = TRIES(RT_TERMINAL_CAS, b) == 3 &&
             w == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_CANCEL | RSXCAP_F_STOP, C1, RSXCAP_O_DONE, 0);
        break;
    case 9: /* the detach settle of REQUESTED: a STOP change retried */
        name = "detach settle";
        CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
        b = step_hits(DT_SETTLE_CAS);
        inj_arm(DT_SETTLE_CAS, 1, 0);
        fflush_(&FX[0].c);
        CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0, "detach");
        w = SW();
        ok = TRIES(DT_SETTLE_CAS, b) == 2 &&
             w == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_STOP, C1, RSXCAP_O_ABANDONED, RSXCAP_R_DETACH);
        inj_done();
        CHECK(ok, "%s: attempts and word (%llx)", name, (unsigned long long)w);
        printf("ROW %s attempts %d word %llx\n", name, TRIES(DT_SETTLE_CAS, b), (unsigned long long)w);
        CHECK(ps3dk_prof_gpu_release(H[0], NULL) == 0, "release");
        expect_open(0);
        expect_nocap(C1);
        finish_n(0, 0);
        return;
    }
    inj_done();
    CHECK(ok, "%s: attempts and word (%llx)", name, (unsigned long long)w);
    printf("ROW %s word %llx\n", name, (unsigned long long)w);
    expect_open(which == 2 ? 1 : 0);
    if (which == 2) {
        finish_faulted(0);
        return;
    }
    expect_line("anycaps");
    finish();
}
/* same-cap CANCEL preservation across ARMING, ARMED, CLOSING, TERMINAL and the recycle */
static uint64_t g_seen[4];
static void rec_word(int point)
{
    g_seen[point == RT_ARMED_CAS ? 0 : point == RT_CLOSING_CAS ? 1 : 2] = SW();
    if (point == RT_ARMED_CAS)
        step_arm(RT_CLOSING_CAS, 1);
    else if (point == RT_CLOSING_CAS)
        step_arm(RT_TERMINAL_CAS, 1);
}
static void c_cancel_preserved_same_cap(void)
{
    actor a;
    setup();
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    step_arm(RT_AFTER_CLAIM, 1);
    actor_start(&a, act_cancel_c1); /* CANCEL while ARMING */
    CHECK(FR(0) == 0, "arm");
    actor_join(&a);
    CHECK(SW() == RSXCAP_WORD(RSXCAP_ARMED, RSXCAP_F_CANCEL, C1, 0, 0), "ARMED+CANCEL");
    st_inline = rec_word;
    step_arm(RT_CLOSING_CAS, 1);
    CHECK(FR(0) == 0, "close");
    st_inline = NULL;
    CHECK((g_seen[1] & RSXCAP_F_CANCEL) && RSXCAP_STATE(g_seen[1]) == RSXCAP_ARMED, "the CLOSING attempt loaded CANCEL");
    CHECK((g_seen[2] & RSXCAP_F_CANCEL) && RSXCAP_STATE(g_seen[2]) == RSXCAP_CLOSING, "CLOSING kept CANCEL");
    CHECK(SW() == RSXCAP_WORD(RSXCAP_TERMINAL, RSXCAP_F_CANCEL, C1, RSXCAP_O_DONE, 0), "TERMINAL kept CANCEL");
    CHECK(ps3dk_prof__rsxcap_recycle(C1) == 0 && SW() == RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_CANCEL, C1, 0, 0),
          "the recycle to IDLE(e) kept CANCEL");
    CHECK(REQ(H[0], &C2) == RSXCAP_ACCEPTED && SW() == RSXCAP_WORD(RSXCAP_REQUESTED, 0, C2, 0, 0),
          "only the next claim drops it");
    CHECK(ps3dk_prof__rsxcap_cancel(C2) == 0, "cancel");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 0, 0);
    finish();
}
/* checkpoint descriptors through production registration (5 kinds + BEGIN + checkpoints + END) */
static void ckpt_desc(int variant)
{
    rsxcap_draw dr;
    g_slots = 1024;
    g_maxd = variant == 0 ? 8 : variant == 1 ? 6 : variant == 3 ? 7 : 0;
    setup();
    arm_on(0, &C1);
    draws(0, 4096, RSXCAP_K_ARRAYS);
    drain();
    if (variant >= 2) { /* the first checkpoint's MARK is LOST (the FIFO refuses in that hook) */
        FX[0].refuse = 1;
        FX[0].c.end = FX[0].c.current + 3;
        CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "the 4097th draw (LOST begin)");
        FX[0].refuse = 0;
        FX[0].c.end = FX[0].buf + CBUF;
        ps3dk_prof__rsxcap_draw_end(&dr);
    } else {
        draws(0, 1, RSXCAP_K_ARRAYS);
    }
    if (variant == 3)
        draws(0, 4096, RSXCAP_K_ARRAYS); /* the second checkpoint: its registration is refused (quota) */
    drain();
    CHECK(FR(0) == 0, "boundary");
    rsxcap_outcome o = Q(C1);
    switch (variant) {
    case 0:
        CHECK(o.outcome == RSXCAP_O_DONE && o.end.ckpt_missing == 0, "exact: every descriptor registers, DONE");
        expect_open(0);
        expect_cap(C1, "FINALIZED", 4097, 1);
        break;
    case 1:
        CHECK(o.end.ckpt_missing == 1, "one short: the checkpoint refused, counted once (%u)", o.end.ckpt_missing);
        CHECK(o.outcome == RSXCAP_O_ABANDONED && o.reason == RSXCAP_R_END_DESCRIPTOR, "END also refused at the quota");
        expect_open(0);
        expect_capx(C1, "PARTIAL", "missing END", 4097, 0);
        break;
    case 2:
        CHECK(o.end.ckpt_missing == 1 && o.outcome == RSXCAP_O_DONE, "receipt LOST: counted once (%u)",
              o.end.ckpt_missing);
        expect_open(4);
        expect_capx(C1, "PARTIAL", "missing checkpoints", 4096, 0);
        fprintf(g_exp, "has %u LOSS inside\n", C1);
        break;
    case 3:
        CHECK(o.end.ckpt_missing == 2, "LOST on one checkpoint, refused on another: 2 (%u)", o.end.ckpt_missing);
        CHECK(o.outcome == RSXCAP_O_ABANDONED && o.reason == RSXCAP_R_END_DESCRIPTOR, "END refused at the quota");
        expect_open(4);
        fprintf(g_exp, "cap %u ~ PARTIAL\nhas %u missing END\n", C1, C1);
        break;
    }
    finish();
}
/* pool identity: an app descriptor named rsx.capture.begin in its own file is a separate descriptor */
static void c_pool_identity_vs_app(void)
{
    ps3dk_prof_desc app = {"rsx.capture.begin", "", "rsxcap_rt.c", 4242, 2, 0, 0};
    uint32_t n;
    const ps3dk_prof_desc *pool = ps3dk_prof__rsxcap_pool(&n);
    setup();
    CHECK(ps3dk_prof_gpu_mark(H[0], CTX(0), &app) == 0, "the app MARK registers through the production path");
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    CHECK(FR(0) == 0, "close");
    CHECK(app.state != 0 && pool[0].state != 0 && pool[1].state != 0 && app.state != pool[0].state &&
              app.state < 0xFFFFFFF0u && pool[0].state < 0xFFFFFFF0u,
          "two descriptors: the app's (%u) and the pool's BEGIN (%u)", app.state, pool[0].state);
    CHECK(outcome_is(C1, RSXCAP_O_DONE, 0), "the capture is unaffected");
    expect_open(0);
    expect_cap(C1, "FINALIZED", 2, 0);
    finish_n(1, 1); /* the app MARK under a reserved name with no v1 tag: one anomaly (S1) */
}
/* runtime receipt refusals: no invented seq, event or counts */
static void receipt_refused_rt(int how)
{
    wsnap s;
    rsxcap_draw dr;
    setup();
    arm_on(0, &C1);
    draws(0, 2, RSXCAP_K_ARRAYS);
    drain();
    if (how == 0) { /* anomaly: the drainer latches it (a label outside the window) */
        __atomic_store_n(&BK[0].lab[64], 0x7FFFFFF0u, __ATOMIC_SEQ_CST);
        CHECK(ps3dk_prof_drain() == 0, "the drainer sees the anomaly");
        __atomic_store_n(&BK[0].lab[64], 0u, __ATOMIC_SEQ_CST);
    } else { /* exhausted: a loss with no LOSS credit left */
        ps3dk_prof__test_gpu_ledger_exhaust(1);
        FX[0].refuse = 1;
        FX[0].c.end = FX[0].c.current + 3;
        CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 1, "a LOST draw");
        ps3dk_prof__rsxcap_draw_end(&dr);
        FX[0].refuse = 0;
        FX[0].c.end = FX[0].buf + CBUF;
        draws(0, 1, RSXCAP_K_ARRAYS); /* an EVENT closes the open loss into a record */
        drain(); /* the drainer finds no LOSS credit: exhausted, the sticky admission stop */
    }
    fflush_(&FX[0].c);
    wsnap_take(0, &s);
    dsnap d0 = dom_snap(0);
    CHECK(ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &dr) == 0 && dr.open == 0, "REFUSED: no zone");
    CHECK(wsnap_same(0, &s) && dom_same(0, &d0, 0), "no seq, ticket or GPU word for the refusal");
    CHECK(FR(0) == PS3DK_PROF_ESTOPPED, "the boundary's admission refused");
    rsxcap_outcome o = Q(C1);
    CHECK(o.outcome == RSXCAP_O_ABANDONED && o.reason == RSXCAP_R_DOMAIN_STOPPED, "ABANDONED(domain stopped)");
    CHECK(o.end.c.ref == 1 && o.end.c.ev == (how ? 6u : 4u) && o.end.c.lost == (how ? 2u : 0u),
          "the receipt counts are the real ones: ev %u lost %u ref %u", o.end.c.ev, o.end.c.lost, o.end.c.ref);
    CHECK(o.end.hooks == (how ? 5u : 3u), "hooks %u", o.end.hooks);
    expect_open(4);
    expect_capx(C1, "PARTIAL", "missing END", 2, 0);
    if (how)
        ps3dk_prof__test_gpu_ledger_exhaust(0);
    finish_faulted(0);
}
/* sealed: the owner's hook paused after its ARMED load; the real shutdown seals; the hook's BEGIN is refused */
static rsxcap_draw g_o_dr;
static int g_o_open;
static void o_frame(void) { atomic_store(&o_rc, FR(0)); }
static void o_draw1(void) { g_o_open = ps3dk_prof__rsxcap_draw_begin(CTX(0), RSXCAP_K_ARRAYS, &g_o_dr); if (g_o_open) ps3dk_prof__rsxcap_draw_end(&g_o_dr); }
static void o_draw2(void)
{
    o_draw1();
    o_draw1();
}
static void c_receipt_refused_sealed(void)
{
    osetup(1);
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    own_do(o_frame);
    own_do(o_draw2);
    fflush_(&FX[0].c);
    drain();
    step_arm(HK_AFTER_SLOT_LOAD, 1);
    own_post(o_draw1);
    step_wait_paused();
    dsnap d0 = dom_snap(0);
    expect_open(1);
    int rc = ps3dk_prof_shutdown(); /* STOP, then the seal, while the owner is paused inside its hook */
    printf("shutdown rc %d\n", rc);
    step_go();
    SPIN_UNTIL(atomic_load(&o_done) == atomic_load(&o_seq), "the owner's hook");
    CHECK(g_o_open == 0 && dom_same(0, &d0, 0), "the BEGIN on the sealed domain: REFUSED, no seq or ticket");
    own_post(o_draw1); /* the next hook observes STOP: the stopping settlement, frozen counts */
    SPIN_UNTIL(atomic_load(&o_done) == atomic_load(&o_seq), "the owner's next hook");
    rsxcap_outcome o = Q(C1);
    CHECK(o.outcome == RSXCAP_O_ABANDONED && o.reason == RSXCAP_R_STOPPING, "TERMINAL(ABANDONED, stopping)");
    CHECK(o.end.c.ref == 1 && o.end.c.ev == 4 && o.end.hooks == 3, "ref 1 (the refusal), ev 4, hooks 3 (%u %u %u)",
          o.end.c.ref, o.end.c.ev, o.end.hooks);
    atomic_store(&o_quit, 1);
    pthread_join(o_thr, NULL);
    expect_capx(C1, "PARTIAL", "missing END", 2, 0);
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
/* the real shutdown (STOP, then the seal through gpu_runtime) with the owner paused at P0-P6 */
static void c_seal_point(int pt)
{
    int end_side = pt <= 4;
    osetup(1);
    CHECK(REQ(H[0], &C1) == RSXCAP_ACCEPTED, "request");
    if (end_side) {
        own_do(o_frame);
        CHECK(ST(SW()) == RSXCAP_ARMED, "armed");
        own_do(o_draw2);
        fflush_(&FX[0].c);
        drain();
    }
    step_arm(pt == 0 ? RT_BEFORE_CLOSING : PRG_PT(PRG_SP_P1 + pt - 1), 1);
    own_post(o_frame);
    step_wait_paused();
    fflush_(&FX[0].c); /* the GPU executes what was reserved before the pause (P4: END; P6: BEGIN) */
    expect_open(1);
    int rc = ps3dk_prof_shutdown();
    printf("shutdown rc %d (the owner is paused inside gpu_frame)\n", rc);
    step_go();
    SPIN_UNTIL(atomic_load(&o_done) == atomic_load(&o_seq), "the owner's boundary");
    rsxcap_outcome o = Q(C1);
    if (pt == 0)
        CHECK(o.outcome == RSXCAP_O_ABANDONED && o.reason == RSXCAP_R_STOPPING, "P0: STOP before CLOSING");
    else if (pt == 1)
        CHECK(o.outcome == RSXCAP_O_ABANDONED && o.reason == RSXCAP_R_DOMAIN_STOPPED, "P1: admission refused");
    else if (end_side)
        CHECK(o.outcome == RSXCAP_O_DONE, "P%d: the admitted END decides: DONE (%u %u)", pt, o.outcome, o.reason);
    else
        CHECK(o.outcome == RSXCAP_O_ABANDONED && o.reason == RSXCAP_R_STOPPING && o.begin_r.kind == PRG_R_EVENT,
              "P%d: BEGIN enqueued, STOP at the ARMED publication", pt);
    atomic_store(&o_quit, 1);
    pthread_join(o_thr, NULL);
    /* the file: the frozen frontier (R85) */
    if (pt == 4)
        expect_cap(C1, "FINALIZED", 2, 0); /* END published and executed before the seal */
    else if (pt == 5)
        expect_nocap(C1); /* BEGIN beyond the frontier */
    else if (pt == 6)
        expect_capx(C1, "PARTIAL", "missing END", 0, 0);
    else
        expect_capx(C1, "PARTIAL", "missing END", 2, 0); /* P0-P3: no END in the file (DONE at P2/P3 asserted) */
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
/* the registered-owner prerequisite (a carry to S3/S5): an unregistered owner's domain is not capable */
static void c_attach_unregistered_not_capable(void)
{
    fx_init(0, 0, 0, g_slots, 64);
    CHECK(start(2, g_slots) == 0, "start");
    CHECK(ps3dk_prof__rsxcap_enable() == 0, "enable");
    CHECK(ps3dk_prof_gpu_attach(CTX(0), 0, g_slots, 64, &H[0]) == 0, "attach from an unregistered thread");
    CHECK(ps3dk_prof__test_rsxcap_capable(0) == 0, "not capable (no kind registration under the mutex)");
    CHECK(REQ(H[0], &C1) == RSXCAP_REFUSED && C1 == 0 && g_reason == RSXCAP_R_NO_CAPABILITY, "REFUSED(no capability)");
    expect_open(0);
    expect_line("kinds 0");
    fflush_(&FX[0].c);
    CHECK(ps3dk_prof_gpu_detach(H[0], CTX(0), 3000000) == 0 && ps3dk_prof_gpu_release(H[0], NULL) == 0, "cycle");
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
/* THREAD mode: the real drain thread concurrent with the capture and the shutdown (the device-memory seam) */
static int thread_mode_ok(void)
{
#ifndef GPP_HOST_DEVMEM
    CHECK(0, "THREAD-mode cases need the device-memory seam (the TSan build)");
    return 0;
#else
    g_thread_mode = 1;
    return 1;
#endif
}
/* wait (bounded) until the drain thread has accepted n events of domain 0 */
static void wait_events(uint64_t n)
{
    ps3dk_prof_gpu_stats st;
    long i;
    for (i = 0;; i++) {
        if (ps3dk_prof_gpu_get_stats(H[0], &st) == 0 && st.events >= n)
            return;
        if (i >= 10000000L)
            harness_stall("waiting for the drain thread");
        sched_yield();
    }
}
static void c_thread_capture_basic(void)
{
    if (!thread_mode_ok())
        return;
    setup();
    for (int k = 0; k < 3; k++) {
        uint32_t c;
        CHECK(REQ(H[0], &c) == RSXCAP_ACCEPTED && FR(0) == 0, "arm %d", k);
        draws(0, 60, RSXCAP_K_INDEX); /* BEGIN + 120 + END: inside the 256-slot ring */
        fflush_(&FX[0].c);
        CHECK(FR(0) == 0 && outcome_is(c, RSXCAP_O_DONE, 0) && ps3dk_prof__rsxcap_recycle(c) == 0, "close %d", k);
        fflush_(&FX[0].c);
        wait_events(122u * (uint64_t)(k + 1)); /* the drain thread, concurrent, frees the ring */
    }
    expect_open(0);
    for (uint32_t c = 1; c <= 3; c++)
        expect_cap(c, "FINALIZED", 60, 0);
    finish();
}
static void c_thread_capture_shutdown(void)
{
    if (!thread_mode_ok())
        return;
    setup();
    arm_on(0, &C1);
    draws(0, 100, RSXCAP_K_ARRAYS); /* inside the ring */
    fflush_(&FX[0].c);
    expect_open(0);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown while ARMED: the drain thread seals");
    CHECK(Q(C1).outcome == RSXCAP_O_ARMED_STOPPED, "ARMED+STOP with no later owner call");
    expect_capx(C1, "PARTIAL", "missing END", 100, 0);
    fprintf(g_exp, "anomalies 0\n");
    fclose(g_exp);
    CHECK(take_capture() == 0, "capture");
}
static void c_thread_other_stress(void)
{
    pthread_t t[2];
    if (!thread_mode_ok())
        return;
    g_slots = 1024;
    setup();
    for (int i = 0; i < 2; i++)
        pthread_create(&t[i], NULL, stress_hook, NULL);
    atomic_store(&g_hooks_go, 1);
    for (int f = 0; f < 300; f++) {
        agent_turn();
        CHECK(FR(0) == 0, "frame %d", f);
        draws(0, 32, RSXCAP_K_INDEX);
        fflush_(&FX[0].c);
    }
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    CHECK(FR(0) == 0, "last boundary");
    expect_open(0);
    expect_line("anycaps");
    finish();
}

/* ================================================================ the harness controls */
static void c_harness_stall(void)
{
    g_spin_max = 1000000;
    step_wait_paused(); /* nobody pauses: HARNESS-STALL, exit 3 */
}
static void c_harness_crash(void)
{
    volatile int *z = NULL;
    *z = 1;
}

/* ================================================================ dispatch */
typedef struct tcase {
    const char *name;
    void (*fn)(void);
} tcase;
#define WRAP(name, call)                                                                                               \
    static void w_##name(void) { call; }
WRAP(receipt_suppressed, c_receipt_suppressed(64))
WRAP(receipt_suppressed_control, c_receipt_suppressed(63))
WRAP(receipt_lost_records, lost_records(16, 1))
WRAP(receipt_lost_records_control, lost_records(15, 0))
WRAP(arm_admission_refused, c_arm_admission_refused(0))
WRAP(end_admission_refused, c_arm_admission_refused(1))
WRAP(begin_desc_refused, desc_case(0, 0))
WRAP(begin_desc_exact, desc_case(0, 1))
WRAP(end_desc_refused, desc_case(1, 0))
WRAP(end_desc_exact, desc_case(1, 1))
WRAP(begin_receipt_lost, receipt_case(1, 0))
WRAP(begin_receipt_refused, receipt_case(1, 1))
WRAP(end_receipt_lost, receipt_case(0, 0))
WRAP(end_receipt_refused, receipt_case(0, 1))
WRAP(stop_armed_hook, stop_armed(0))
WRAP(stop_armed_frame, stop_armed(1))
WRAP(stop_armed_detach, stop_armed(2))
WRAP(stop_claiming_before_fields, stop_claiming(AG_AFTER_CLAIM_CAS))
WRAP(stop_claiming_after_fields, stop_claiming(AG_CLAIMING))
WRAP(stop_at_claim_publication, stop_claiming(AG_PUB_LOAD))
WRAP(stop_vs_new_epoch_cas_a, stop_new_epoch(0))
WRAP(stop_vs_new_epoch_cas_b, stop_new_epoch(1))
WRAP(stop_after_publication, c_stop_after_publication(0))
WRAP(stop_after_revalidation, c_stop_after_publication(1))
WRAP(stop_then_stale_owner_cas_b1, stop_stale_owner(0))
WRAP(stop_then_stale_owner_cas_b2, stop_stale_owner(1))
WRAP(stop_cut_before_cas, stop_cut(RT_BEFORE_CLOSING))
WRAP(stop_cut_after_cas, stop_cut(RT_AFTER_FREEZE))
WRAP(stop_cut_after_reg, stop_cut(RT_AFTER_END_DESC))
WRAP(stop_cut_after_admit, stop_cut(PRG_PT(PRG_SP_P2)))
WRAP(race_arm_cancel_a, c_race_arm_cancel(1))
WRAP(race_arm_cancel_b, c_race_arm_cancel(0))
WRAP(sat_other_ctx, sat16_case(0))
WRAP(sat_immediate, sat16_case(1))
WRAP(cap_262144, cap_case(262144))
WRAP(cap_262145, cap_case(262145))
WRAP(block_4096, block_case(4096))
WRAP(block_4097, block_case(4097))
WRAP(empty_frame, block_case(0))
WRAP(other_thread_65535, c_other_thread_n(65535))
WRAP(other_thread_65536, c_other_thread_n(65536))
WRAP(other_thread_freeze_race_a, c_other_thread_freeze_race(1))
WRAP(other_thread_freeze_race_b, c_other_thread_freeze_race(0))
WRAP(fault_settles_now, c_fault_settles_now(0))
WRAP(fault_settles_now_control, c_fault_settles_now(1))
WRAP(agent_unlock_fail_armed_won, c_agent_unlock_fail_arming_won(1))
WRAP(agent_unlock_fail_arming_won, c_agent_unlock_fail_arming_won(0))
WRAP(detach_vs_validation, detach_vs_claim(AG_AFTER_SCAN, 0))
WRAP(detach_vs_validation_control, detach_vs_claim(0, 0))
WRAP(detach_during_claiming, detach_vs_claim(AG_CLAIMING, 0))
WRAP(domain0_vs_detach, detach_vs_claim(AG_AFTER_SCAN, 1))
WRAP(other_thread_stress, c_other_thread_stress(0))
WRAP(pub_bound_0, pub_bound(0))
WRAP(pub_bound_1, pub_bound(1))
WRAP(pub_bound_2, pub_bound(2))
WRAP(pub_bound_3, pub_bound(3))
WRAP(pub_bound_4, pub_bound(4))
WRAP(pub_bound_5, pub_bound(5))
WRAP(pub_bound_6, pub_bound(6))
WRAP(pub_bound_7, pub_bound(7))
WRAP(pub_bound_8, pub_bound(8))
WRAP(pub_bound_9, pub_bound(9))
WRAP(ckpt_desc_0, ckpt_desc(0))
WRAP(ckpt_desc_1, ckpt_desc(1))
WRAP(ckpt_desc_2, ckpt_desc(2))
WRAP(ckpt_desc_3, ckpt_desc(3))
WRAP(rr_anomaly, receipt_refused_rt(0))
WRAP(rr_exhausted, receipt_refused_rt(1))
#define SEALP(n) WRAP(seal_P##n, c_seal_point(n))
SEALP(0) SEALP(1) SEALP(2) SEALP(3) SEALP(4) SEALP(5) SEALP(6)
WRAP(neg_plain_other, c_other_thread_stress(1))
WRAP(slot_churn, c_slot_churn(0))
WRAP(neg_late_fields, c_slot_churn(1))
WRAP(enable_vs_attach_1, c_enable_vs_attach(1))
WRAP(enable_vs_attach_0, c_enable_vs_attach(0))
#define PF(f, a) WRAP(prepare_fail_F##f##_##a, c_prepare_fail(f, #a[0] == 'a'))
PF(1, requested) PF(2, requested) PF(3, requested) PF(4, requested) PF(5, requested) PF(6, requested)
PF(7, requested) PF(1, armed) PF(2, armed) PF(3, armed) PF(4, armed) PF(5, armed) PF(6, armed) PF(7, armed)
#define SP(n) WRAP(stop_P##n, c_stop_point(n))
SP(0) SP(1) SP(2) SP(3) SP(4) SP(5) SP(6)

static const tcase CASES[] = {
    {"receipt_event", c_receipt_event},
    {"receipt_lost_fifo", c_receipt_lost_fifo},
    {"receipt_lost_ring", c_receipt_lost_ring},
    {"receipt_lost_records", w_receipt_lost_records},
    {"receipt_lost_records_control", w_receipt_lost_records_control},
    {"receipt_suppressed", w_receipt_suppressed},
    {"receipt_suppressed_control", w_receipt_suppressed_control},
    {"receipt_refused_faulted", c_receipt_refused_faulted},
    {"plain_path_identity", c_plain_path_identity},
    {"close_done", c_close_done},
    {"close_none_token", c_close_none_token},
    {"arm_basic", c_arm_basic},
    {"arm_none_token", c_arm_none_token},
    {"arm_other_domain", c_arm_other_domain},
    {"slot_claim_basic", c_slot_claim_basic},
    {"slot_capacity_16_17", c_slot_capacity_16_17},
    {"slot_req_refusals", c_slot_req_refusals},
    {"slot_req_no_capability", c_slot_req_no_capability},
    {"slot_req_busy", c_slot_req_busy},
    {"slot_req_busy_steps", c_slot_req_busy_steps},
    {"slot_cancel_before_arm", c_slot_cancel_before_arm},
    {"slot_domain0", c_slot_domain0},
    {"recycle", c_recycle},
    {"recycle_wrong_cap", c_recycle_wrong_cap},
    {"cancel_stale", c_cancel_stale},
    {"two_conn_wire1", c_two_conn_wire1},
    {"record_immutable", c_record_immutable},
    {"terminal_record_immutable", c_terminal_record_immutable},
    {"prepare_fail_F1_requested", w_prepare_fail_F1_requested},
    {"prepare_fail_F2_requested", w_prepare_fail_F2_requested},
    {"prepare_fail_F3_requested", w_prepare_fail_F3_requested},
    {"prepare_fail_F4_requested", w_prepare_fail_F4_requested},
    {"prepare_fail_F5_requested", w_prepare_fail_F5_requested},
    {"prepare_fail_F6_requested", w_prepare_fail_F6_requested},
    {"prepare_fail_F7_requested", w_prepare_fail_F7_requested},
    {"prepare_fail_F1_armed", w_prepare_fail_F1_armed},
    {"prepare_fail_F2_armed", w_prepare_fail_F2_armed},
    {"prepare_fail_F3_armed", w_prepare_fail_F3_armed},
    {"prepare_fail_F4_armed", w_prepare_fail_F4_armed},
    {"prepare_fail_F5_armed", w_prepare_fail_F5_armed},
    {"prepare_fail_F6_armed", w_prepare_fail_F6_armed},
    {"prepare_fail_F7_armed", w_prepare_fail_F7_armed},
    {"arm_admission_refused", w_arm_admission_refused},
    {"end_admission_refused", w_end_admission_refused},
    {"begin_desc_refused", w_begin_desc_refused},
    {"begin_desc_exact", w_begin_desc_exact},
    {"end_desc_refused", w_end_desc_refused},
    {"end_desc_exact", w_end_desc_exact},
    {"end_lock_fault", c_end_lock_fault},
    {"stop_before_cas", c_stop_before_cas},
    {"claim_lost_to_reclaim", c_claim_lost_to_reclaim},
    {"inspect_then_lose", c_inspect_then_lose},
    {"stop_before_armed_pub", c_stop_before_armed_pub},
    {"stop_between_cas_and_begin", c_stop_between_cas_and_begin},
    {"cancel_during_arming", c_cancel_during_arming},
    {"begin_receipt_lost", w_begin_receipt_lost},
    {"begin_receipt_refused", w_begin_receipt_refused},
    {"end_receipt_lost", w_end_receipt_lost},
    {"end_receipt_refused", w_end_receipt_refused},
    {"detach_requested", c_detach_requested},
    {"detach_armed", c_detach_armed},
    {"domain_released_while_requested", c_domain_released_while_requested},
    {"stop_armed_hook", w_stop_armed_hook},
    {"stop_armed_frame", w_stop_armed_frame},
    {"stop_armed_detach", w_stop_armed_detach},
    {"stop_no_owner", c_stop_no_owner},
    {"stop_mid_zone", c_stop_mid_zone},
    {"cancel_armed", c_cancel_armed},
    {"cancel_closing", c_cancel_closing},
    {"cancel_armed_recycle_next", c_cancel_armed_recycle_next},
    {"shutdown_sets_stop", c_shutdown_sets_stop},
    {"stop_idle_then_request", c_stop_idle_then_request},
    {"stop_idle_vs_request", c_stop_idle_vs_request},
    {"stop_claiming_before_fields", w_stop_claiming_before_fields},
    {"stop_claiming_after_fields", w_stop_claiming_after_fields},
    {"stop_at_claim_publication", w_stop_at_claim_publication},
    {"stop_vs_cancel", c_stop_vs_cancel},
    {"stop_vs_recycle", c_stop_vs_recycle},
    {"stop_vs_new_epoch_cas_a", w_stop_vs_new_epoch_cas_a},
    {"stop_vs_new_epoch_cas_b", w_stop_vs_new_epoch_cas_b},
    {"stop_after_publication", w_stop_after_publication},
    {"stop_after_revalidation", w_stop_after_revalidation},
    {"stop_withdraw_vs_owner_after_stop", c_stop_withdraw_vs_owner_after_stop},
    {"query_armed_stop_no_mutation", c_query_armed_stop_no_mutation},
    {"stop_after_arming", c_stop_after_arming},
    {"stop_then_stale_owner_cas_b1", w_stop_then_stale_owner_cas_b1},
    {"stop_then_stale_owner_cas_b2", w_stop_then_stale_owner_cas_b2},
    {"stop_cut_before_cas", w_stop_cut_before_cas},
    {"stop_cut_after_cas", w_stop_cut_after_cas},
    {"stop_cut_after_reg", w_stop_cut_after_reg},
    {"stop_cut_after_admit", w_stop_cut_after_admit},
    {"stale_owner_after_reclaim", c_stale_owner_after_reclaim},
    {"race_arm_cancel_a", w_race_arm_cancel_a},
    {"race_arm_cancel_b", w_race_arm_cancel_b},
    {"draw_basic", c_draw_basic},
    {"sat_other_ctx", w_sat_other_ctx},
    {"sat_immediate", w_sat_immediate},
    {"sat_five", c_sat_five},
    {"cap_262144", w_cap_262144},
    {"cap_262145", w_cap_262145},
    {"block_4096", w_block_4096},
    {"block_4097", w_block_4097},
    {"empty_frame", w_empty_frame},
    {"draw_begin_refused_block", c_draw_begin_refused_block},
    {"ckpt_mark_depth", c_ckpt_mark_depth},
    {"counts_request_local", c_counts_request_local},
    {"after_cap_counting", c_after_cap_counting},
    {"other_thread_65535", w_other_thread_65535},
    {"other_thread_65536", w_other_thread_65536},
    {"other_thread_contention", c_other_thread_contention},
    {"other_thread_freeze_race_a", w_other_thread_freeze_race_a},
    {"other_thread_freeze_race_b", w_other_thread_freeze_race_b},
    {"other_thread_stale_cas", c_other_thread_stale_cas},
    {"other_thread_old_cap", c_other_thread_old_cap},
    {"hook_reads_old_record", c_hook_reads_old_record},
    {"enable_before_init", c_enable_before_init},
    {"enable_no_gpu", c_enable_no_gpu},
    {"enable_late", c_enable_late},
    {"enable_twice", c_enable_twice},
    {"enable_after_shutdown", c_enable_after_shutdown},
    {"enable_registers_kinds", c_enable_registers_kinds},
    {"kinds_refused_not_capable", c_kinds_refused_not_capable},
    {"not_enabled", c_not_enabled},
    {"fault_settles_now", w_fault_settles_now},
    {"fault_settles_now_control", w_fault_settles_now_control},
    {"fault_sticky_calls", c_fault_sticky_calls},
    {"fault_no_recovery", c_fault_no_recovery},
    {"fault_then_detach", c_fault_then_detach},
    {"fault_never_drained", c_fault_never_drained},
    {"fault_drained_prefix", c_fault_drained_prefix},
    {"ckpt_publish_fault_settles", c_ckpt_publish_fault_settles},
    {"stop_P0", w_stop_P0},
    {"stop_P1", w_stop_P1},
    {"stop_P2", w_stop_P2},
    {"stop_P3", w_stop_P3},
    {"stop_P4", w_stop_P4},
    {"stop_P5", w_stop_P5},
    {"stop_P6", w_stop_P6},
    {"request_reentry_refused", c_request_reentry_refused},
    {"request_from_non_owner_ok", c_request_from_non_owner_ok},
    {"request_lock_fault", c_request_lock_fault},
    {"agent_unlock_fail_withdrawable", c_agent_unlock_fail_withdrawable},
    {"agent_unlock_fail_armed_won", w_agent_unlock_fail_armed_won},
    {"agent_unlock_fail_arming_won", w_agent_unlock_fail_arming_won},
    {"fault_settles_with_flags", c_fault_settles_with_flags},
    {"detach_vs_validation", w_detach_vs_validation},
    {"detach_vs_validation_control", w_detach_vs_validation_control},
    {"detach_during_claiming", w_detach_during_claiming},
    {"domain0_vs_detach", w_domain0_vs_detach},
    {"detach_vs_publication_a", c_detach_vs_publication_a},
    {"detach_vs_publication_b", c_detach_vs_publication_b},
    {"detach_vs_publication_both", c_detach_vs_publication_both},
    {"no_stale_slot_reuse", c_no_stale_slot_reuse},
    {"fault_vs_publication", c_fault_vs_publication},
    {"domain0_attach_between_scan_and_claim", c_domain0_attach_between_scan_and_claim},
    {"domain0_vs_attach_a", c_domain0_vs_attach_a},
    {"domain0_release_vs_resolution", c_domain0_release_vs_resolution},
    {"enable_vs_attach_enable_first", w_enable_vs_attach_1},
    {"enable_vs_attach_attach_first", w_enable_vs_attach_0},
    {"begin_publish_fail", c_begin_publish_fail},
    {"agent_unlock_fail_stop_at_withdraw", c_agent_unlock_fail_stop_at_withdraw},
    {"early_terminal_query", c_early_terminal_query},
    {"query_stale_refused", c_query_stale_refused},
    {"end_publish_fail", c_end_publish_fail},
    {"fault_abandon_refused", c_fault_abandon_refused},
    {"fault_resources_retained", c_fault_resources_retained},
    {"other_thread_stress", w_other_thread_stress},
    {"neg_plain_other", w_neg_plain_other},
    {"slot_churn", w_slot_churn},
    {"neg_late_fields", w_neg_late_fields},
    {"request_vs_shutdown", c_request_vs_shutdown},
    {"stop_service", c_stop_service},
    {"claim_vs_detach", c_claim_vs_detach},
    {"pool_tags_once", c_pool_tags_once},
    {"request_pin_saturated", c_request_pin_saturated},
    {"request_pin_contention", c_request_pin_contention},
    {"ext_fault_open_draw", c_ext_fault_open_draw},
    {"ckpt_lock_fault_stops_hook", c_ckpt_lock_fault_stops_hook},
    {"pub_bound_agent_publication", w_pub_bound_0},
    {"pub_bound_agent_claim", w_pub_bound_1},
    {"pub_bound_agent_withdrawal", w_pub_bound_2},
    {"pub_bound_agent_cancel", w_pub_bound_3},
    {"pub_bound_agent_recycle", w_pub_bound_4},
    {"pub_bound_owner_claim", w_pub_bound_5},
    {"pub_bound_owner_armed", w_pub_bound_6},
    {"pub_bound_owner_closing", w_pub_bound_7},
    {"pub_bound_owner_terminal", w_pub_bound_8},
    {"pub_bound_detach_settle", w_pub_bound_9},
    {"cancel_preserved_same_cap", c_cancel_preserved_same_cap},
    {"ckpt_desc_exact", w_ckpt_desc_0},
    {"ckpt_desc_refused", w_ckpt_desc_1},
    {"ckpt_missing_lost", w_ckpt_desc_2},
    {"ckpt_missing_once", w_ckpt_desc_3},
    {"pool_identity_vs_app", c_pool_identity_vs_app},
    {"receipt_refused_anomaly", w_rr_anomaly},
    {"receipt_refused_exhausted", w_rr_exhausted},
    {"receipt_refused_sealed", c_receipt_refused_sealed},
    {"seal_P0", w_seal_P0},
    {"seal_P1", w_seal_P1},
    {"seal_P2", w_seal_P2},
    {"seal_P3", w_seal_P3},
    {"seal_P4", w_seal_P4},
    {"seal_P5", w_seal_P5},
    {"seal_P6", w_seal_P6},
    {"attach_unregistered_not_capable", c_attach_unregistered_not_capable},
    {"thread_capture_basic", c_thread_capture_basic},
    {"thread_capture_shutdown", c_thread_capture_shutdown},
    {"thread_other_stress", c_thread_other_stress},
    {"harness_stall", c_harness_stall},
    {"harness_crash", c_harness_crash},
};

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++)
            printf("%s\n", CASES[i].name);
        return 0;
    }
    if (argc < 3)
        return 2;
    g_case_name = argv[1];
    g_out = argv[2];
    for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++)
        if (!strcmp(CASES[i].name, g_case_name)) {
            CASES[i].fn();
            if (g_bad)
                return 1;
            printf("RT %s PASS\n", g_case_name);
            return 0;
        }
    printf("RT %s FAIL unknown case\n", g_case_name);
    return 2;
}
