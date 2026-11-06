/* Capture RSX frame S2: the core harness (prg.c standalone over an emulator port, the R1 pattern).  Receipts,
 * prg_frame_capture's one admission, prg_zone_abandon, the hook-fault latch and its first-fault reason, and the seal
 * at the six points P1-P6 (a second thread seals while the producer is paused at the step point).  Host model only.
 *
 * One case per process.  Prints "CORE <case> PASS" or "CORE <case> FAIL ...".  usage: rsxcap_core <case> */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prg.h"
#include "rsxcap_steps.h"

static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("CORE %s FAIL line %d: ", g_case_name, __LINE__);                                                  \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---------------------------------------------------------------- the emulator (pairs execute at reserve) */
static pthread_mutex_t g_em = PTHREAD_MUTEX_INITIALIZER, g_snap = PTHREAD_MUTEX_INITIALIZER,
                       g_ext = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_timer[1024];
static uint32_t g_label[256];
static uint64_t g_clock = 1000, g_tb = 5;
static int g_reserve_fail;       /* reserve_pair refuses (FIFO) */
static atomic_int g_snap_unlock_fail_at, g_snap_lock_fail_at, g_snap_ops; /* the nth snap op fails (1-based) */
static atomic_uint g_words;       /* pairs reserved (GPU words written) */

static int e_reserve(void *c, uint32_t r, uint32_t l, uint32_t v)
{
    (void)c;
    if (g_reserve_fail)
        return -1;
    pthread_mutex_lock(&g_em);
    g_clock += 37;
    g_timer[r] = g_clock;
    g_label[l] = v;
    pthread_mutex_unlock(&g_em);
    atomic_fetch_add(&g_words, 1);
    return 0;
}
static void e_flush(void *c) { (void)c; }
static uint32_t e_rlabel(void *c, uint32_t l)
{
    uint32_t v;
    (void)c;
    pthread_mutex_lock(&g_em);
    v = g_label[l];
    pthread_mutex_unlock(&g_em);
    return v;
}
static void e_wlabel(void *c, uint32_t l, uint32_t v)
{
    (void)c;
    pthread_mutex_lock(&g_em);
    g_label[l] = v;
    pthread_mutex_unlock(&g_em);
}
static uint64_t e_rtimer(void *c, uint32_t r)
{
    uint64_t v;
    (void)c;
    pthread_mutex_lock(&g_em);
    v = g_timer[r];
    pthread_mutex_unlock(&g_em);
    return v;
}
static void e_wtimer(void *c, uint32_t r, uint64_t v)
{
    (void)c;
    pthread_mutex_lock(&g_em);
    g_timer[r] = v;
    pthread_mutex_unlock(&g_em);
}
static void e_sync(void *c) { (void)c; }
static uint64_t e_mftb(void *c)
{
    (void)c;
    return __atomic_add_fetch(&g_tb, 11, __ATOMIC_RELAXED);
}
static int e_now(void *c, uint64_t *us)
{
    static uint64_t now;
    (void)c;
    *us = __atomic_add_fetch(&now, 10, __ATOMIC_RELAXED); /* every read advances: deadlines are finite */
    return 0;
}
static int e_slock(void *c)
{
    (void)c;
    int n = atomic_fetch_add(&g_snap_ops, 1) + 1;
    if (atomic_load(&g_snap_lock_fail_at) == n)
        return -1;
    return pthread_mutex_lock(&g_snap);
}
static int e_sunlock(void *c)
{
    (void)c;
    int n = atomic_fetch_add(&g_snap_ops, 1) + 1;
    pthread_mutex_unlock(&g_snap);
    return atomic_load(&g_snap_unlock_fail_at) == n ? -1 : 0; /* the real unlock, then the error */
}
static int e_elock(void *c)
{
    (void)c;
    return pthread_mutex_lock(&g_ext);
}
static int e_eunlock(void *c)
{
    (void)c;
    return pthread_mutex_unlock(&g_ext);
}

/* the sink: decoded GPU_EVENTS (desc, kind, seq), LOSS ranges, FINAL */
typedef struct ev {
    uint64_t seq, frame;
    uint32_t desc, owner;
    uint8_t kind;
} ev;
static ev g_ev[4096];
static int g_nev;
static uint64_t g_loss_first[64], g_loss_count[64];
static int g_nloss, g_final_state = -1;
static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint64_t rd64(const uint8_t *b) { return (uint64_t)rd32(b) | (uint64_t)rd32(b + 4) << 32; }
static int o_chunk(void *c, uint32_t type, const uint8_t *p, size_t n)
{
    (void)c;
    (void)n;
    if (type == 14) {
        uint64_t first = rd64(p + 8);
        uint32_t k = rd32(p + 16);
        for (uint32_t i = 0; i < k && g_nev < 4096; i++) {
            const uint8_t *e = p + 20 + 28 * i;
            g_ev[g_nev++] = (ev){first + i, rd64(e + 20), rd32(e + 8), rd32(e + 16), e[12]};
        }
    } else if (type == 15 && g_nloss < 64) {
        g_loss_first[g_nloss] = rd64(p + 8);
        g_loss_count[g_nloss++] = rd64(p + 16);
    } else if (type == 16) {
        g_final_state = p[9];
    }
    return 0;
}

static const prg_port PORT = {NULL, e_reserve, e_flush, e_rlabel, e_wlabel, e_rtimer, e_wtimer, e_sync, e_mftb,
                              e_now, e_slock, e_sunlock, e_elock, e_eunlock, NULL};
static const prg_out OUT = {NULL, o_chunk};
static prg_dom D;
static prg_meta META[1024]; /* report_count entries: the largest case attaches 1024 */
static prg_stage STAGE[1024];
static uint8_t SCR[PRG_SCRATCH_FOR(1024)];
static prg_ledger L;
static uint32_t g_desc_count = 8;

/* the step seam: admission accounting per point, plus the shared pause */
static atomic_int g_cnt[32];
void prg_test_step(struct prg_dom *d, int point)
{
    (void)d;
    if (point > 0 && point < 32)
        atomic_fetch_add(&g_cnt[point], 1);
    step_hit(point);
}
static void cnt_reset(void)
{
    for (int i = 0; i < 32; i++)
        atomic_store(&g_cnt[i], 0);
}
#define TRIES atomic_load(&g_cnt[PRG_SP_ADMIT_TRY])
#define ADMITS atomic_load(&g_cnt[PRG_SP_ADMITTED])
#define LEAVES atomic_load(&g_cnt[PRG_SP_LEAVE])

static void setup(uint32_t slots)
{
    memset(&L, 0, sizeof L);
    L.loss_max = 4096;
    L.br_epoch_max = 16;
    L.br_file_max = 64;
    CHECK(prg_attach(&D, 1026, 0, slots, 64, &PORT, &OUT, &L, META, STAGE, SCR, slots) == PRG_OK, "attach");
    D.desc_count = &g_desc_count;
    cnt_reset();
    g_nev = g_nloss = 0;
    g_final_state = -1;
}
static uint32_t inflight(void) { return __atomic_load_n(&D.inflight, __ATOMIC_SEQ_CST); }
static int rk(const prg_receipt *r, uint32_t kind, uint32_t reason) { return r->kind == kind && r->reason == reason; }

/* ---------------------------------------------------------------- receipts */
static void stop_by(const char *how)
{
    if (!strcmp(how, "sealed")) {
        prg_seal(&D);
        CHECK(__atomic_load_n(&D.state, __ATOMIC_SEQ_CST) == PRG_SEALED, "sealed");
    } else if (!strcmp(how, "detaching") || !strcmp(how, "sealing")) {
        __atomic_store_n(&D.state, !strcmp(how, "detaching") ? PRG_DETACHING : PRG_SEALING, __ATOMIC_SEQ_CST);
    } else if (!strcmp(how, "anomaly")) {
        __atomic_store_n(&D.anomaly, 1u, __ATOMIC_RELEASE);
    } else if (!strcmp(how, "overflow")) {
        __atomic_store_n(&D.overflow, 1u, __ATOMIC_RELEASE);
    } else if (!strcmp(how, "exhausted")) {
        __atomic_store_n(&D.exhausted, 1u, __ATOMIC_RELEASE);
    } else if (!strcmp(how, "faulted")) {
        prg_hook_fault(&D);
    }
}
static void c_receipt_refused(const char *how)
{
    prg_token t;
    prg_receipt r = {9, 9, 9};
    uint64_t s0, t0;
    setup(16);
    stop_by(how);
    s0 = D.p_seq;
    t0 = D.p_ticket;
    CHECK(prg_begin_r(&D, 1, &t, &r) == PRG_ESTOPPED && rk(&r, PRG_R_REFUSED, PRG_RR_NOT_ADMITTED) && r.seq == 0,
          "begin REFUSED(not admitted), no seq (%u %u)", r.kind, r.reason);
    CHECK(prg_mark_r(&D, 1, &r) == PRG_ESTOPPED && rk(&r, PRG_R_REFUSED, PRG_RR_NOT_ADMITTED), "mark refused");
    CHECK(D.p_seq == s0 && D.p_ticket == t0 && inflight() == 0, "nothing numbered or submitted");
}
static void c_receipt_einval(void)
{
    prg_token a, b;
    prg_receipt r;
    setup(16);
    CHECK(prg_begin_r(&D, 1, &a, &r) == PRG_OK && rk(&r, PRG_R_EVENT, 0) && r.seq == 0, "begin a EVENT(0)");
    CHECK(prg_begin_r(&D, 2, &b, &r) == PRG_OK && rk(&r, PRG_R_EVENT, 0) && r.seq == 1, "begin b EVENT(1)");
    CHECK(prg_end_r(&D, &a, 1, &r) == PRG_EINVAL && rk(&r, PRG_R_REFUSED, PRG_RR_EINVAL), "end of a non-top token");
    CHECK(D.p_seq == 2, "no number for the refusal");
    CHECK(prg_end_r(&D, &b, 2, &r) == PRG_OK && rk(&r, PRG_R_EVENT, 0) && r.seq == 2, "control: the top ends EVENT");
}
static void c_receipt_seq_overflow(void)
{
    prg_receipt r;
    setup(16);
    D.p_seq = UINT64_MAX - 1u;
    CHECK(prg_mark_r(&D, 1, &r) == PRG_OK && rk(&r, PRG_R_EVENT, 0) && r.seq == UINT64_MAX - 1u, "one EVENT");
    CHECK(prg_mark_r(&D, 1, &r) == PRG_EOVERFLOW && rk(&r, PRG_R_REFUSED, PRG_RR_SEQ_OVERFLOW), "then REFUSED");
}
static void c_receipt_suppress_overflow(void)
{
    prg_token t[PRG_DEPTH + PRG_SUPPRESS_MAX + 1];
    prg_receipt r;
    setup(1024);
    for (uint32_t i = 0; i < PRG_DEPTH; i++)
        CHECK(prg_begin_r(&D, 1, &t[i], &r) == PRG_OK && r.kind == PRG_R_EVENT, "depth %u", i);
    for (uint32_t i = 0; i < PRG_SUPPRESS_MAX - 1; i++)
        CHECK(prg_begin_r(&D, 1, &t[PRG_DEPTH + i], &r) == PRG_OK && rk(&r, PRG_R_SUPPRESSED, 0) && r.seq == 0,
              "suppressed level %u", i);
    CHECK(prg_begin_r(&D, 1, &t[PRG_DEPTH + 63], &r) == PRG_OK && rk(&r, PRG_R_SUPPRESSED, 0),
          "control: the 64th suppressed level is SUPPRESSED");
    CHECK(prg_begin_r(&D, 1, &t[PRG_DEPTH + 64], &r) == PRG_EOVERFLOW && rk(&r, PRG_R_REFUSED, PRG_RR_SUPPRESS_OVERFLOW),
          "the 65th: REFUSED(suppress overflow)");
    CHECK(prg_end_r(&D, &t[PRG_DEPTH + 63], 1, &r) == PRG_OK && rk(&r, PRG_R_SUPPRESSED, 0), "suppressed end");
}
static void c_receipt_mark_depth(void)
{
    prg_token t[PRG_DEPTH];
    prg_receipt r;
    setup(1024);
    for (uint32_t i = 0; i < PRG_DEPTH - 1; i++)
        (void)prg_begin_r(&D, 1, &t[i], &r);
    CHECK(prg_mark_r(&D, 2, &r) == PRG_OK && r.kind == PRG_R_EVENT, "control: a MARK at depth 63 is EVENT");
    (void)prg_begin_r(&D, 1, &t[PRG_DEPTH - 1], &r);
    CHECK(prg_mark_r(&D, 2, &r) == PRG_EOVERFLOW && rk(&r, PRG_R_REFUSED, PRG_RR_MARK_DEPTH), "depth 64: REFUSED");
}
static void c_publish_fail_receipt(void)
{
    prg_receipt r;
    setup(16);
    atomic_store(&g_snap_ops, 0);
    atomic_store(&g_snap_unlock_fail_at, 2); /* the publication's unlock */
    CHECK(prg_mark_r(&D, 1, &r) == PRG_EFAULT && rk(&r, PRG_R_EVENT, 0) && r.seq == 0, "EVENT kept, rc EFAULT");
    CHECK(__atomic_load_n(&D.fault, __ATOMIC_ACQUIRE) == 1 && D.fault_reason == PRG_FAULT_GENERIC, "fault latched");
}

/* the public wrappers keep their return codes: a LOST submission is rc OK (R6) */
static void c_wrapper_rc(void)
{
    prg_token t;
    setup(16);
    g_reserve_fail = 1;
    CHECK(prg_mark(&D, 1) == PRG_OK, "a LOST mark: rc OK");
    CHECK(prg_begin(&D, 1, &t) == PRG_OK, "a LOST begin: rc OK");
    CHECK(prg_end(&D, &t, 1) == PRG_OK, "a LOST end: rc OK");
    g_reserve_fail = 0;
    CHECK(D.p_seq == 3 && D.st.lost_fifo == 3, "three numbered losses");
}

/* ---------------------------------------------------------------- prg_frame_capture */
static void c_fc_admission_accounting(void)
{
    prg_fc_out o;
    setup(16);
    /* pre-validation refusals: 0/0/0, inflight untouched */
    __atomic_store_n(&D.inflight, 7u, __ATOMIC_SEQ_CST);
    prg_frame_capture(&D, 0, 5, 0, 1, 0, &o);
    CHECK(o.rc == PRG_EINVAL && TRIES == 0 && ADMITS == 0 && LEAVES == 0 && inflight() == 7, "owner 0 frame 5");
    prg_frame_capture(&D, 1, 5, 1, 99, 0, &o);
    CHECK(o.rc == PRG_EINVAL && TRIES == 0 && inflight() == 7, "desc1 out of range");
    __atomic_store_n(&D.inflight, 0u, __ATOMIC_SEQ_CST);
    /* each successful path: 1/1/1 */
    const struct {
        uint32_t e, b;
        int unlock_fail;
        const char *what;
    } P[] = {{1, 0, 0, "END-only"}, {0, 2, 0, "BEGIN-only"}, {1, 2, 0, "combined"}, {1, 2, 2, "END publish fault"},
             {1, 2, 4, "BEGIN publish fault"}};
    for (unsigned i = 0; i < sizeof P / sizeof *P; i++) {
        setup(16);
        atomic_store(&g_snap_ops, 0);
        atomic_store(&g_snap_unlock_fail_at, P[i].unlock_fail);
        prg_frame_capture(&D, 1, 1, 1, P[i].e, P[i].b, &o);
        CHECK(TRIES == 1 && ADMITS == 1 && LEAVES == 1 && inflight() == 0, "%s: %d/%d/%d", P[i].what, TRIES, ADMITS,
              LEAVES);
        atomic_store(&g_snap_unlock_fail_at, 0);
    }
    /* a rejected admission: 1/0/0, inflight 0 */
    setup(16);
    stop_by("anomaly");
    prg_frame_capture(&D, 1, 1, 1, 1, 2, &o);
    CHECK(o.rc == PRG_ESTOPPED && TRIES == 1 && ADMITS == 0 && LEAVES == 0 && inflight() == 0, "rejected: %d/%d/%d",
          TRIES, ADMITS, LEAVES);
}
static void c_fc_refused(const char *how)
{
    prg_fc_out o;
    setup(16);
    CHECK(prg_frame(&D, 3, 4, 2) == PRG_OK, "frame 3/4");
    stop_by(how);
    uint64_t s0 = D.p_seq, t0 = D.p_ticket;
    prg_frame_capture(&D, 5, 6, 7, 1, 2, &o);
    CHECK(o.rc == PRG_ESTOPPED && o.committed == 0, "ESTOPPED, not committed");
    CHECK(D.p_seq == s0 && D.p_ticket == t0, "nothing submitted");
    CHECK(D.p_frame_owner == 3 && D.p_frame_id == 4 && D.p_owner_ref == 2, "nothing committed");
    CHECK(rk(&o.end, PRG_R_REFUSED, PRG_RR_NOT_ADMITTED) && rk(&o.begin, PRG_R_REFUSED, PRG_RR_NOT_ADMITTED), "receipts");
}
static void c_fc_order(void)
{
    prg_fc_out o;
    setup(16);
    CHECK(prg_frame(&D, 3, 4, 2) == PRG_OK, "old frame 3/4 ref 2");
    prg_frame_capture(&D, 5, 6, 7, 1, 2, &o);
    CHECK(o.rc == PRG_OK && o.committed == 1 && rk(&o.end, PRG_R_EVENT, 0) && rk(&o.begin, PRG_R_EVENT, 0) &&
              o.end.seq == 0 && o.begin.seq == 1,
          "both EVENT");
    CHECK(META[0].desc == 0 && META[0].frame_owner == 3 && META[0].frame_id == 4 && META[0].owner_ref == 2 &&
              META[0].kind == PRG_K_MARK,
          "END under the OLD fields (%u/%llu ref %u)", META[0].frame_owner, (unsigned long long)META[0].frame_id,
          META[0].owner_ref);
    CHECK(META[1].desc == 1 && META[1].frame_owner == 5 && META[1].frame_id == 6 && META[1].owner_ref == 7,
          "BEGIN under the NEW fields");
    CHECK(D.snap.seq == 2, "both published");
}
static void c_fc_end_publish_fail(void)
{
    prg_fc_out o;
    setup(16);
    CHECK(prg_frame(&D, 3, 4, 2) == PRG_OK, "old frame");
    cnt_reset();
    atomic_store(&g_snap_ops, 0);
    atomic_store(&g_snap_unlock_fail_at, 2);
    prg_frame_capture(&D, 5, 6, 7, 1, 2, &o);
    CHECK(o.rc == PRG_EFAULT && o.committed == 0 && rk(&o.end, PRG_R_EVENT, 0) && o.begin.kind == PRG_R_NONE,
          "EFAULT, committed 0, END EVENT, no BEGIN");
    CHECK(D.p_frame_owner == 3 && D.p_frame_id == 4 && D.p_owner_ref == 2, "no commit");
    CHECK(LEAVES == 1 && inflight() == 0, "one leave");
}
static void c_fc_begin_publish_fail(void)
{
    prg_fc_out o;
    setup(16);
    atomic_store(&g_snap_ops, 0);
    atomic_store(&g_snap_unlock_fail_at, 4); /* END's publication succeeds, BEGIN's unlock fails */
    prg_frame_capture(&D, 5, 6, 7, 1, 2, &o);
    CHECK(o.rc == PRG_EFAULT && o.committed == 1 && rk(&o.begin, PRG_R_EVENT, 0), "EFAULT, committed, BEGIN EVENT");
    CHECK(D.p_frame_owner == 5 && D.p_owner_ref == 7 && LEAVES == 1, "committed, one leave");
}
static void c_fc_einval_none_frame(void)
{
    prg_fc_out o;
    setup(16);
    prg_frame_capture(&D, 0, 1, 0, 0, 0, &o);
    CHECK(o.rc == PRG_EINVAL && TRIES == 0 && D.p_frame_id == 0, "EINVAL before admit, no transition");
    prg_frame_capture(&D, 0, 0, 0, 0, 0, &o);
    CHECK(o.rc == PRG_OK && o.committed == 1, "control: NONE {0, 0} commits");
}
static void c_fc_desc1_range(void)
{
    prg_fc_out o;
    const uint32_t v[] = {0, 1, 8, 9, UINT32_MAX};
    const int ok[] = {1, 1, 1, 0, 0};
    for (int i = 0; i < 5; i++) {
        setup(16);
        prg_frame_capture(&D, 1, 1, 1, v[i], 0, &o);
        CHECK((o.rc == PRG_OK) == ok[i] && (ok[i] || TRIES == 0), "end_desc1 %u: rc %d", v[i], o.rc);
        setup(16);
        prg_frame_capture(&D, 1, 1, 1, 0, v[i], &o);
        CHECK((o.rc == PRG_OK) == ok[i] && (ok[i] || TRIES == 0), "begin_desc1 %u: rc %d", v[i], o.rc);
    }
    setup(16);
    D.desc_count = NULL;
    prg_frame_capture(&D, 1, 1, 1, 1, 0, &o);
    CHECK(o.rc == PRG_EINVAL, "no published count: every nonzero desc1 refused");
}

/* ---------------------------------------------------------------- prg_zone_abandon */
static void c_abandon(const char *v)
{
    prg_token a, b, s;
    prg_receipt r;
    uint64_t s0;
    setup(1024);
    if (!strcmp(v, "top")) {
        (void)prg_begin_r(&D, 1, &a, &r);
        (void)prg_begin_r(&D, 2, &b, &r);
        s0 = D.p_seq;
        cnt_reset();
        CHECK(prg_zone_abandon(&D, &b, 2) == PRG_OK && D.p_depth == 1 && D.p_seq == s0 && D.snap.seq == s0,
              "the top pops, nothing submitted or published");
        CHECK(TRIES == 1 && ADMITS == 1 && LEAVES == 1, "one admission");
        CHECK(prg_end_r(&D, &a, 1, &r) == PRG_OK && r.kind == PRG_R_EVENT, "the zone below ends normally");
    } else if (!strcmp(v, "suppressed_top")) {
        prg_token t[PRG_DEPTH];
        for (uint32_t i = 0; i < PRG_DEPTH; i++)
            (void)prg_begin_r(&D, 1, &t[i], &r);
        (void)prg_begin_r(&D, 3, &s, &r);
        CHECK(r.kind == PRG_R_SUPPRESSED, "suppressed");
        s0 = D.p_seq;
        CHECK(prg_zone_abandon(&D, &s, 3) == PRG_OK && D.p_suppressed == 0 && D.p_depth == PRG_DEPTH && D.p_seq == s0,
              "the innermost suppressed level pops");
        CHECK(prg_zone_abandon(&D, &s, 3) == PRG_EINVAL, "its serial never matches again");
    } else if (!strcmp(v, "lost_begin")) {
        g_reserve_fail = 1;
        (void)prg_begin_r(&D, 1, &a, &r);
        g_reserve_fail = 0;
        CHECK(r.kind == PRG_R_LOST && D.p_depth == 1, "a LOST begin is on the stack");
        CHECK(prg_zone_abandon(&D, &a, 1) == PRG_OK && D.p_depth == 0, "and abandons");
    } else if (!strcmp(v, "wrong_desc")) {
        (void)prg_begin_r(&D, 1, &a, &r);
        CHECK(prg_zone_abandon(&D, &a, 2) == PRG_EINVAL && D.p_depth == 1, "another descriptor: EINVAL, unchanged");
    } else if (!strcmp(v, "not_top")) {
        (void)prg_begin_r(&D, 1, &a, &r);
        (void)prg_begin_r(&D, 2, &b, &r);
        CHECK(prg_zone_abandon(&D, &a, 1) == PRG_EINVAL && D.p_depth == 2, "not the top: EINVAL, unchanged");
    } else if (!strcmp(v, "stale_token")) {
        (void)prg_begin_r(&D, 1, &a, &r);
        (void)prg_end_r(&D, &a, 1, &r);
        (void)prg_begin_r(&D, 1, &b, &r);
        CHECK(prg_zone_abandon(&D, &a, 1) == PRG_EINVAL && D.p_depth == 1, "a stale token at the same depth: EINVAL");
    } else if (!strcmp(v, "empty_stack")) {
        a = (prg_token){1026, 0, 0, 1, 0};
        CHECK(prg_zone_abandon(&D, &a, 1) == PRG_EINVAL && D.p_depth == 0, "empty stack: EINVAL");
    } else if (!strcmp(v, "refused_admission")) {
        (void)prg_begin_r(&D, 1, &a, &r);
        prg_seal(&D);
        cnt_reset();
        CHECK(prg_zone_abandon(&D, &a, 1) == PRG_EINVAL && D.p_depth == 1, "sealed: EINVAL, stack unchanged");
        CHECK(TRIES == 1 && ADMITS == 0 && LEAVES == 0, "1/0/0 (%d/%d/%d)", TRIES, ADMITS, LEAVES);
    }
}

/* ---------------------------------------------------------------- the fault reason (R81) */
static void inline_hook_fault(int point)
{
    (void)point;
    prg_hook_fault(&D);
}
static void c_fault_reason(int generic_first)
{
    prg_receipt r;
    setup(16);
    if (generic_first) {
        atomic_store(&g_snap_ops, 0);
        atomic_store(&g_snap_lock_fail_at, 1);
        CHECK(prg_mark_r(&D, 1, &r) == PRG_EFAULT, "the snap lock fault");
        atomic_store(&g_snap_lock_fail_at, 0);
        prg_hook_fault(&D);
        CHECK(D.fault == 1 && D.fault_reason == PRG_FAULT_GENERIC, "generic stays first (%u)", D.fault_reason);
    } else {
        /* the hook fault lands inside an admitted call whose publication then faults (generic, second) */
        st_inline = inline_hook_fault;
        step_arm(PRG_SP_ADMITTED, 1);
        atomic_store(&g_snap_ops, 0);
        atomic_store(&g_snap_lock_fail_at, 1);
        CHECK(prg_mark_r(&D, 1, &r) == PRG_EFAULT && r.kind == PRG_R_EVENT, "the publication faults after the latch");
        st_inline = NULL;
        CHECK(D.fault == 1 && D.fault_reason == PRG_FAULT_HOOK, "hook first (%u)", D.fault_reason);
    }
}

/* concurrent: the drainer's injected snap fault against the producer's hook fault; exactly one CAS wins */
static atomic_int g_go;
static void *drainer(void *a)
{
    (void)a;
    SPIN_UNTIL(atomic_load(&g_go), "drainer start");
    prg_drain(&D);
    return NULL;
}
static void c_fault_reason_concurrent(void)
{
    for (int rep = 0; rep < 202; rep++) {
        pthread_t th;
        prg_receipt r;
        setup(16);
        (void)prg_mark_r(&D, 1, &r);
        atomic_store(&g_snap_ops, 0);
        atomic_store(&g_snap_lock_fail_at, 1); /* the drainer's snapshot copy faults */
        atomic_store(&g_go, 0);
        pthread_create(&th, NULL, drainer, NULL);
        if (rep == 0) { /* forced: the drainer first */
            atomic_store(&g_go, 1);
            pthread_join(th, NULL);
            prg_hook_fault(&D);
            CHECK(D.fault_reason == PRG_FAULT_GENERIC, "drainer first: generic");
            continue;
        }
        if (rep == 1) { /* forced: the hook first */
            prg_hook_fault(&D);
            atomic_store(&g_go, 1);
            pthread_join(th, NULL);
            CHECK(D.fault_reason == PRG_FAULT_HOOK, "hook first: hook");
            continue;
        }
        atomic_store(&g_go, 1);
        prg_hook_fault(&D);
        pthread_join(th, NULL);
        uint32_t fr = __atomic_load_n(&D.fault_reason, __ATOMIC_ACQUIRE);
        CHECK(fr == PRG_FAULT_GENERIC || fr == PRG_FAULT_HOOK, "one winner (%u)", fr);
        CHECK(D.fault == 1, "fault latched");
        atomic_store(&g_snap_lock_fail_at, 0);
    }
}

/* ---------------------------------------------------------------- the seal at P1-P6 (R65, R85) */
static atomic_int g_sealed, g_rel_rc;
static void *sealer(void *a)
{
    (void)a;
    step_wait_paused();
    prg_seal(&D);
    atomic_store(&g_rel_rc, prg_release(&D)); /* while the producer is inside its admission (P2-P6) */
    atomic_store(&g_sealed, 1);
    step_go();
    return NULL;
}
static void c_seal(int pt, int tsan_only)
{
    prg_fc_out o;
    pthread_t th;
    int end_in = 0, begin_in = 0, end_lost = 0, begin_lost = 0;
    (void)tsan_only;
    setup(16);
    CHECK(prg_frame(&D, 3, 4, 2) == PRG_OK, "old frame");
    CHECK(prg_mark_r(&D, 3, &(prg_receipt){0}) == PRG_OK, "an earlier event");
    cnt_reset();
    atomic_store(&g_sealed, 0);
    step_arm(PRG_SP_P1 + pt - 1, 1);
    pthread_create(&th, NULL, sealer, NULL);
    prg_frame_capture(&D, 5, 6, 7, 1, 2, &o);
    pthread_join(th, NULL);
    CHECK(atomic_load(&g_sealed) == 1, "sealed at P%d", pt);
    CHECK(inflight() == 0 && (pt == 1 ? LEAVES == 0 : LEAVES == 1), "leave accounting (%d)", LEAVES);
    if (pt == 1) {
        CHECK(o.rc == PRG_ESTOPPED && o.committed == 0 && D.p_frame_owner == 3, "P1: ESTOPPED, nothing committed");
    } else {
        CHECK(o.rc == PRG_OK && o.committed == 1, "P%d: the admitted call finishes (rc %d)", pt, o.rc);
        CHECK(atomic_load(&g_rel_rc) == PRG_EBUSY, "P%d: release EBUSY while inflight (%d)", pt, atomic_load(&g_rel_rc));
        CHECK(prg_release(&D) == PRG_OK, "release OK after the leave");
    }
    if (pt == 1)
        CHECK(atomic_load(&g_rel_rc) == PRG_OK, "P1: nothing in flight, release OK");
    for (int i = 0; i < g_nev; i++) {
        end_in += g_ev[i].seq == 1 && g_ev[i].desc == 0;
        begin_in += g_ev[i].seq == 2 && g_ev[i].desc == 1;
    }
    for (int i = 0; i < g_nloss; i++) {
        end_lost += g_loss_first[i] <= 1 && 1 < g_loss_first[i] + g_loss_count[i];
        begin_lost += g_loss_first[i] <= 2 && 2 < g_loss_first[i] + g_loss_count[i];
    }
    /* the frozen frontier table (R65) and the exact prefix (R85) */
    const int want_end = pt >= 4, want_begin = pt == 6;
    CHECK((end_in || end_lost) == want_end && !(end_in && end_lost), "P%d: END in the frontier %d (event %d loss %d)", pt,
          want_end, end_in, end_lost);
    CHECK((begin_in || begin_lost) == want_begin, "P%d: BEGIN in the frontier %d (event %d loss %d)", pt, want_begin,
          begin_in, begin_lost);
    CHECK(g_nev >= 1 && g_ev[0].seq == 0, "the earlier event is in the file");
    printf("events");
    for (int i = 0; i < g_nev; i++)
        printf(" %llu:%u", (unsigned long long)g_ev[i].seq, g_ev[i].desc);
    for (int i = 0; i < g_nloss; i++)
        printf(" loss%llu+%llu", (unsigned long long)g_loss_first[i], (unsigned long long)g_loss_count[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const char *c = g_case_name = argv[1];
    if (!strncmp(c, "receipt_refused_", 16) && (!strcmp(c + 16, "sealed") || !strcmp(c + 16, "detaching") ||
                                                !strcmp(c + 16, "anomaly") || !strcmp(c + 16, "overflow") ||
                                                !strcmp(c + 16, "exhausted") || !strcmp(c + 16, "faulted")))
        c_receipt_refused(c + 16);
    else if (!strcmp(c, "receipt_refused_einval"))
        c_receipt_einval();
    else if (!strcmp(c, "receipt_refused_seq_overflow"))
        c_receipt_seq_overflow();
    else if (!strcmp(c, "receipt_refused_suppress_overflow"))
        c_receipt_suppress_overflow();
    else if (!strcmp(c, "receipt_refused_mark_depth"))
        c_receipt_mark_depth();
    else if (!strcmp(c, "publish_fail_receipt"))
        c_publish_fail_receipt();
    else if (!strcmp(c, "wrapper_rc"))
        c_wrapper_rc();
    else if (!strcmp(c, "fc_admission_accounting"))
        c_fc_admission_accounting();
    else if (!strncmp(c, "fc_refused_", 11))
        c_fc_refused(c + 11);
    else if (!strcmp(c, "fc_order"))
        c_fc_order();
    else if (!strcmp(c, "fc_end_publish_fail"))
        c_fc_end_publish_fail();
    else if (!strcmp(c, "fc_begin_publish_fail"))
        c_fc_begin_publish_fail();
    else if (!strcmp(c, "fc_einval_none_frame"))
        c_fc_einval_none_frame();
    else if (!strcmp(c, "fc_desc1_range"))
        c_fc_desc1_range();
    else if (!strncmp(c, "abandon_", 8))
        c_abandon(c + 8);
    else if (!strcmp(c, "fault_reason_generic_first"))
        c_fault_reason(1);
    else if (!strcmp(c, "fault_reason_hook_first"))
        c_fault_reason(0);
    else if (!strcmp(c, "fault_reason_concurrent"))
        c_fault_reason_concurrent();
    else if (!strncmp(c, "seal_P", 6) && c[6] >= '1' && c[6] <= '6' && !c[7])
        c_seal(c[6] - '0', 0);
    else if (!strcmp(c, "release_while_inflight")) {
        for (int pt = 2; pt <= 6; pt++) {
            g_nev = g_nloss = 0;
            c_seal(pt, 0);
        }
    } else if (!strcmp(c, "harness_stall")) { /* a harness control: must be classified INVALID */
        g_spin_max = 1000000;
        step_arm(PRG_SP_P1, 1);
        setup(16);
        prg_fc_out o;
        prg_frame_capture(&D, 1, 1, 1, 0, 0, &o); /* pauses with nobody to release it */
    } else if (!strcmp(c, "harness_crash")) {
        volatile int *z = NULL;
        *z = 1;
    } else {
        printf("CORE %s FAIL unknown case\n", c);
        return 2;
    }
    if (g_bad)
        return 1;
    printf("CORE %s PASS\n", c);
    return 0;
}
