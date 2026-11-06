/* overhead-bench production-path lifecycle and publication controls: the REAL driver (common/ob_drv.c), SPU
 * lifecycle (ob_spu_life.c), GPU section (ob_gpu_run.c), checked output (ob_io.c) and CSV writer, bound to mocks of
 * the profiler, pthreads (real threads; create/join/init can be failed), files (in memory), LV2/SPU and GCM.
 *
 * A baseline run (no fault) must succeed: every output under its final name, the exact receipt, every resource
 * released, the profiler shut down once.  Then, for every mocked operation the baseline calls and for each of its
 * calls (first, second, middle, last; all when there are at most 6), that one call fails; plus combined faults on
 * the failure-only paths (terminate, join after a lost reply, image close after a failed join).  Every faulted run
 * must: return rc != 0; publish no receipt; leave no output under its final name; keep the state rules (no join
 * after a failed terminate; no lane_end, group destroy or image close while the SPU group is not joined; no GPU
 * release after a failed fence or detach; no profiler shutdown while a worker, producer, lane or domain is
 * retained; every created worker joined).  usage: test_ob_life  (prints one line per failed check and the totals) */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/ob_cases.h"
#include "../common/ob_drv.h"

enum {
    F_OPEN, F_WRITE, F_FSYNC, F_CLOSE, F_RENAME,
    P_INIT, P_TBEGIN, P_DRAIN, P_SHUTDOWN,
    T_MUTEX, T_COND, T_CREATE, T_JOIN,
    S_INIT, S_IMPORT, S_REG, S_LANE, S_GROUP, S_THREAD, S_START, S_MB, S_NOREPLY, S_ECHO, S_ACK, S_FINI, S_TERM,
    S_JOIN, S_LANE_END, S_DESTROY, S_IMGCLOSE,
    G_INIT, G_LABEL, G_ATTACH, G_CAL, G_DETACH, G_RELEASE, G_ZONE,
    S_START_NS, S_EXECRC, S_LEDGER,
    NOPS
};
static const char *const opn[NOPS] = {
    "open", "write", "fsync", "close", "rename", "prof_init", "thread_begin", "drain", "shutdown", "mutex_init",
    "cond_init", "thread_create", "thread_join", "spu_init", "image_import", "image_register", "lane_create",
    "group_create", "thread_init", "group_start", "write_mb", "reply_lost", "reply_identity", "startup_ack",
    "fini_rc", "terminate", "group_join", "lane_end", "group_destroy", "image_close", "gcm_init", "label_fence",
    "gpu_attach", "calibrate", "gpu_detach", "gpu_release", "gpu_zone", "start_proven_not_started",
    "block_exec_rc", "block_ledger"};

static unsigned long calls[NOPS], fail_at[NOPS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int hit(int op)
{
    pthread_mutex_lock(&g_mu);
    unsigned long k = ++calls[op];
    int f = fail_at[op] && fail_at[op] == k;
    pthread_mutex_unlock(&g_mu);
    return f;
}

/* ---- in-memory files ---- */
#define NF 16
typedef struct mfile {
    char name[200];
    char data[1 << 18];
    unsigned long len;
    int used, open;
} mfile;
static mfile FS[NF];
static mfile *fs_find(const char *n)
{
    for (int i = 0; i < NF; ++i)
        if (FS[i].used && !strcmp(FS[i].name, n))
            return &FS[i];
    return NULL;
}
static int m_open(void *c, const char *p)
{
    (void)c;
    if (hit(F_OPEN))
        return -1;
    mfile *f = fs_find(p);
    for (int i = 0; !f && i < NF; ++i)
        if (!FS[i].used)
            f = &FS[i];
    if (!f)
        return -1;
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", p);
    f->used = f->open = 1;
    return (int)(f - FS);
}
static long m_write(void *c, int fd, const void *b, unsigned long n)
{
    (void)c;
    if (hit(F_WRITE))
        return -1;
    mfile *f = &FS[fd];
    if (!f->open || f->len + n > sizeof f->data)
        return -1;
    unsigned long k = n > 7 ? n / 2 : n; /* short writes: the writer must continue */
    memcpy(f->data + f->len, b, k);
    f->len += k;
    return (long)k;
}
static int m_fsync(void *c, int fd)
{
    (void)c;
    (void)fd;
    return hit(F_FSYNC) ? -1 : 0;
}
static int m_close(void *c, int fd)
{
    (void)c;
    FS[fd].open = 0; /* closed even when it reports failure */
    return hit(F_CLOSE) ? -1 : 0;
}
static int m_rename(void *c, const char *a, const char *b)
{
    (void)c;
    if (hit(F_RENAME))
        return -1;
    mfile *f = fs_find(a), *g = fs_find(b);
    if (!f)
        return -1;
    if (g)
        g->used = 0;
    snprintf(f->name, sizeof f->name, "%s", b);
    return 0;
}
static const ob_fio IO = {NULL, m_open, m_write, m_fsync, m_close, m_rename};

/* ---- profiler, threads, PPU blocks ---- */
static unsigned long g_tb = 1000;
static int prof_up, shut_downs, registered;
static uint64_t m_tb(void *c)
{
    (void)c;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g_tb += 7;
    pthread_mutex_unlock(&g_mu);
    return v;
}
static int m_init(void *c)
{
    (void)c;
    if (hit(P_INIT))
        return -1;
    prof_up = 1;
    return 0;
}
static int m_tbegin(void *c, const char *n)
{
    (void)c;
    (void)n;
    if (hit(P_TBEGIN))
        return -2;
    __atomic_add_fetch(&registered, 1, __ATOMIC_SEQ_CST);
    return 0;
}
static void m_tend(void *c)
{
    (void)c;
    __atomic_sub_fetch(&registered, 1, __ATOMIC_SEQ_CST);
}
static int m_drain(void *c)
{
    (void)c;
    return hit(P_DRAIN) ? -3 : 0;
}
static int m_shutdown(void *c)
{
    (void)c;
    ++shut_downs;
    if (hit(P_SHUTDOWN))
        return -4;
    prof_up = 0;
    return 0;
}
static int m_mutex(void *c, void *m)
{
    (void)c;
    return hit(T_MUTEX) ? -1 : pthread_mutex_init((pthread_mutex_t *)m, NULL);
}
static int m_cond(void *c, void *v)
{
    (void)c;
    return hit(T_COND) ? -1 : pthread_cond_init((pthread_cond_t *)v, NULL);
}
static pthread_t TH[8];
static int th_n, th_joined;
static int m_create(void *c, void **h, void *(*fn)(void *), void *arg)
{
    (void)c;
    if (hit(T_CREATE) || th_n >= 8)
        return -1;
    if (pthread_create(&TH[th_n], NULL, fn, arg) != 0)
        return -1;
    *h = &TH[th_n++];
    return 0;
}
static int m_join(void *c, void *h)
{
    (void)c;
    pthread_join(*(pthread_t *)h, NULL); /* the real thread is always joined (no leak); the result can be failed */
    ++th_joined;
    return hit(T_JOIN) ? -1 : 0;
}
static void m_run(void *c, const ob_block *b, uint64_t *t0, uint64_t *t1)
{
    (void)b;
    *t0 = m_tb(c);
    *t1 = m_tb(c);
}
static char g_log[4096];
static void m_log(void *c, const char *w, int rc)
{
    (void)c;
    (void)rc;
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof g_log - n, "%s|", w);
}

/* ---- the SPU (the program's reply protocol, synchronous) ---- */
static ob_spu_res RES;
static int spu_started, spu_exited, spu_terminated, spu_joined, spu_running;
static int viol_join_unproven, viol_after_unjoined, n_term, n_join, n_lane_end, n_destroy, n_imgclose, n_cmds;
static void reply(const uint32_t w[8], uint32_t echo)
{
    int lost = hit(S_NOREPLY), bad = hit(S_ECHO);
    memcpy(RES.w, w, 8 * sizeof w[0]);
    RES.w[8] = bad ? echo ^ 0x10000u : echo;
    RES.w[9] = 1;
    if (!lost)
        ++RES.seq;
}
static int s_init(void *c) { (void)c; return hit(S_INIT) ? -1 : 0; }
static int s_import(void *c) { (void)c; return hit(S_IMPORT) ? -1 : 0; }
static int s_reg(void *c, uint32_t *i) { (void)c; *i = 1; return hit(S_REG) ? -1 : 0; }
static int s_lane(void *c, uint32_t im, ob_lane *l)
{
    (void)c;
    (void)im;
    l->handle = 7, l->ring_slots = 1024, l->ea_ring = 0x1000, l->ea_ctl = 0x2000;
    return hit(S_LANE) ? -1 : 0;
}
static int s_group(void *c) { (void)c; return hit(S_GROUP) ? -1 : 0; }
static int s_thread(void *c, const ob_lane *l, uint64_t ea)
{
    (void)c;
    (void)l;
    (void)ea;
    memset(&RES, 0, sizeof RES);
    return hit(S_THREAD) ? -1 : 0;
}
/* S_START_NS: a documented, proven non-start (nothing runs).  S_START: the producer is created and runs (it even
 * posts its startup reply), THEN the start call reports failure: an unclassified start error. */
static int s_start(void *c)
{
    (void)c;
    if (hit(S_START_NS))
        return OB_SPU_START_NOT_STARTED;
    spu_started = spu_running = 1;
    uint32_t w[8] = {0};
    if (hit(S_ACK))
        w[0] = (uint32_t)-5;
    reply(w, 0);
    if (hit(S_START))
        return -1;
    return 0;
}
static int s_mb(void *c, uint32_t word)
{
    (void)c;
    if (hit(S_MB))
        return -1;
    uint32_t w[8] = {0};
    if (word == OB_SPU_EXIT) {
        w[0] = hit(S_FINI) ? 9u : 0u;
        spu_exited = 1;
        spu_running = 0;
        reply(w, 0);
        return 0;
    }
    uint32_t cs = (word >> 8) & 0xFFu, tu = word & 0xFFu;
    ++n_cmds;
    w[0] = 5000, w[1] = 4000;
    if (tu == 0) {
        ob_ledger l = ob_planned((int)cs);
        w[3] = (uint32_t)l.sseq, w[4] = (uint32_t)l.tail_lost, w[5] = (uint32_t)l.staged;
        w[6] = (uint32_t)l.published, w[7] = (uint32_t)l.lost;
    }
    if (hit(S_EXECRC))
        w[2] = 7u; /* a well-identified reply whose block failed */
    if (hit(S_LEDGER))
        w[3] += 1u; /* a well-identified rc-0 reply with a wrong ledger */
    reply(w, word);
    return 0;
}
static uint32_t s_seq(void *c) { (void)c; return RES.seq; }
static void s_words(void *c, uint32_t o[16]) { (void)c; memcpy(o, RES.w, sizeof RES.w); }
static void s_pause(void *c) { (void)c; }
static int s_term(void *c)
{
    (void)c;
    ++n_term;
    if (hit(S_TERM))
        return -1;
    spu_terminated = 1;
    spu_running = 0;
    return 0;
}
static int s_join(void *c)
{
    (void)c;
    ++n_join;
    if (!spu_exited && !spu_terminated)
        viol_join_unproven = 1; /* a real join here could hang: the driver must not reach it */
    if (hit(S_JOIN))
        return -1;
    spu_joined = 1;
    return 0;
}
static int s_lane_end(void *c, uint32_t h)
{
    (void)c;
    (void)h;
    ++n_lane_end;
    if (spu_started && !spu_joined)
        viol_after_unjoined = 1;
    return hit(S_LANE_END) ? -1 : 0;
}
static int s_destroy(void *c)
{
    (void)c;
    ++n_destroy;
    if (spu_started && !spu_joined)
        viol_after_unjoined = 1;
    return hit(S_DESTROY) ? -1 : 0;
}
static int s_imgclose(void *c)
{
    (void)c;
    ++n_imgclose;
    if (spu_started && !spu_joined)
        viol_after_unjoined = 1;
    return hit(S_IMGCLOSE) ? -1 : 0;
}
static const ob_spu_port SPU = {NULL,   s_init,  s_import, s_reg,  s_lane,  s_group,   s_thread, s_start,
                                s_mb,   s_seq,   s_words,  s_pause, s_term, s_join,    s_lane_end,
                                s_destroy, s_imgclose, m_tb, m_log};

/* ---- the GPU ---- */
static uint32_t lab_pending, lab_cur, cmdbuf[1 << 16], *cur = cmdbuf;
static int gpu_attached, gpu_detached, viol_release, n_release;
static void adv(unsigned w)
{
    cur += w;
    if (cur >= cmdbuf + (1 << 16) - 64)
        cur = cmdbuf;
}
static void r_report(void *c, uint32_t i) { (void)c; (void)i; adv(2); }
static void r_label(void *c, uint32_t v) { (void)c; lab_pending = v; adv(4); }
static void r_draw(void *c, uint32_t i) { (void)c; (void)i; adv(10); }
static int r_zone(void *c, uint32_t i)
{
    (void)c;
    (void)i;
    adv(6);
    return hit(G_ZONE) ? -1 : 0;
}
static void r_nop(void *c, uint32_t w) { (void)c; adv(w); }
static void r_flush(void *c)
{
    (void)c;
    if (!hit(G_LABEL))
        lab_cur = lab_pending; /* the GPU executed up to the label */
}
static const ob_rsx_port RSX = {NULL, r_report, r_label, r_draw, r_zone, r_zone, r_nop, r_flush};
static int g_init(void *c) { (void)c; return hit(G_INIT) ? -1 : 0; }
static void g_store(void *c, uint32_t v) { (void)c; lab_cur = lab_pending = v; }
static uint32_t g_read(void *c) { (void)c; return lab_cur; }
static int g_attach(void *c, uint32_t f, uint32_t n, uint32_t l, uint32_t *g)
{
    (void)c;
    (void)f;
    (void)n;
    (void)l;
    *g = 3;
    if (hit(G_ATTACH))
        return -1;
    gpu_attached = 1;
    return 0;
}
static int g_cal(void *c, uint32_t g) { (void)c; (void)g; return hit(G_CAL) ? -1 : 0; }
static int g_detach(void *c, uint32_t g)
{
    (void)c;
    (void)g;
    if (hit(G_DETACH))
        return -1;
    gpu_detached = 1;
    return 0;
}
static int g_release(void *c, uint32_t g)
{
    (void)c;
    (void)g;
    ++n_release;
    if (!gpu_detached)
        viol_release = 1;
    if (hit(G_RELEASE))
        return -1;
    gpu_attached = 0;
    return 0;
}
static uint32_t *g_cur(void *c) { (void)c; return cur; }
static uint64_t g_rt(void *c, uint32_t i) { (void)c; return 100u + i; }
static const ob_gpu_port GPU = {NULL,    g_init,  g_store, g_read, s_pause, g_attach, g_cal,  g_detach,
                                g_release, m_drain, m_tb,   g_cur,  g_rt,    m_log,    &RSX};

static const ob_drv_port PORT = {NULL,     &IO,      "/t/ob",   77,       79800000, 1,      m_tb,
                                 m_init,   m_tbegin, m_tend,    m_drain,  m_shutdown, m_mutex, m_cond,
                                 m_create, m_join,   m_run,     &SPU,     0x3000,   3,      &GPU,
                                 OB_GPU_REPORT_COUNT, 3, m_log};

static unsigned checks, failures;
static void check(int ok, const char *what, const char *ctx)
{
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL %s [%s] log: %.300s\n", what, ctx, g_log);
    }
}

static void reset(void)
{
    memset(calls, 0, sizeof calls);
    memset(fail_at, 0, sizeof fail_at);
    memset(FS, 0, sizeof FS);
    memset(&RES, 0, sizeof RES);
    prof_up = shut_downs = registered = 0;
    th_n = th_joined = 0;
    spu_started = spu_exited = spu_terminated = spu_joined = spu_running = 0;
    viol_join_unproven = viol_after_unjoined = n_term = n_join = n_lane_end = n_destroy = n_imgclose = n_cmds = 0;
    gpu_attached = gpu_detached = viol_release = n_release = 0;
    lab_pending = lab_cur = 0;
    cur = cmdbuf;
    g_log[0] = 0;
}

static int any_final(void)
{
    return fs_find("/t/ob.csv") || fs_find("/t/ob.gpu.csv") || fs_find("/t/ob.bench-end.txt");
}

/* the invariants every run keeps */
static void invariants(const ob_drv_result *r, const char *ctx)
{
    check(!viol_join_unproven, "no group join without a proven exit or a successful terminate", ctx);
    check(!viol_after_unjoined, "no lane_end / group destroy / image close while a started group is not joined", ctx);
    check(!viol_release, "no GPU release without a successful detach", ctx);
    check(th_joined == th_n, "every created worker joined", ctx);
    int retained = r->workers_retained || r->spu_life == OB_LIFE_RETAINED || r->gpu_life == OB_LIFE_RETAINED;
    check(!(retained && shut_downs), "no profiler shutdown while anything is retained", ctx);
    check(shut_downs <= 1, "at most one shutdown", ctx);
    if (n_term && (fail_at[S_TERM] && calls[S_TERM] >= fail_at[S_TERM]))
        check(n_join == 0, "no join after a failed terminate", ctx);
    for (int i = 0; i < NF; ++i)
        check(!FS[i].used || !FS[i].open, "every opened file closed", ctx);
}

static void baseline(unsigned long base[NOPS])
{
    reset();
    ob_drv_result r;
    ob_drv_run(&PORT, &r);
    invariants(&r, "baseline");
    check(r.rc == 0 && r.receipt_published, "baseline: the run succeeds and publishes the receipt", "baseline");
    mfile *rc = fs_find("/t/ob.bench-end.txt"), *cs = fs_find("/t/ob.csv"), *gc = fs_find("/t/ob.gpu.csv");
    char want[160];
    snprintf(want, sizeof want, "BENCH_END rows=%lu invalid=0 ledger_mismatches=0 receipt=complete\n", r.planned);
    check(rc && rc->len == strlen(want) && !memcmp(rc->data, want, rc->len), "baseline: the exact receipt", "baseline");
    check(cs && gc && r.rows == r.planned && r.gpu_rows == 1u + 5u, "baseline: CSVs final, every planned row",
          "baseline");
    char endl[40];
    snprintf(endl, sizeof endl, "END,%lu\n", r.rows);
    check(cs && cs->len > strlen(endl) && !memcmp(cs->data + cs->len - strlen(endl), endl, strlen(endl)),
          "baseline: END carries the rows written", "baseline");
    check(!fs_find("/t/ob.csv.tmp") && !fs_find("/t/ob.csv.failed") && !fs_find("/t/ob.bench-end.tmp"),
          "baseline: no tmp or failed file left", "baseline");
    check(r.spu_life == OB_LIFE_CLEAN && r.gpu_life == OB_LIFE_CLEAN && !r.workers_retained && shut_downs == 1 &&
              registered == 0 && n_join == 1 && n_lane_end == 1 && n_destroy == 1 && n_imgclose == 1 && n_release == 1,
          "baseline: everything released, the profiler shut down once", "baseline");
    memcpy(base, calls, sizeof calls);
    FILE *f = fopen(getenv("OB_LIFE_CSV") ? getenv("OB_LIFE_CSV") : "/dev/null", "w");
    if (f && cs) {
        fwrite(cs->data, 1, cs->len, f);
        fclose(f);
    }
    f = fopen(getenv("OB_LIFE_GPU_CSV") ? getenv("OB_LIFE_GPU_CSV") : "/dev/null", "w");
    if (f && gc) {
        fwrite(gc->data, 1, gc->len, f);
        fclose(f);
    }
}

static unsigned long runs;
static void faulted(const int *ops, const unsigned long *ks, int n, const char *label)
{
    char ctx[160];
    reset();
    int k = snprintf(ctx, sizeof ctx, "%s", label);
    for (int i = 0; i < n; ++i) {
        fail_at[ops[i]] = ks[i];
        k += snprintf(ctx + k, sizeof ctx - (size_t)k, " %s#%lu", opn[ops[i]], ks[i]);
    }
    ob_drv_result r;
    ob_drv_run(&PORT, &r);
    ++runs;
    invariants(&r, ctx);
    check(r.rc != 0 && !r.receipt_published, "a faulted run fails without a receipt", ctx);
    check(!any_final(), "no output under its final name", ctx);
}

int main(void)
{
    unsigned long base[NOPS];
    baseline(base);
    printf("  baseline calls:");
    for (int op = 0; op < NOPS; ++op)
        if (base[op])
            printf(" %s=%lu", opn[op], base[op]);
    printf("\n");
    unsigned long per_op[NOPS];
    memset(per_op, 0, sizeof per_op);
    for (int op = 0; op < NOPS; ++op) {
        unsigned long n = base[op];
        if (op == S_TERM || n == 0)
            continue;
        unsigned long ks[6];
        int nk = 0;
        if (n <= 6) {
            for (unsigned long k = 1; k <= n; ++k)
                ks[nk++] = k;
        } else {
            ks[nk++] = 1, ks[nk++] = 2, ks[nk++] = n / 2, ks[nk++] = n - 1, ks[nk++] = n;
        }
        for (int i = 0; i < nk; ++i) {
            faulted(&op, &ks[i], 1, "single");
            ++per_op[op];
        }
    }
    /* failure-only paths, combined */
    {
        int o1[] = {S_NOREPLY, S_TERM};
        unsigned long k1[] = {3, 1};
        faulted(o1, k1, 2, "lost reply + terminate fails");
        check(n_join == 0 && n_lane_end == 0 && n_imgclose == 0 && shut_downs == 0,
              "terminate failed: uncertain, nothing joined, ended, closed or shut down", "combined 1");
        int o2[] = {S_NOREPLY, S_JOIN};
        unsigned long k2[] = {3, 1};
        faulted(o2, k2, 2, "lost reply + join fails");
        check(n_term == 1 && n_lane_end == 0 && n_imgclose == 0 && n_destroy == 0 && shut_downs == 0,
              "join failed: lane, group and image retained, no shutdown", "combined 2");
        int o3[] = {S_MB};
        unsigned long k3[] = {base[S_MB]};
        faulted(o3, k3, 1, "exit command send fails");
        check(n_term == 1 && n_join == 1 && shut_downs == 1, "exit not sent: terminate, then join, then shutdown",
              "combined 3");
        int o4[] = {S_NOREPLY};
        unsigned long k4[] = {1};
        faulted(o4, k4, 1, "startup acknowledgement lost");
        check(n_term == 1 && n_join == 1 && th_n == 0, "no startup ack: no worker created; terminate and join",
              "combined 4");
        int o5[] = {S_JOIN, S_IMGCLOSE};
        unsigned long k5[] = {1, 1};
        faulted(o5, k5, 2, "join fails with image close armed");
        check(n_imgclose == 0, "after a failed join the image is not closed", "combined 5");
        int o6[] = {T_CREATE};
        unsigned long k6[] = {4};
        faulted(o6, k6, 1, "partial startup");
        check(th_n == 3 && th_joined == 3 && shut_downs == 1, "partial startup: the 3 created workers joined",
              "combined 6");
        int o7[] = {G_LABEL};
        unsigned long k7[] = {3};
        faulted(o7, k7, 1, "early GPU stop");
        mfile *g = fs_find("/t/ob.gpu.csv.failed");
        check(g && strstr(g->data, "END,") && !strstr(g->data, "END,6") && strstr(g->data, ",1\nEND,"),
              "early GPU stop: END carries the rows actually written, kept as .failed", "combined 7");
        check(shut_downs == 1, "a later fresh fence (close) succeeded: the domain is released, then the shutdown",
              "combined 7");
        int o8[] = {G_LABEL};
        unsigned long k8[] = {base[G_LABEL]};
        faulted(o8, k8, 1, "close fence lost");
        check(shut_downs == 0 && n_release == 0, "the close fence never came back: domain retained, no shutdown",
              "combined 8");
    }
    /* start classification */
    {
        int o[] = {S_START};
        unsigned long k[] = {1};
        faulted(o, k, 1, "start fails after the producer ran (unclassified)");
        check(n_term == 1 && n_join == 1 && n_lane_end == 1 && n_imgclose == 1 && shut_downs == 1,
              "uncertain start: terminate, then join, then lane_end/destroy/image close, then shutdown", "start 1");
        int o2[] = {S_START, S_TERM};
        unsigned long k2[] = {1, 1};
        faulted(o2, k2, 2, "uncertain start + terminate fails");
        check(n_join == 0 && n_lane_end == 0 && n_destroy == 0 && n_imgclose == 0 && shut_downs == 0,
              "uncertain start, terminate failed: everything retained, no shutdown", "start 2");
        int o3[] = {S_START, S_JOIN};
        unsigned long k3[] = {1, 1};
        faulted(o3, k3, 2, "uncertain start + join fails");
        check(n_term == 1 && n_lane_end == 0 && n_destroy == 0 && n_imgclose == 0 && shut_downs == 0,
              "uncertain start, join failed: everything retained, no shutdown", "start 3");
        int o4[] = {S_START_NS};
        unsigned long k4[] = {1};
        faulted(o4, k4, 1, "proven non-start");
        check(!spu_started && n_term == 0 && n_join == 0 && n_lane_end == 1 && n_destroy == 1 && n_imgclose == 1 &&
                  shut_downs == 1,
              "proven non-start: no terminate or join; lane_end, destroy, image close, shutdown", "start 4");
    }
    /* well-identified block replies that fail: no later command, no receipt, the diagnostic row kept */
    for (int which = 0; which < 2; ++which) {
        int op = which ? S_LEDGER : S_EXECRC;
        unsigned long ks[] = {1, 2, base[S_EXECRC] / 2, base[S_EXECRC]};
        for (int i = 0; i < 4; ++i) {
            faulted(&op, &ks[i], 1, which ? "block ledger mismatch" : "block execution rc");
            check(n_cmds == (int)ks[i], "no command after the failed block", which ? "ledger" : "exec rc");
            mfile *f = fs_find("/t/ob.csv.failed");
            check(f != NULL && (which ? strstr(g_log, "ledger differs") != NULL : strstr(f->data, ",1,7,") != NULL),
                  "the diagnostic row is kept in the failed CSV", which ? "ledger" : "exec rc");
            check(shut_downs == 1 && !fs_find("/t/ob.bench-end.txt"), "an orderly teardown without a receipt",
                  which ? "ledger" : "exec rc");
        }
    }
    unsigned long covered = 0;
    for (int op = 0; op < NOPS; ++op)
        covered += per_op[op] > 0;
    printf("  faulted runs %lu over %lu operations\n", runs, covered);
    printf("test_ob_life: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
