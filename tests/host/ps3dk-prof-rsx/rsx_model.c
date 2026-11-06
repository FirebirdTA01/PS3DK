/* P3.3 R1 host model harness for the RSX domain core (prg.c): a GPU emulator executing the submitted
 * timestamp+label pairs in order, the port, a recording sink, and the deterministic traces of the design (r2-r5).
 * Each trace writes <name>.prof (header, CLOCKS, v1 scope-2 DESCRIPTORS, THREADS, the core's chunks, TRAILER unless the
 * sink failed), <name>.truth (what the producer did, per number) and <name>.expect (the trace's exact claims).
 * Host model only: not target evidence; A1/A2 are assumptions.
 *
 * usage: rsx_model OUTDIR [trace...] */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prg.h"

static const char *g_dir;
static int g_bad, g_traces;
#define FAIL(...)                                                                                                      \
    do {                                                                                                               \
        printf("R1 FAIL %s:%d: ", __func__, __LINE__);                                                                \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        g_bad++;                                                                                                       \
    } while (0)
#define WANT(c, ...)                                                                                                   \
    do {                                                                                                               \
        if (!(c))                                                                                                      \
            FAIL(__VA_ARGS__);                                                                                         \
    } while (0)

/* ---------------------------------------------------------------- the GPU emulator */
enum { F_NONE = 0, F_LABEL_FIRST, F_DECREASE, F_LABEL_BACK, F_LABEL_FUTURE, F_TORN, F_LATE_REPORT, F_STALE_MONO };
typedef struct pair {
    uint32_t report, label, value;
} pair;
typedef struct gpu {
    pthread_mutex_t m; /* every report/label access goes through it (host emulation of coherent memory) */
    uint64_t timer[4096];
    uint32_t labelv[256];
    pair q[8192];
    uint32_t qh, qt;     /* queued, executed */
    uint64_t clock;
    int fault, fault_at; /* the fault applies to the fault_at-th executed pair (1-based) */
    int fail_reserve_at; /* the n-th reserve_pair (1-based) fails; 0 none */
    int fail_every;      /* every n-th reserve fails (n >= 2); 0 none */
    int every_count;
    void (*rtimer_hook)(void); /* runs after each report read (the second-label-read cases) */
    int reserves;
    void (*reserve_hook)(void); /* runs inside reserve_pair after the commands are queued (a callback) */
    uint32_t executed;
    uint64_t true_t[65536];  /* the GPU clock when each ticket's pair executed (index: ticket low 16 bits) */
    uint8_t ran[65536];
    int lk_kind, lk_skip, lk_hits; /* the lock-fault injector: the (lk_skip+1)-th call of kind lk_kind fails */
    void (*slock_hook)(void);      /* runs before the slock_hook_at-th snap lock (outside the lock) */
    int slock_hook_at, slock_n;
    void (*rlabel_hook)(void);     /* runs after each label read */
    int tb_mode, tb_small;         /* 1: the timebase is stuck at 1; 2: it restarts small (backward, then advancing) */
    uint64_t now;
    int clock_fail;
    int oob; /* report accesses outside the emulated area */
} gpu;
static gpu G;

static void gpu_reset(void)
{
    pthread_mutex_destroy(&G.m);
    memset(&G, 0, sizeof G);
    pthread_mutex_init(&G.m, NULL);
    G.clock = 1000;
}

/* execute up to n queued pairs in order */
static void gpu_step(uint32_t n)
{
    pthread_mutex_lock(&G.m);
    while (n-- && G.qt < G.qh) {
        pair p = G.q[G.qt++ % 8192];
        int k = (int)++G.executed;
        G.clock += 37;
        G.true_t[(uint32_t)(p.value - 1) & 0xFFFF] = G.clock;
        G.ran[(uint32_t)(p.value - 1) & 0xFFFF] = 1;
        if (G.fault && k == G.fault_at) {
            switch (G.fault) {
            case F_LABEL_FIRST: G.labelv[p.label] = p.value; break;           /* report never written */
            case F_DECREASE: G.timer[p.report] = 5; G.labelv[p.label] = p.value; break;
            case F_LABEL_BACK: G.timer[p.report] = G.clock; G.labelv[p.label] = p.value - 3; break;
            case F_LABEL_FUTURE: G.timer[p.report] = G.clock; G.labelv[p.label] = p.value + 5000; break;
            case F_TORN: G.timer[p.report] = (G.clock & 0xFFFFFFFFull) | (0x00000001ull << 32); G.labelv[p.label] = p.value;
                break; /* a mixed value, non-poison and increasing: undetectable */
            case F_LATE_REPORT: G.labelv[p.label] = p.value; G.timer[p.report] = G.clock; break; /* label first, but the
                                                                         report lands before the PPU reads: undetectable */
            case F_STALE_MONO: G.timer[p.report] = G.clock + 1000000; G.labelv[p.label] = p.value; break; /* wrong but
                                                                                          monotone: undetectable */
            }
            continue;
        }
        G.timer[p.report] = G.clock;
        G.labelv[p.label] = p.value;
    }
    pthread_mutex_unlock(&G.m);
}

/* ---------------------------------------------------------------- the port */
static pthread_mutex_t g_snap = PTHREAD_MUTEX_INITIALIZER, g_ext = PTHREAD_MUTEX_INITIALIZER;
static int p_reserve(void *c, uint32_t report, uint32_t label, uint32_t value)
{
    (void)c;
    pthread_mutex_lock(&G.m);
    if ((G.fail_reserve_at && ++G.reserves == G.fail_reserve_at) ||
        (G.fail_every && ++G.every_count % G.fail_every == 0)) {
        pthread_mutex_unlock(&G.m);
        return -1; /* all or nothing: nothing queued */
    }
    G.q[G.qh++ % 8192] = (pair){report, label, value};
    pthread_mutex_unlock(&G.m);
    if (G.reserve_hook)
        G.reserve_hook();
    return 0;
}
static void p_flush(void *c) { (void)c; }
static uint32_t p_rlabel(void *c, uint32_t l)
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
static void p_wlabel(void *c, uint32_t l, uint32_t v)
{
    (void)c;
    pthread_mutex_lock(&G.m);
    G.labelv[l] = v;
    pthread_mutex_unlock(&G.m);
}
static uint64_t p_rtimer(void *c, uint32_t r)
{
    uint64_t v;
    (void)c;
    if (r >= 4096)
        return G.oob++, PRG_POISON; /* outside the emulated report area: recorded, never an access */
    pthread_mutex_lock(&G.m);
    v = G.timer[r];
    pthread_mutex_unlock(&G.m);
    if (G.rtimer_hook)
        G.rtimer_hook();
    return v;
}
static void p_wtimer(void *c, uint32_t r, uint64_t v)
{
    (void)c;
    if (r >= 4096) {
        G.oob++;
        return;
    }
    pthread_mutex_lock(&G.m);
    G.timer[r] = v;
    pthread_mutex_unlock(&G.m);
}
static void p_sync(void *c) { (void)c; }
static uint64_t g_tb;
static uint64_t p_mftb(void *c)
{
    (void)c;
    if (G.tb_mode == 1)
        return 1;
    if (G.tb_mode == 2)
        return (uint64_t)++G.tb_small;
    return __atomic_add_fetch(&g_tb, 11, __ATOMIC_RELAXED);
}
static int p_now(void *c, uint64_t *us)
{
    (void)c;
    if (G.clock_fail)
        return -1;
    *us = __atomic_add_fetch(&G.now, 10, __ATOMIC_RELAXED); /* every read advances 10 us: deadlines are finite */
    return 0;
}
enum { LK_NONE = 0, LK_SLOCK, LK_SUNLOCK, LK_ELOCK, LK_EUNLOCK };
static int lk_trip(int kind)
{
    if (G.lk_kind != kind)
        return 0;
    if (G.lk_skip) {
        G.lk_skip--;
        return 0;
    }
    G.lk_kind = LK_NONE;
    G.lk_hits++;
    return 1;
}
static int p_slock(void *c)
{
    (void)c;
    if (G.slock_hook && ++G.slock_n == G.slock_hook_at) {
        void (*h)(void) = G.slock_hook;
        G.slock_hook = NULL;
        h();
    }
    return lk_trip(LK_SLOCK) ? -1 : pthread_mutex_lock(&g_snap); /* a refused lock: not taken */
}
static int p_sunlock(void *c)
{
    (void)c;
    pthread_mutex_unlock(&g_snap);
    return lk_trip(LK_SUNLOCK) ? -1 : 0; /* the seam: really unlocked, then the error */
}
static int p_elock(void *c) { (void)c; return lk_trip(LK_ELOCK) ? -1 : pthread_mutex_lock(&g_ext); }
static int p_eunlock(void *c)
{
    (void)c;
    pthread_mutex_unlock(&g_ext);
    return lk_trip(LK_EUNLOCK) ? -1 : 0;
}
/* R2's hold hook: events with the held descriptor are not emittable while g_hold_desc names it */
static uint32_t g_hold_desc = UINT32_MAX;
static int p_emittable(void *c, const prg_meta *m)
{
    (void)c;
    return m->desc != __atomic_load_n(&g_hold_desc, __ATOMIC_ACQUIRE);
}
static const prg_port g_port = {NULL, p_reserve, p_flush, p_rlabel, p_wlabel, p_rtimer, p_wtimer, p_sync, p_mftb, p_now,
                                p_slock, p_sunlock, p_elock, p_eunlock, p_emittable};

/* ---------------------------------------------------------------- the sink */
typedef struct chunk {
    uint32_t type, len;
    uint8_t *data;
} chunk;
static chunk g_ch[4096];
static int g_nch, g_sink_dead, g_fail_type, g_fail_nth, g_seen_type;
static int o_chunk(void *c, uint32_t type, const uint8_t *p, size_t len)
{
    (void)c;
    if (g_sink_dead)
        return -1;
    if (g_fail_type && type == (uint32_t)g_fail_type && ++g_seen_type == g_fail_nth) {
        g_sink_dead = 1;
        return -1;
    }
    g_ch[g_nch].type = type;
    g_ch[g_nch].len = (uint32_t)len;
    g_ch[g_nch].data = malloc(len ? len : 1);
    memcpy(g_ch[g_nch].data, p, len);
    g_nch++;
    return 0;
}
static const prg_out g_out = {NULL, o_chunk};

/* ---------------------------------------------------------------- the capture file */
typedef struct wbuf {
    uint8_t *b;
    size_t n, cap;
} wbuf;
static void wb(wbuf *w, const void *p, size_t n)
{
    if (w->n + n > w->cap) {
        w->cap = (w->n + n) * 2 + 64;
        w->b = realloc(w->b, w->cap);
    }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}
static void wle(wbuf *w, uint64_t v, int bytes)
{
    uint8_t t[8];
    for (int i = 0; i < bytes; i++)
        t[i] = (uint8_t)(v >> (8 * i));
    wb(w, t, (size_t)bytes);
}
static void wstr(wbuf *w, const char *s)
{
    wle(w, strlen(s), 2);
    wb(w, s, strlen(s));
}
static uint32_t crc32b(const uint8_t *d, size_t n)
{
    static uint32_t tab[256];
    uint32_t c = 0xFFFFFFFFu;
    if (!tab[1])
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t x = k;
            for (int j = 0; j < 8; j++)
                x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            tab[k] = x;
        }
    for (size_t i = 0; i < n; i++)
        c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void frame(wbuf *f, uint64_t *seq, uint32_t type, const uint8_t *p, size_t len)
{
    size_t start = f->n;
    wle(f, type, 4);
    wle(f, len, 4);
    wle(f, (*seq)++, 8);
    wb(f, p, len);
    wle(f, crc32b(f->b + start, f->n - start), 4);
}

#define NDESC 4
static void write_capture(const char *name)
{
    wbuf f = {0}, p = {0};
    uint64_t seq = 0;
    char path[1024];
    FILE *fp;
    wb(&f, "PS3DKPRF", 8);
    wle(&f, 1, 2);
    wle(&f, 0, 2);
    wle(&f, 1, 4); /* DOMAIN_EXTENSIONS */
    wstr(&p, "app");
    wstr(&p, "test-build-0001");
    wstr(&p, "0.26.0");
    wstr(&p, "gcc 16.1.0");
    wstr(&p, "-O2");
    wstr(&p, "rsx-model");
    wle(&p, 1, 1);
    wle(&p, 1, 1);
    wle(&p, 0, 2);
    wle(&p, 7, 8);
    wle(&p, 4096, 4);
    wle(&p, 8, 4);
    wle(&p, 16, 4);
    frame(&f, &seq, 1, p.b, p.n);
    p.n = 0;
    wle(&p, 1, 4);
    wle(&p, 1, 4);
    wle(&p, 0, 4);
    wle(&p, 79800000, 8);
    wle(&p, 1000, 8);
    frame(&f, &seq, 2, p.b, p.n);
    p.n = 0; /* v1 DESCRIPTORS: NDESC entries of scope 2 (gpu-complete) */
    wle(&p, NDESC, 4);
    for (int i = 0; i < NDESC; i++) {
        char nm[16];
        /* the v1 descriptor id: FNV-1a-64 over (build_id, 0, file, 0, line LE32, name) */
        uint64_t h = 0xcbf29ce484222325ull;
        uint8_t key[64];
        size_t kn = 0;
        uint32_t line = 10 + (uint32_t)i;
        snprintf(nm, sizeof nm, "gpu_%d", i);
        memcpy(key + kn, "test-build-0001", 15), kn += 15;
        key[kn++] = 0;
        memcpy(key + kn, "gpu.c", 5), kn += 5;
        key[kn++] = 0;
        for (int b = 0; b < 4; b++)
            key[kn++] = (uint8_t)(line >> (8 * b));
        memcpy(key + kn, nm, strlen(nm)), kn += strlen(nm);
        for (size_t b = 0; b < kn; b++)
            h = (h ^ key[b]) * 0x100000001b3ull;
        wle(&p, (uint64_t)i, 4);
        wle(&p, h, 8);
        wle(&p, 2, 1);
        wle(&p, 0, 1);
        wle(&p, 10 + (uint64_t)i, 4);
        wstr(&p, nm);
        wstr(&p, "");
        wstr(&p, "gpu.c");
    }
    frame(&f, &seq, 3, p.b, p.n);
    p.n = 0; /* THREADS: incarnation 1 (the render thread, the frame owner) */
    wle(&p, 1, 4);
    wle(&p, 1, 4);
    wle(&p, 0, 4);
    wle(&p, 1234, 8);
    wstr(&p, "render");
    frame(&f, &seq, 4, p.b, p.n);
    for (int i = 0; i < g_nch; i++)
        frame(&f, &seq, g_ch[i].type, g_ch[i].data, g_ch[i].len);
    if (!g_sink_dead) { /* TRAILER: incarnation 1 ACKED with no v1 records, NDESC descriptors emitted */
        p.n = 0;
        wle(&p, 0, 4);     /* end_flags */
        wle(&p, 1, 4);     /* one final entry */
        wle(&p, 1, 4);     /* incarnation 1 */
        wle(&p, 2, 1);     /* ACKED */
        wle(&p, 0, 1);
        wle(&p, 0, 2);
        for (int k = 0; k < 6; k++)
            wle(&p, 0, 8); /* produced, written, dropped, refused, regerr, open_spans */
        wle(&p, 0, 8);     /* error_count */
        wle(&p, 0, 4);
        wle(&p, 0, 4);
        wle(&p, 0, 8);     /* unregistered */
        wle(&p, NDESC, 8); /* descriptors emitted */
        frame(&f, &seq, 6, p.b, p.n);
    }
    snprintf(path, sizeof path, "%s/%s.prof", g_dir, name);
    fp = fopen(path, "wb");
    if (!fp || fwrite(f.b, 1, f.n, fp) != f.n || fclose(fp) != 0)
        FAIL("write %s", path);
    free(f.b);
    free(p.b);
}

/* ---------------------------------------------------------------- truth (what the producer did, per number) */
typedef struct truth {
    uint64_t seq;
    uint32_t desc;
    uint8_t kind, depth;
    int submitted; /* 0: numbered but lost at submission */
    uint64_t ticket;
} truth;
static truth g_tr[70000];
static int g_ntr;
static void tr_note(prg_dom *d, uint64_t before_seq, uint64_t before_ticket, uint32_t desc, uint8_t kind, uint8_t depth)
{
    if (d->p_seq == before_seq)
        return; /* no number (suppressed or refused) */
    g_tr[g_ntr++] = (truth){before_seq, desc, kind, depth, d->p_ticket != before_ticket, before_ticket};
}

/* ---------------------------------------------------------------- one domain per trace */
static prg_dom D;
static prg_meta g_meta[PRG_SLOTS_MAX];
static prg_stage g_stage[PRG_SLOTS_MAX];
static uint8_t g_scratch[PRG_SCRATCH_BYTES];
static prg_ledger g_led;
static const prg_ledger LED_READER = {PRG_LOSS_CHUNKS_MAX, 0, 0, PRG_BRACKETS_EPOCH_MAX, PRG_BRACKETS_FILE_MAX, 0};

static void fresh_led(uint32_t slots, prg_ledger led)
{
    for (int i = 0; i < g_nch; i++)
        free(g_ch[i].data);
    g_nch = 0;
    g_sink_dead = g_fail_type = g_fail_nth = g_seen_type = 0;
    g_ntr = 0;
    gpu_reset();
    g_led = led;
    WANT(prg_attach(&D, 7, 16, slots, 200, &g_port, &g_out, &g_led, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_OK, "attach");
}
static void fresh(uint32_t slots) { fresh_led(slots, LED_READER); }

static int B(uint32_t desc, prg_token *t)
{
    uint64_t s = D.p_seq, k = D.p_ticket;
    uint8_t depth = (uint8_t)D.p_depth;
    int rc = prg_begin(&D, desc, t);
    tr_note(&D, s, k, desc, PRG_K_BEGIN, depth);
    return rc;
}
static int E(prg_token *t, uint32_t desc)
{
    uint64_t s = D.p_seq, k = D.p_ticket;
    uint8_t depth = (uint8_t)(t->depth & 0x7fffffff);
    int rc = prg_end(&D, t, desc);
    tr_note(&D, s, k, desc, PRG_K_END, depth);
    return rc;
}
static int M(uint32_t desc)
{
    uint64_t s = D.p_seq, k = D.p_ticket;
    uint8_t depth = (uint8_t)D.p_depth; /* depth 64 is refused unnumbered: no truth row */
    int rc = prg_mark(&D, desc);
    tr_note(&D, s, k, desc, PRG_K_MARK, depth);
    return rc;
}

/* write the capture, the truth and the trace's claims */
static FILE *finish(const char *name)
{
    char path[1024];
    FILE *f;
    WANT(!D.final_written || D.retired, "%s: an accepted FINAL belongs to the one retired terminal decision", name);
    WANT(!G.oob, "%s: %d report accesses outside the emulated area", name, G.oob);
    write_capture(name);
    snprintf(path, sizeof path, "%s/%s.truth", g_dir, name);
    f = fopen(path, "w");
    for (int i = 0; i < g_ntr; i++)
    {
        uint32_t k = (uint32_t)g_tr[i].ticket & 0xFFFF;
        /* seq desc kind depth submitted executed true_timer */
        fprintf(f, "%llu %u %u %u %d %d %llu\n", (unsigned long long)g_tr[i].seq, g_tr[i].desc, g_tr[i].kind,
                g_tr[i].depth, g_tr[i].submitted, g_tr[i].submitted && G.ran[k],
                (unsigned long long)(g_tr[i].submitted && G.ran[k] ? G.true_t[k] : 0));
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s.expect", g_dir, name);
    g_traces++;
    f = fopen(path, "w");
    fprintf(f, "final_state %u\nretired %u\nfinal_written %u\nsink_dead %d\n", D.final_state, D.retired,
            D.final_written, g_sink_dead);
    return f;
}

/* ---------------------------------------------------------------- traces */
static void t_steady(void)
{
    prg_token t[3];
    FILE *e;
    fresh(64);
    for (int i = 0; i < 10; i++) {
        WANT(B(0, &t[0]) == 0, "begin");
        WANT(B(1, &t[1]) == 0, "begin nested");
        WANT(E(&t[1], 1) == 0, "end");
        WANT(M(2) == 0, "mark");
        WANT(E(&t[0], 0) == 0, "end");
        gpu_step(100);
        prg_drain(&D);
    }
    prg_seal(&D);
    WANT(D.final_state == 1 && D.final_written && D.events_accepted == 50 && !D.lost_accepted, "steady: FINALIZED 50");
    WANT(prg_release(&D) == PRG_OK && prg_release(&D) == PRG_EINVAL, "release exactly once");
    e = finish("r1-steady");
    fprintf(e, "exit 0\nevents 50\nlost 0\n");
    fclose(e);
}

static void t_full_ring(void)
{
    FILE *e;
    fresh(4);
    for (int i = 0; i < 10; i++)
        WANT(M(0) == 0, "mark %d", i); /* 0..3 submitted, 4..9 lost: the ring is full (nothing drained) */
    WANT(D.p_open_count == 6 && D.p_rec_head == 0, "a private open loss of 6, no record yet");
    gpu_step(100);
    prg_drain(&D); /* events 0..3 emitted; the open loss is still private */
    WANT(M(1) == 0, "the next mark closes the loss into a record, then submits"); /* event 10 */
    WANT(D.p_rec_head == 1 && D.rec[0].first == 4 && D.rec[0].count == 6, "record [4, 6]");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 5 && D.lost_accepted == 6, "FINALIZED, 5 events, LOSS 6");
    e = finish("r1-full-ring");
    fprintf(e, "exit 4\nevents 5\nlost 6\nloss 4 6 3\n");
    fclose(e);
}

static void t_fifo_refusal(void)
{
    FILE *e;
    fresh(64);
    G.fail_reserve_at = 3;
    for (int i = 0; i < 5; i++)
        WANT(M(0) == 0, "mark");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.events_accepted == 4 && D.lost_accepted == 1 && D.st.lost_fifo == 1, "FIFO refusal: LOSS [2, 1]");
    e = finish("r1-fifo-refusal");
    fprintf(e, "exit 4\nevents 4\nlost 1\nloss 2 1 3\n");
    fclose(e);
}

static void t_sink_fail(int type, const char *name)
{
    prg_token t;
    FILE *e;
    fresh(64);
    g_fail_type = type;
    g_fail_nth = 1;
    for (int i = 0; i < 6; i++) {
        WANT(B(0, &t) == 0 && E(&t, 0) == 0, "zone");
        gpu_step(100);
        prg_drain(&D);
    }
    WANT(M(1) == 0, "mark");
    prg_seal(&D); /* a submitted mark pending: trailing loss */
    WANT(D.retired, "retired even when output failed");
    WANT(prg_release(&D) == PRG_OK, "release after a sink failure");
    e = finish(name);
    fprintf(e, "exit 1\n"); /* a failed sink never writes the TRAILER: INCOMPLETE */
    fclose(e);
}

static void t_nested_suppression(void)
{
    prg_token t[70];
    FILE *e;
    fresh(256);
    for (int i = 0; i < 67; i++)
        WANT(B(0, &t[i]) == 0, "begin %d", i);
    WANT(D.p_depth == 64 && D.p_suppressed == 3 && D.p_seq == 64, "64 numbered, 3 suppressed (no numbers)");
    for (int i = 66; i >= 0; i--)
        WANT(E(&t[i], 0) == 0, "end %d", i);
    WANT(D.p_seq == 128 && !D.p_suppressed && !D.p_depth, "128 numbers, back to depth 0");
    gpu_step(1000);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 128, "FINALIZED 128");
    e = finish("r1-nested-suppression");
    fprintf(e, "exit 0\nevents 128\nlost 0\n");
    fclose(e);
}

static void t_end_lost(void)
{
    prg_token t;
    FILE *e;
    fresh(64);
    WANT(B(0, &t) == 0, "begin");
    G.fail_reserve_at = 1; /* the END's pair: the counter counts only reserves made once it is armed */
    WANT(E(&t, 0) == 0, "end numbered, lost");
    WANT(!D.p_depth, "the stack popped");
    WANT(M(1) == 0, "mark after: closes the loss");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.events_accepted == 2 && D.lost_accepted == 1, "BEGIN + MARK, LOSS [1, 1]");
    e = finish("r1-end-lost");
    fprintf(e, "exit 4\nevents 2\nlost 1\nloss 1 1 3\n");
    fclose(e);
}

static void t_label_wrap(void)
{
    FILE *e;
    uint64_t base = 0xFFFFFFFDull;
    fresh(8);
    D.p_ticket = D.completed = D.drained = D.drained_pub = base; /* rebased near the u32 label wrap */
    p_wlabel(NULL, 200, (uint32_t)base);
    for (int i = 0; i < 6; i++) {
        WANT(M(0) == 0, "mark");
        gpu_step(i % 2 ? 2 : 0); /* skipped intermediate label values */
        prg_drain(&D);
    }
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 6, "6 events across the wrap");
    e = finish("r1-label-wrap");
    fprintf(e, "exit 0\nevents 6\nlost 0\n");
    fclose(e);
}

static void drain_in_reserve(void)
{
    gpu_step(100); /* the GPU executes the pair before its ticket is published */
    prg_drain(&D); /* the drainer sees a label ahead of the published ticket: DEFERRED, nothing read */
}
static void t_deferral(void)
{
    FILE *e;
    fresh(8);
    G.reserve_hook = drain_in_reserve;
    WANT(M(0) == 0, "mark");
    G.reserve_hook = NULL;
    WANT(!D.anomaly && D.drained == 0, "deferred, not an anomaly");
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 1, "drained after the publication");
    e = finish("r1-deferral");
    fprintf(e, "exit 0\nevents 1\nlost 0\n");
    fclose(e);
}

static void t_seal_pending(void)
{
    FILE *e;
    fresh(16);
    for (int i = 0; i < 5; i++)
        WANT(M(0) == 0, "mark");
    gpu_step(2);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 2 && D.events_accepted == 2 && D.lost_accepted == 3, "ABANDONED, trailing LOSS 3");
    WANT(prg_release(&D) == PRG_OK, "release after the app's quiescence");
    e = finish("r1-seal-pending");
    fprintf(e, "exit 4\nevents 2\nlost 3\nloss 2 3 4\n");
    fclose(e);
}

static void t_detach_retry(void)
{
    FILE *e;
    fresh(16);
    for (int i = 0; i < 3; i++)
        WANT(M(0) == 0, "mark");
    WANT(prg_detach(&D, 50, NULL, NULL) == PRG_ETIMEDOUT && D.state == PRG_DETACHING, "timeout keeps DETACHING");
    WANT(prg_release(&D) == PRG_EBUSY, "no release before retirement");
    gpu_step(100);
    prg_drain(&D);
    WANT(prg_detach(&D, 1000, NULL, NULL) == PRG_OK && D.final_state == 1, "retry: FINALIZED");
    prg_seal(&D); /* the seal finds DETACH_FINAL: nothing */
    WANT(prg_release(&D) == PRG_OK, "released once");
    e = finish("r1-detach-retry");
    fprintf(e, "exit 0\nevents 3\nlost 0\n");
    fclose(e);
}

static void t_detach_then_shutdown(void)
{
    FILE *e;
    fresh(16);
    WANT(M(0) == 0, "mark");
    WANT(prg_detach(&D, 30, NULL, NULL) == PRG_ETIMEDOUT, "timeout");
    prg_seal(&D); /* shutdown seals DETACHING without waiting */
    WANT(D.state == PRG_SEALED && D.final_state == 2, "SEALED from DETACHING, ABANDONED");
    WANT(prg_detach(&D, 30, NULL, NULL) == PRG_ESTOPPED, "a later detach sees the frozen result");
    gpu_step(100); /* the app's Finish */
    WANT(prg_release(&D) == PRG_OK, "release");
    e = finish("r1-detach-then-shutdown");
    fprintf(e, "exit 4\nevents 0\nlost 1\nloss 0 1 4\n");
    fclose(e);
}

static void t_calibrate(void)
{
    FILE *e;
    fresh(16);
    G.reserve_hook = NULL;
    /* success: the GPU executes during the flush window (the hook steps it) */
    G.reserve_hook = drain_in_reserve; /* executes the pair; the drainer meets an ACTIVE cell and stops there */
    WANT(prg_calibrate(&D, 1000) == PRG_OK, "calibrate");
    G.reserve_hook = NULL;
    WANT(D.p_br_head == 1, "one bracket");
    prg_drain(&D); /* CONSUMED: the slot is poisoned and freed */
    WANT(D.cal_state == PRG_CAL_IDLE && D.drained == 1, "the drainer freed the calibration slot");
    /* timeout, then the late completion is only drained */
    WANT(prg_calibrate(&D, 30) == PRG_ETIMEDOUT && D.cal_state == PRG_CAL_CANCELLED, "timeout -> CANCELLED");
    gpu_step(100);
    prg_drain(&D);
    WANT(D.cal_state == PRG_CAL_IDLE && D.p_br_head == 1, "the late completion is drained, no bracket");
    WANT(M(0) == 0, "mark");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.st.brackets == 1, "FINALIZED with one CALIBRATION bracket");
    e = finish("r1-calibrate");
    fprintf(e, "exit 0\nevents 1\nlost 0\nbrackets 1\n");
    fclose(e);
}

static void t_overflow(void)
{
    FILE *e;
    fresh(16);
    D.p_seq = UINT64_MAX - 2;
    D.next_emit = UINT64_MAX - 2;
    WANT(M(0) == 0 && M(0) == 0, "two marks");
    WANT(M(0) == PRG_EOVERFLOW && D.overflow, "the third exhausts: OVERFLOWED");
    WANT(M(0) == PRG_ESTOPPED, "then refused, no number");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 3, "FINAL OVERFLOWED");
    e = finish("r1-overflow");
    fprintf(e, "noreader\n"); /* the rebased seq is not a valid file start */
    fclose(e);
}

/* the fault hits the LAST of four pairs, after a first drain has accepted two (a baseline for the window check);
 * `events` is how many the runtime accepts before it stops (detected) or 4 (undetected counterexample) */
static void t_fault(int fault, const char *name, int detected, int events)
#define BENIGN(n) (strstr((n), "benign") != NULL)
{
    FILE *e;
    fresh(16);
    G.fault = fault;
    G.fault_at = 4;
    for (int i = 0; i < 4; i++)
        WANT(M(0) == 0, "mark");
    gpu_step(2);
    prg_drain(&D);
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    if (detected)
        WANT(D.anomaly && D.final_state == 2 && D.events_accepted == (uint64_t)events, "%s: detected, %d events + "
             "LOSS 4 + ABANDONED (got anomaly %u state %u events %llu)", name, events, D.anomaly, D.final_state,
             (unsigned long long)D.events_accepted);
    else
        WANT(!D.anomaly && D.final_state == 1 && D.events_accepted == 4, "%s: UNDETECTED (raw only)", name);
    e = finish(name);
    if (detected)
        fprintf(e, "exit 4\nevents %d\nlost %d\nloss %d %d 4\n", events, 4 - events, events, 4 - events);
    else
        fprintf(e, "exit 0\nevents 4\nlost 0\n%s\n", BENIGN(name) ? "undetected-benign 1" /* value correct anyway */
                                                                  : "undetected 1"); /* the checker sees the wrong value */
    fclose(e);
}


/* ---- r4 intervals: 16 records occupied, then a success, then a loss, then a success with no credit ---- */
static void t_intervals(void)
{
    FILE *e;
    fresh(256);
    G.fail_every = 2; /* reserves 2, 4, 6, ... fail: success, loss, success, loss ... */
    for (int i = 0; i < 33; i++)
        WANT(M(0) == 0, "mark %d", i);
    WANT(D.p_rec_head == 16 && D.rec_tail == 0, "16 records published, none retired (got head %llu)",
         (unsigned long long)D.p_rec_head);
    G.fail_every = 0;
    WANT(M(1) == 0, "the next mark");
    G.fail_reserve_at = 1;
    WANT(M(2) == 0, "a loss");
    G.fail_reserve_at = 0;
    {
        uint64_t before = D.p_open_count;
        WANT(M(3) == 0 && D.p_open_count == before + 1, "no credit: the next success is folded into the open loss");
    }
    gpu_step(1000);
    prg_drain(&D); /* retires records */
    WANT(M(3) == 0 && D.p_open_count == 0, "with credit the success first closes the open loss");
    gpu_step(1000);
    prg_drain(&D);
    prg_seal(&D);
    e = finish("r1-intervals");
    fprintf(e, "exit 4\nevents %llu\nlost %llu\n", (unsigned long long)D.events_accepted,
            (unsigned long long)D.lost_accepted);
    fclose(e);
}

/* ---- the second label read advances while the drainer reads the reports ---- */
static int g_adv;
static void advance_once(void)
{
    if (!g_adv++)
        gpu_step(1); /* one more pair completes between L1 and L2: consistent, deferred to the next drain */
}
static void t_second_label_advance(void)
{
    FILE *e;
    fresh(16);
    for (int i = 0; i < 4; i++)
        WANT(M(0) == 0, "mark");
    gpu_step(2);
    g_adv = 0;
    G.rtimer_hook = advance_once;
    prg_drain(&D);
    G.rtimer_hook = NULL;
    WANT(!D.anomaly && D.drained == 2, "L2 advanced legitimately: consistent, 2 drained");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 4, "FINALIZED 4");
    e = finish("r1-second-label-advance");
    fprintf(e, "exit 0\nevents 4\nlost 0\n");
    fclose(e);
}

/* ---- first use over arbitrary old memory: attach poisons every slot first ---- */
static void t_first_use_stale(void)
{
    FILE *e;
    for (int i = 0; i < g_nch; i++)
        free(g_ch[i].data);
    g_nch = 0;
    g_sink_dead = g_fail_type = g_fail_nth = g_seen_type = 0;
    g_ntr = 0;
    gpu_reset();
    for (int i = 0; i < 4096; i++)
        G.timer[i] = 999999999; /* old reports from a previous user of the range: large and monotone-looking */
    g_led = LED_READER;
    WANT(prg_attach(&D, 7, 16, 16, 200, &g_port, &g_out, &g_led, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_OK, "attach");
    G.fault = F_LABEL_FIRST;
    G.fault_at = 1;
    WANT(M(0) == 0, "mark");
    gpu_step(10);
    prg_drain(&D);
    WANT(D.anomaly, "the first-use label-before-report reads the POISON, not the old memory");
    prg_seal(&D);
    e = finish("r1-first-use-stale");
    fprintf(e, "exit 4\nevents 0\nlost 1\nloss 0 1 4\n");
    fclose(e);
}

/* ---- the inflight handshake: the seal runs while a zone call is paused inside its reserve callback ---- */
static int g_rel_in_hook;
static void seal_in_reserve(void)
{
    G.reserve_hook = NULL;
    prg_seal(&D); /* the call is admitted and in flight; the seal freezes the published snapshot without waiting */
    g_rel_in_hook = prg_release(&D);
}
static void t_seal_inflight(void)
{
    FILE *e;
    fresh(16);
    WANT(M(0) == 0, "mark 0");
    gpu_step(10);
    prg_drain(&D);
    G.reserve_hook = seal_in_reserve;
    WANT(M(1) == 0, "mark 1, paused in its reserve callback across the seal");
    WANT(g_rel_in_hook == PRG_EBUSY, "release refused while the call is in flight");
    WANT(D.final_state == 1 && D.events_accepted == 1, "FINAL from the frozen snapshot: next_seq 1, FINALIZED");
    WANT(M(2) == PRG_ESTOPPED, "a later call is refused at admission");
    gpu_step(10); /* the app's Finish: the late pair executes into still-borrowed resources */
    WANT(prg_release(&D) == PRG_OK, "release after the call's exit and the app's quiescence");
    g_ntr = 1; /* the frozen file covers number 0 only; number 1 lies beyond the frontier */
    e = finish("r1-seal-inflight");
    fprintf(e, "exit 0\nevents 1\nlost 0\n");
    fclose(e);
}

/* ---- a calibration paused in its reserve callback across the seal, no numbered event ---- */
static void t_seal_calibrating(void)
{
    FILE *e;
    fresh(16);
    G.reserve_hook = seal_in_reserve;
    (void)prg_calibrate(&D, 20);
    WANT(g_rel_in_hook == PRG_EBUSY, "release refused while calibrate is in flight");
    WANT(D.final_state == 1 && D.retired, "FINALIZED next_seq 0 (no numbered event)");
    gpu_step(10);
    WANT(prg_release(&D) == PRG_OK, "release after calibrate returned");
    e = finish("r1-seal-calibrating");
    fprintf(e, "exit 0\nevents 0\nlost 0\n");
    fclose(e);
}

/* ---- detach with a private open loss at the end: it completes, the open loss is emitted as reason 3 ---- */
static void t_detach_open_loss(void)
{
    FILE *e;
    fresh(4);
    for (int i = 0; i < 7; i++)
        WANT(M(0) == 0, "mark %d", i); /* 0..3 submitted, 4..6 a private open loss (the ring is full) */
    gpu_step(100);
    prg_drain(&D);
    WANT(prg_detach(&D, 1000, NULL, NULL) == PRG_OK, "detach completes despite the open loss");
    WANT(D.final_state == 1 && D.events_accepted == 4 && D.lost_accepted == 3, "FINALIZED, 4 events, LOSS 3");
    WANT(prg_release(&D) == PRG_OK, "release");
    e = finish("r1-detach-open-loss");
    fprintf(e, "exit 4\nevents 4\nlost 3\nloss 4 3 3\n");
    fclose(e);
}

/* ---- L1 inside the window, then an UNPUBLISHED pair completes before L2: L2 runs past the published ticket ----
 * (found by the threaded runs: deferral must cap at the published ticket, not fall back to `completed`) */
static void step_one(void)
{
    G.rtimer_hook = NULL;
    gpu_step(1); /* the in-flight call's pair (queued, its ticket not yet published) */
}
static void drain_mid_call(void)
{
    G.reserve_hook = NULL;
    gpu_step(1);               /* only the published pair: L1 = 1 */
    G.rtimer_hook = step_one;  /* after the report read, the unpublished pair completes: L2 = 2 */
    prg_drain(&D);
}
static void t_label_beyond_issued(void)
{
    FILE *e;
    fresh(16);
    WANT(M(0) == 0, "mark 0 (published, not executed)");
    G.reserve_hook = drain_mid_call;
    WANT(M(1) == 0, "mark 1, the drain runs inside its reserve callback");
    WANT(!D.anomaly && D.drained == 1, "L2 past the published ticket is consistent (got anomaly %u drained %llu)",
         D.anomaly, (unsigned long long)D.drained);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 2, "FINALIZED 2");
    e = finish("r1-label-beyond-issued");
    fprintf(e, "exit 0\nevents 2\nlost 0\n");
    fclose(e);
}

/* ---- the seal wins against detach's polling ---- */
static void seal_from_poll(void *c)
{
    (void)c;
    prg_seal(&D);
}
static void t_detach_vs_seal(void)
{
    FILE *e;
    fresh(16);
    WANT(M(0) == 0, "mark");
    WANT(prg_detach(&D, 1000, seal_from_poll, NULL) == PRG_ESTOPPED, "the seal won: detach returns ESTOPPED");
    WANT(D.state == PRG_SEALED && D.final_state == 2, "one FINAL, from the seal (ABANDONED: the mark pending)");
    gpu_step(10);
    WANT(prg_release(&D) == PRG_OK && prg_release(&D) == PRG_EINVAL, "release once");
    e = finish("r1-detach-vs-seal");
    fprintf(e, "exit 4\nevents 0\nlost 1\nloss 0 1 4\n");
    fclose(e);
}

/* ================================================================ the R1 corrections (review 11:52) */

/* ---- 1. the fault policy at every lock site, the seam (the real unlock, then the error) included.  A fault latches:
 * no admission, no work, no finalization from an unproven snapshot; only a fault AFTER a completed finalization (its
 * closing ext unlock) leaves a FINAL and a release behind ---- */
static void step_in_reserve(void) { gpu_step(100); }
static void drain_poll(void *c)
{
    (void)c;
    prg_drain(&D);
}
enum { S_PRODUCER, S_CALIBRATE, S_DRAIN, S_SEAL, S_DETACH };
static void t_lockfault(const char *name, int site, int kind, int skip, int rc_want, int events, int final)
{
    FILE *e;
    int rc = 0;
    fresh(16);
    WANT(M(0) == 0 && M(0) == 0, "%s: two marks", name);
    gpu_step(100);
    prg_drain(&D); /* events 0 and 1 accepted: the proven baseline */
    WANT(M(0) == 0, "%s: mark 2", name);
    gpu_step(100);
    G.lk_kind = kind;
    G.lk_skip = skip;
    switch (site) {
    case S_PRODUCER: rc = M(1); break; /* numbered and submitted, then its publication fails */
    case S_CALIBRATE:
        G.reserve_hook = step_in_reserve;
        rc = prg_calibrate(&D, 1000);
        G.reserve_hook = NULL;
        break;
    case S_DRAIN: prg_drain(&D); break;
    case S_SEAL: prg_seal(&D); break;
    case S_DETACH: rc = prg_detach(&D, 1000, drain_poll, NULL); break;
    }
    G.lk_kind = LK_NONE;
    WANT(G.lk_hits == 1, "%s: the fault was injected once (hits %d)", name, G.lk_hits);
    WANT(rc == rc_want, "%s: rc %d, want %d", name, rc, rc_want);
    WANT(D.fault, "%s: the fault latched", name);
    WANT(M(2) == PRG_ESTOPPED && prg_calibrate(&D, 10) == PRG_ESTOPPED, "%s: no admission after the fault", name);
    prg_drain(&D);
    prg_seal(&D);
    WANT(prg_detach(&D, 10, NULL, NULL) == PRG_EFAULT, "%s: detach reports the fault", name);
    WANT(D.events_accepted == (uint64_t)events, "%s: %llu events, want %d", name,
         (unsigned long long)D.events_accepted, events);
    if (final)
        WANT(D.retired && D.final_written && D.final_state == 1 && prg_release(&D) == PRG_OK,
             "%s: the finalization completed before the fault: its FINAL stands, release once", name);
    else
        WANT(!D.retired && !D.final_written && prg_release(&D) == PRG_EBUSY,
             "%s: no FINAL, never retired, the resources stay borrowed", name);
    e = finish(name);
    fprintf(e, "exit %d\nevents %d\nlost 0\n", final ? 0 : 4, events);
    fclose(e);
}

/* ---- 2. the second label read rejects the interval AFTER valid timers, with a LOSS record that forces an
 * intermediate events flush: nothing of the interval may reach the output, the poison or drained_pub ---- */
static int g_reads;
static void label_back_on_third(void)
{
    if (++g_reads == 3) {
        G.rtimer_hook = NULL;
        p_wlabel(NULL, 200, 1); /* backward: L1 saw 3 */
    }
}
static void t_l2_reject_flush(void)
{
    FILE *e;
    fresh(16);
    WANT(M(0) == 0, "event 0");
    G.fail_reserve_at = 1;
    WANT(M(0) == 0, "event 1, lost");
    G.fail_reserve_at = 0;
    WANT(M(1) == 0 && M(2) == 0, "event 2 closes the record [1, 1]; event 3");
    gpu_step(100);
    g_reads = 0;
    G.rtimer_hook = label_back_on_third;
    prg_drain(&D);
    G.rtimer_hook = NULL;
    WANT(D.anomaly && !D.events_accepted && !D.lost_accepted && !D.drained && !D.drained_pub && !D.rec_tail,
         "the rejected interval committed nothing (events %llu lost %llu drained %llu)",
         (unsigned long long)D.events_accepted, (unsigned long long)D.lost_accepted, (unsigned long long)D.drained);
    WANT(p_rtimer(NULL, 16) != PRG_POISON, "slot 0 was not poisoned");
    prg_seal(&D);
    WANT(D.final_state == 2 && !D.events_accepted && D.lost_accepted == 4, "ABANDONED, one trailing LOSS [0, 4)");
    e = finish("r1-l2-reject-flush");
    fprintf(e, "exit 4\nevents 0\nlost 4\nloss 0 4 4\n");
    fclose(e);
}

/* ---- 2. a two-slot ring refilled on every reuse: the producer runs during the drainer's read phase (the hook) and
 * must find every slot still owned until the interval is committed ---- */
static void refill(void) { (void)M(1); }
static void t_small_ring_refill(void)
{
    FILE *e;
    fresh(2);
    WANT(M(0) == 0 && M(0) == 0, "tickets 0 and 1 fill the ring");
    gpu_step(100);
    G.rtimer_hook = refill;
    prg_drain(&D);
    G.rtimer_hook = NULL;
    WANT(D.st.lost_ring == 2 && D.drained == 2 && D.events_accepted == 2,
         "both refills found the ring full: no slot freed before the second label read (lost_ring %llu)",
         (unsigned long long)D.st.lost_ring);
    WANT(M(2) == 0 && M(2) == 0, "two more: the first closes the loss [2, 2]");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 4 && D.lost_accepted == 2, "FINALIZED, 4 events, LOSS 2");
    e = finish("r1-small-ring-refill");
    fprintf(e, "exit 4\nevents 4\nlost 2\nloss 2 2 3\n");
    fclose(e);
}

/* ---- 3. calibration ---- */
static void label_zero(void)
{
    G.rtimer_hook = NULL;
    p_wlabel(NULL, 200, 0); /* the second bracket label goes backward ... */
    G.rlabel_hook = NULL;
}
static int g_rl;
static void label_restore(void)
{
    if (++g_rl == 1) { /* ... and is restored after the caller's read: the drainer sees the true label */
        G.rlabel_hook = NULL;
        p_wlabel(NULL, 200, 1);
    }
}
static void arm_restore(void)
{
    label_zero();
    g_rl = 0;
    G.rlabel_hook = label_restore;
}
static void t_cal_bad_second_label(void)
{
    FILE *e;
    fresh(16);
    G.reserve_hook = step_in_reserve;
    G.rtimer_hook = arm_restore;
    WANT(prg_calibrate(&D, 1000) == PRG_EANOMALY && D.cal_state == PRG_CAL_BAD, "a bad second label: BAD");
    G.reserve_hook = NULL;
    WANT(D.p_br_head == 0, "no bracket enqueued");
    prg_drain(&D);
    WANT(D.anomaly, "the drainer latched the cell's anomaly");
    prg_seal(&D);
    WANT(D.final_state == 2, "ABANDONED");
    e = finish("r1-cal-bad-second-label");
    fprintf(e, "exit 4\nevents 0\nlost 0\nbrackets 0\n");
    fclose(e);
}
static void t_cal_poison(void)
{
    FILE *e;
    fresh(16);
    G.fault = F_LABEL_FIRST; /* the calibration pair's label lands, its report never does */
    G.fault_at = 1;
    G.reserve_hook = step_in_reserve;
    WANT(prg_calibrate(&D, 1000) == PRG_EANOMALY && D.cal_state == PRG_CAL_BAD, "a poisoned bracket report: BAD");
    G.reserve_hook = NULL;
    prg_drain(&D);
    WANT(D.anomaly && D.st.cal_bad == 1, "the anomaly policy, not a silent drop");
    prg_seal(&D);
    WANT(D.final_state == 2, "ABANDONED");
    e = finish("r1-cal-poison");
    fprintf(e, "exit 4\nevents 0\nlost 0\nbrackets 0\n");
    fclose(e);
}
static void clock_dies(void) { G.clock_fail = 1; }
static void t_cal_clock_fail(void)
{
    FILE *e;
    fresh(16);
    G.reserve_hook = clock_dies; /* the clock fails while the caller polls */
    WANT(prg_calibrate(&D, 1000) == PRG_ECLOCK && D.cal_state == PRG_CAL_CANCELLED, "ECLOCK (not ETIMEDOUT), CANCELLED");
    G.reserve_hook = NULL;
    G.clock_fail = 0;
    gpu_step(100);
    prg_drain(&D);
    WANT(D.cal_state == PRG_CAL_IDLE && D.drained == 1, "the late completion is consumed");
    WANT(M(0) == 0, "mark");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 1, "FINALIZED");
    e = finish("r1-cal-clock-fail");
    fprintf(e, "exit 0\nevents 1\nlost 0\nbrackets 0\n");
    fclose(e);
}
static void tb_stuck(void)
{
    G.rtimer_hook = NULL;
    G.tb_mode = 1;
}
static void t_cal_tb_order(void)
{
    FILE *e;
    fresh(16);
    G.reserve_hook = step_in_reserve;
    WANT(prg_calibrate(&D, 1000) == PRG_OK, "a good bracket first");
    prg_drain(&D);
    G.tb_mode = 2; /* the timebase restarts small: the next bracket would start before the last one ended */
    WANT(prg_calibrate(&D, 1000) == PRG_ECLOCK && D.cal_state == PRG_CAL_CONSUMED, "overlap: ECLOCK, no bracket");
    prg_drain(&D);
    G.tb_mode = 0;
    G.rtimer_hook = tb_stuck; /* tb1 == tb0 */
    WANT(prg_calibrate(&D, 1000) == PRG_ECLOCK, "a timebase that did not advance: ECLOCK");
    G.tb_mode = 0;
    G.reserve_hook = NULL;
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.p_br_head == 1, "FINALIZED with the one good bracket");
    e = finish("r1-cal-tb-order");
    fprintf(e, "exit 0\nevents 0\nlost 0\nbrackets 1\n");
    fclose(e);
}
static void seal_now(void)
{
    G.rtimer_hook = NULL;
    prg_seal(&D);
}
static void t_cal_seal_during(void)
{
    FILE *e;
    fresh(16);
    G.reserve_hook = step_in_reserve;
    G.rtimer_hook = seal_now; /* the seal runs between the caller's completion and its outcome */
    WANT(prg_calibrate(&D, 1000) == PRG_ESTOPPED, "the finalization took the cell: ESTOPPED");
    G.reserve_hook = NULL;
    WANT(D.final_state == 1 && D.drained == 1 && D.cal_state == PRG_CAL_IDLE && D.p_br_head == 0,
         "FINALIZED, the slot consumed, no bracket (state %u cal %u)", D.final_state, D.cal_state);
    WANT(prg_release(&D) == PRG_OK, "release after the call returned");
    e = finish("r1-cal-seal-during");
    fprintf(e, "exit 0\nevents 0\nlost 0\nbrackets 0\n");
    fclose(e);
}
static void t_cal_frontier_frozen(void)
{
    FILE *e;
    fresh(16);
    G.reserve_hook = step_in_reserve;
    G.slock_n = 0;
    G.slock_hook_at = 2; /* the caller's second publication (the bracket's): the seal runs just before it */
    G.slock_hook = seal_now;
    WANT(prg_calibrate(&D, 1000) == PRG_OK, "the caller completed its bracket");
    G.reserve_hook = NULL;
    WANT(D.final_state == 1 && D.p_br_head == 1 && D.br_tail == 0,
         "FINALIZED from the frozen bracket frontier: the unpublished bracket is beyond it");
    WANT(prg_release(&D) == PRG_OK, "release");
    e = finish("r1-cal-frontier-frozen");
    fprintf(e, "exit 0\nevents 0\nlost 0\nbrackets 0\n");
    fclose(e);
}

/* ---- 4. pairing ---- */
static void t_pair_desc(void)
{
    FILE *e;
    prg_token t, x;
    fresh(16);
    WANT(B(1, &t) == 0, "begin desc 1");
    WANT(E(&t, 2) == PRG_EINVAL, "END with another descriptor: refused, no number");
    x = t;
    x.desc = 2; /* a forged token: its own desc matches the call, the stack's does not */
    WANT(E(&x, 2) == PRG_EINVAL, "the stack's BEGIN descriptor is checked");
    x = t;
    x.begin_seq = 99;
    WANT(E(&x, 1) == PRG_EINVAL, "a token for another BEGIN");
    x = (prg_token){0, 0, 0, 1, 0};
    WANT(E(&x, 1) == PRG_EINVAL, "an unissued token (domain 0)");
    WANT(E(&t, 1) == 0, "the true END");
    WANT(E(&t, 1) == PRG_EINVAL, "the same token again: refused");
    WANT(D.p_seq == 2 && D.st.refused_api == 5, "two numbers, five refusals");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 2, "FINALIZED 2");
    e = finish("r1-pair-desc");
    fprintf(e, "exit 0\nevents 2\nlost 0\n");
    fclose(e);
}
static void t_mark_depth64(void)
{
    FILE *e;
    prg_token t[66];
    fresh(256);
    for (int i = 0; i < 64; i++)
        WANT(B(0, &t[i]) == 0, "begin %d", i);
    WANT(M(1) == PRG_EOVERFLOW && D.st.refused_mark_depth == 1, "a MARK at depth 64: refused, no number");
    WANT(B(0, &t[64]) == 0 && M(1) == PRG_EOVERFLOW, "and inside suppressed nesting");
    WANT(E(&t[64], 0) == 0, "the suppressed zone ends");
    WANT(D.p_seq == 64, "no number for either refused MARK");
    for (int i = 63; i >= 0; i--)
        WANT(E(&t[i], 0) == 0, "end %d", i);
    WANT(M(1) == 0, "a MARK at depth 0");
    gpu_step(1000);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 129, "FINALIZED 129");
    e = finish("r1-mark-depth64");
    fprintf(e, "exit 0\nevents 129\nlost 0\n");
    fclose(e);
}
static void t_pair_suppressed(void)
{
    FILE *e;
    prg_token t[64], s1, s2;
    fresh(256);
    for (int i = 0; i < 64; i++)
        WANT(B(0, &t[i]) == 0, "begin %d", i);
    WANT(B(5, &s1) == 0 && B(6, &s2) == 0, "two suppressed");
    WANT(E(&s1, 5) == PRG_EINVAL, "the outer suppressed zone cannot end first");
    WANT(E(&t[63], 0) == PRG_EINVAL, "a numbered zone cannot end inside suppressed nesting");
    WANT(E(&s2, 6) == 0, "the innermost ends");
    WANT(E(&s2, 6) == PRG_EINVAL, "reused: refused");
    WANT(E(&s1, 5) == 0, "then the outer");
    for (int i = 63; i >= 0; i--)
        WANT(E(&t[i], 0) == 0, "end %d", i);
    WANT(E(&t[0], 0) == PRG_EINVAL, "a reused numbered token at depth 0");
    WANT(D.p_seq == 128 && !D.p_suppressed, "128 numbers");
    gpu_step(1000);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 128, "FINALIZED 128");
    e = finish("r1-pair-suppressed");
    fprintf(e, "exit 0\nevents 128\nlost 0\n");
    fclose(e);
}

/* ---- 5. aggregate bounds ---- */
static prg_ledger led(uint32_t loss_max, uint32_t br_epoch, uint32_t br_file)
{
    return (prg_ledger){loss_max, 0, 0, br_epoch, br_file, 0};
}
/* success, loss, success, loss, success, loss, success: three loss records */
static void three_records(void)
{
    G.fail_every = 2;
    for (int i = 0; i < 7; i++)
        WANT(M(0) == 0, "mark %d", i);
    G.fail_every = 0;
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
}
static void t_loss_budget_exact(void)
{
    FILE *e;
    fresh_led(16, led(4, 100, 100)); /* three LOSS chunks + the reserved terminal: exactly the limit */
    three_records();
    WANT(D.final_state == 1 && D.events_accepted == 4 && D.lost_accepted == 3 && !D.loss_exhausted,
         "all three LOSS within budget");
    WANT(g_led.loss_used == 3 && g_led.loss_reserved == 0, "3 used, the unused terminal returned");
    e = finish("r1-loss-budget-exact");
    fprintf(e, "exit 4\nevents 4\nlost 3\nloss 1 1 3\nloss 3 1 3\nloss 5 1 3\nloss-chunks 3\n");
    fclose(e);
}
static void t_loss_budget_over(void)
{
    FILE *e;
    fresh_led(16, led(3, 100, 100)); /* one under: the third non-terminal LOSS has no credit */
    three_records();
    WANT(D.final_state == 3 && D.loss_exhausted && D.exhausted && D.st.loss_exhausted == 1,
         "exhausted: FINAL OVERFLOWED (got %u)", D.final_state);
    WANT(D.events_accepted == 3 && D.lost_accepted == 4, "events 0 2 4; the terminal LOSS covers [5, 7)");
    WANT(g_led.loss_used == 3 && g_led.loss_reserved == 0, "exactly the limit used");
    e = finish("r1-loss-budget-over");
    fprintf(e, "exit 4\nevents 3\nlost 4\nloss 1 1 3\nloss 3 1 3\nloss 5 2 3\nloss-chunks 3\nexhausted 1\n");
    fclose(e);
}
static void t_attach_bounds(void)
{
    FILE *e;
    static prg_dom D2;
    prg_ledger l = led(1, 1, 1);
    fresh_led(16, led(1, 100, 100)); /* the one credit is the terminal reservation */
    WANT(g_led.loss_reserved == 1, "attach reserved the terminal LOSS");
    WANT(prg_attach(&D2, 8, 64, 16, 201, &g_port, &g_out, &g_led, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_EINVAL,
         "a second domain without a terminal credit is refused");
    l.loss_max = 0;
    WANT(prg_attach(&D2, 8, 64, 16, 201, &g_port, &g_out, &l, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_EINVAL,
         "a zero ledger is refused");
    l.loss_max = 1;
    WANT(prg_attach(&D2, 8, UINT32_MAX - 15, 16, 201, &g_port, &g_out, &l, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) ==
             PRG_EINVAL && l.loss_reserved == 0,
         "report_first + report_count past u32: refused before any arithmetic or reservation");
    WANT(prg_attach(&D2, 8, UINT32_MAX - 16, 16, 201, &g_port, &g_out, &l, NULL, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_EINVAL,
         "no meta");
    /* injected maxima above the reader's hard ceilings are refused; exactly the ceilings are accepted */
    l = LED_READER;
    l.loss_max++;
    WANT(prg_attach(&D2, 8, 64, 16, 201, &g_port, &g_out, &l, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_EINVAL,
         "loss_max over the reader ceiling");
    l = LED_READER;
    l.br_epoch_max++;
    WANT(prg_attach(&D2, 8, 64, 16, 201, &g_port, &g_out, &l, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_EINVAL,
         "br_epoch_max over the reader ceiling");
    l = LED_READER;
    l.br_file_max++;
    WANT(prg_attach(&D2, 8, 64, 16, 201, &g_port, &g_out, &l, g_meta, g_stage, g_scratch, PRG_EVENTS_MAX) == PRG_EINVAL,
         "br_file_max over the reader ceiling");
    l = LED_READER;
    WANT(prg_attach(&D2, 8, 64, 16, 201, &g_port, &g_out, &l, g_meta + 64, g_stage + 64, g_scratch, PRG_EVENTS_MAX) == PRG_OK &&
             l.loss_reserved == 1,
         "exactly the reader ceilings: accepted");
    WANT(M(0) == 0, "mark");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && g_led.loss_used == 0 && g_led.loss_reserved == 0, "FINALIZED, the reservation returned");
    e = finish("r1-attach-bounds");
    fprintf(e, "exit 0\nevents 1\nlost 0\n");
    fclose(e);
}
static void cal_n(int n)
{
    G.reserve_hook = step_in_reserve;
    for (int i = 0; i < n; i++) {
        WANT(prg_calibrate(&D, 1000) == PRG_OK, "calibrate %d", i);
        prg_drain(&D);
    }
    G.reserve_hook = NULL;
    prg_seal(&D);
}
static void t_cal_budget(const char *name, prg_ledger l, int n, int kept)
{
    FILE *e;
    fresh_led(16, l);
    cal_n(n);
    WANT(D.final_state == 1 && D.br_seq == (uint64_t)kept && D.st.brackets_dropped == (uint64_t)(n - kept),
         "%s: %d kept, %d dropped (got %llu, %llu)", name, kept, n - kept, (unsigned long long)D.br_seq,
         (unsigned long long)D.st.brackets_dropped);
    e = finish(name);
    fprintf(e, "exit 0\nevents 0\nlost 0\nbrackets %d\n", kept);
    fclose(e);
}

/* ================================================================ the r1c3 carries (review 12:24) */

/* ---- 1. LOSS-budget exhaustion is a sticky admission stop and FINAL OVERFLOWED: racing an admitted call (the drain
 * that exhausts runs inside the call's reserve callback) and inside the seal's own finalization ---- */
static void four_with_records(void)
{
    G.fail_every = 2; /* events 0 2 4 submitted, 1 and 3 lost: records [1, 1] and [3, 1] */
    for (int i = 0; i < 5; i++)
        WANT(M(0) == 0, "mark %d", i);
    G.fail_every = 0;
    gpu_step(100);
}
static void t_exhaust_admitted(void)
{
    FILE *e;
    fresh_led(16, led(2, 100, 100)); /* the terminal + ONE non-terminal LOSS */
    four_with_records();
    G.reserve_hook = drain_in_reserve; /* event 5 is admitted; the drain inside its reserve exhausts the budget */
    WANT(M(1) == 0, "the admitted call completes");
    G.reserve_hook = NULL;
    WANT(D.exhausted, "the stop is published");
    WANT(M(1) == PRG_ESTOPPED && prg_calibrate(&D, 10) == PRG_ESTOPPED, "calls after exhaustion are refused");
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 3 && D.events_accepted == 2 && D.lost_accepted == 4,
         "OVERFLOWED; events 0 2; LOSS [1, 1] + terminal [3, 3) (got state %u events %llu lost %llu)", D.final_state,
         (unsigned long long)D.events_accepted, (unsigned long long)D.lost_accepted);
    e = finish("r1-exhaust-admitted");
    fprintf(e, "exit 4\nevents 2\nlost 4\nloss 1 1 3\nloss 3 3 3\nloss-chunks 2\nexhausted 1\n");
    fclose(e);
}
static void t_exhaust_at_seal(void)
{
    FILE *e;
    fresh_led(16, led(2, 100, 100));
    four_with_records();
    prg_seal(&D); /* the finalization's own drain exhausts the budget */
    WANT(D.final_state == 3 && D.exhausted && D.events_accepted == 2 && D.lost_accepted == 3,
         "OVERFLOWED from inside the seal (got state %u)", D.final_state);
    WANT(M(1) == PRG_ESTOPPED, "refused");
    e = finish("r1-exhaust-at-seal");
    fprintf(e, "exit 4\nevents 2\nlost 3\nloss 1 1 3\nloss 3 2 3\nloss-chunks 2\nexhausted 1\n");
    fclose(e);
}

/* ---- 2. a backward L2 hidden by the issued cap: completed 0, published ticket 1, L1 = 2 (beyond issued: done1 = 1),
 * L2 = 1 (done2 = 1): the true distances 2 -> 1 are a backward step.  Plain and across the u32 label wrap ---- */
static void label_back_one(void)
{
    G.rtimer_hook = NULL;
    p_wlabel(NULL, 200, (uint32_t)(D.completed + 1));
}
static void drain_beyond_then_back(void)
{
    G.reserve_hook = NULL;
    gpu_step(2);                   /* both pairs: L1 = completed + 2, one past the published ticket */
    G.rtimer_hook = label_back_one; /* after the report read, the label steps back to completed + 1 */
    prg_drain(&D);
}
static void t_l2_back_beyond_issued(const char *name, uint64_t base)
{
    FILE *e;
    fresh(16);
    D.p_ticket = D.completed = D.drained = D.drained_pub = base; /* the first mark publishes the rebased ticket */
    p_wlabel(NULL, 200, (uint32_t)base);
    WANT(M(0) == 0, "%s: mark 0 (published)", name);
    G.reserve_hook = drain_beyond_then_back;
    WANT(M(1) == 0, "%s: mark 1, the drain runs inside its reserve", name);
    WANT(D.anomaly && D.drained == base && D.drained_pub == base && !D.events_accepted,
         "%s: the backward L2 is detected, nothing committed (anomaly %u drained %llu)", name, D.anomaly,
         (unsigned long long)D.drained);
    WANT(p_rtimer(NULL, 16 + (uint32_t)(base % 16)) != PRG_POISON, "%s: the slot was not poisoned", name);
    prg_seal(&D);
    WANT(D.final_state == 2 && D.lost_accepted == 2, "%s: ABANDONED, LOSS [0, 2)", name);
    e = finish(name);
    fprintf(e, "exit 4\nevents 0\nlost 2\nloss 0 2 4\n");
    fclose(e);
}

/* ---- 3. suppressed tokens carry a never-reused issuance serial ---- */
static void t_pair_suppressed_stale(void)
{
    FILE *e;
    prg_token t[64], a, b, x;
    fresh(256);
    for (int i = 0; i < 64; i++)
        WANT(B(0, &t[i]) == 0, "begin %d", i);
    WANT(B(5, &a) == 0 && E(&a, 5) == 0, "suppressed A at level 1, ended");
    WANT(B(5, &b) == 0, "suppressed B at level 1, same descriptor");
    WANT(a.depth == b.depth && a.desc == b.desc && a.begin_seq != b.begin_seq, "same level and desc, new serial");
    WANT(E(&a, 5) == PRG_EINVAL && D.p_suppressed == 1, "A's stale token cannot end B");
    WANT(E(&b, 5) == 0, "B's own token ends B");
    /* the bounded allocation: PRG_SUPPRESS_MAX levels, then refused */
    for (uint32_t i = 0; i < PRG_SUPPRESS_MAX; i++)
        WANT(B(7, &x) == 0, "suppressed level %u", i + 1);
    WANT(B(7, &a) == PRG_EOVERFLOW && D.st.refused_suppress_overflow == 1, "level %u refused", PRG_SUPPRESS_MAX + 1);
    for (uint32_t i = 0; i < PRG_SUPPRESS_MAX; i++) {
        prg_token y = {D.domain_id, 0x80000000u | D.p_suppressed, D.p_sup_stack[D.p_suppressed - 1], 7, 0};
        WANT(E(&y, 7) == 0, "unwind suppressed");
    }
    /* the serial counter exhausted: refused, nothing allocated */
    D.p_sup_serial = UINT64_MAX;
    WANT(B(7, &a) == PRG_EOVERFLOW && !D.p_suppressed && D.st.refused_suppress_overflow == 2,
         "the serial space exhausted: refused");
    for (int i = 63; i >= 0; i--)
        WANT(E(&t[i], 0) == 0, "end %d", i);
    gpu_step(1000);
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 128, "FINALIZED 128");
    e = finish("r1-pair-suppressed-stale");
    fprintf(e, "exit 0\nevents 128\nlost 0\n");
    fclose(e);
}

/* ================================================================ R2 core additions (review GO 12:53) */
static prg_dom D2_SCRATCH;

/* ---- the emittable HOLD: the prefix stops at a held event, nothing after it commits, then it resumes ---- */
static void t_hold_release(void)
{
    FILE *e;
    fresh(16);
    WANT(M(0) == 0 && M(3) == 0 && M(0) == 0, "marks 0 (desc 0), 1 (desc 3), 2 (desc 0)");
    gpu_step(100);
    g_hold_desc = 3;
    prg_drain(&D);
    WANT(D.events_accepted == 1 && D.drained == 1 && D.st.held == 1 && D.drained_pub == 1,
         "held at number 1: only number 0 committed, slots 1 and 2 still owned (drained %llu held %llu)",
         (unsigned long long)D.drained, (unsigned long long)D.st.held);
    WANT(p_rtimer(NULL, 17) != PRG_POISON, "the held slot is not poisoned");
    g_hold_desc = UINT32_MAX;
    prg_drain(&D);
    prg_seal(&D);
    WANT(D.final_state == 1 && D.events_accepted == 3, "released: FINALIZED 3");
    e = finish("r1-hold-release");
    fprintf(e, "exit 0\nevents 3\nlost 0\n");
    fclose(e);
}
static void t_hold_at_seal(void)
{
    FILE *e;
    fresh(16);
    WANT(M(0) == 0 && M(3) == 0 && M(0) == 0, "marks");
    gpu_step(100);
    g_hold_desc = 3;
    prg_drain(&D);
    WANT(prg_detach(&D, 50, NULL, NULL) == PRG_ETIMEDOUT, "a held prefix: detach times out, no early completion");
    prg_seal(&D);
    g_hold_desc = UINT32_MAX;
    WANT(D.final_state == 2 && D.events_accepted == 1 && D.lost_accepted == 2, "held at the seal: LOSS 4 + ABANDONED");
    WANT(prg_release(&D) == PRG_OK, "release after the app's quiescence");
    e = finish("r1-hold-at-seal");
    fprintf(e, "exit 4\nevents 1\nlost 2\nloss 1 2 4\n");
    fclose(e);
}

/* ---- the scratch capacity: GPU_EVENTS chunks split at scratch_events ---- */
static void t_scratch_split(void)
{
    FILE *e;
    int chunks = 0;
    for (int i = 0; i < g_nch; i++)
        free(g_ch[i].data);
    g_nch = 0;
    g_sink_dead = g_fail_type = g_fail_nth = g_seen_type = 0;
    g_ntr = 0;
    gpu_reset();
    g_led = LED_READER;
    WANT(prg_attach(&D, 7, 16, 16, 200, &g_port, &g_out, &g_led, g_meta, g_stage, g_scratch, 2) == PRG_OK,
         "attach with a 2-event scratch");
    WANT(prg_attach(&D2_SCRATCH, 8, 64, 16, 201, &g_port, &g_out, &g_led, g_meta, g_stage, g_scratch, 0) ==
             PRG_EINVAL &&
             prg_attach(&D2_SCRATCH, 8, 64, 16, 201, &g_port, &g_out, &g_led, g_meta, g_stage, g_scratch,
                        PRG_EVENTS_MAX + 1) == PRG_EINVAL,
         "scratch_events 0 and one past the chunk maximum are refused");
    for (int i = 0; i < 5; i++)
        WANT(M(0) == 0, "mark %d", i);
    gpu_step(100);
    prg_drain(&D);
    prg_seal(&D);
    for (int i = 0; i < g_nch; i++)
        chunks += g_ch[i].type == 14;
    WANT(chunks == 3 && D.events_accepted == 5, "5 events in chunks of 2, 2, 1 (got %d chunks)", chunks);
    e = finish("r1-scratch-split");
    fprintf(e, "exit 0\nevents 5\nlost 0\n");
    fclose(e);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    g_dir = argv[1];
    pthread_mutex_init(&G.m, NULL);
    t_steady();
    t_full_ring();
    t_fifo_refusal();
    t_sink_fail(14, "r1-sink-fail-events");
    t_sink_fail(16, "r1-sink-fail-final");
    t_sink_fail(15, "r1-sink-fail-trailing-loss");
    t_nested_suppression();
    t_end_lost();
    t_label_wrap();
    t_deferral();
    t_seal_pending();
    t_detach_retry();
    t_detach_then_shutdown();
    t_calibrate();
    t_overflow();
    t_intervals();
    t_second_label_advance();
    t_first_use_stale();
    t_seal_inflight();
    t_seal_calibrating();
    t_detach_vs_seal();
    t_detach_open_loss();
    t_label_beyond_issued();
    /* the poison/decrease at the 4th rejects the second drain's WHOLE interval (validate before commit) */
    t_fault(F_LABEL_FIRST, "r1-ctl-label-before-report", 1, 2);
    t_fault(F_DECREASE, "r1-ctl-decrease", 1, 2);
    t_fault(F_LABEL_BACK, "r1-ctl-label-backward", 1, 2);       /* the window refuses before any read */
    t_fault(F_LABEL_FUTURE, "r1-ctl-label-future", 1, 2);
    t_fault(F_TORN, "r1-cex-torn-read", 0, 4);
    t_fault(F_LATE_REPORT, "r1-cex-late-report-benign", 0, 4); /* A1 violated, report lands before the read */
    t_fault(F_STALE_MONO, "r1-cex-stale-monotone", 0, 4);
    /* the corrections */
    t_lockfault("r1-fault-producer-slock", S_PRODUCER, LK_SLOCK, 0, PRG_EFAULT, 2, 0);
    t_lockfault("r1-fault-producer-sunlock", S_PRODUCER, LK_SUNLOCK, 0, PRG_EFAULT, 2, 0);
    t_lockfault("r1-fault-cal-slock", S_CALIBRATE, LK_SLOCK, 0, PRG_EFAULT, 2, 0);
    t_lockfault("r1-fault-cal-bracket-sunlock", S_CALIBRATE, LK_SUNLOCK, 1, PRG_EFAULT, 2, 0);
    t_lockfault("r1-fault-drain-elock", S_DRAIN, LK_ELOCK, 0, 0, 2, 0);
    t_lockfault("r1-fault-drain-slock", S_DRAIN, LK_SLOCK, 0, 0, 2, 0);
    t_lockfault("r1-fault-drain-sunlock", S_DRAIN, LK_SUNLOCK, 0, 0, 2, 0);
    t_lockfault("r1-fault-drain-eunlock", S_DRAIN, LK_EUNLOCK, 0, 0, 3, 0);
    t_lockfault("r1-fault-seal-elock", S_SEAL, LK_ELOCK, 0, 0, 2, 0);
    t_lockfault("r1-fault-seal-slock", S_SEAL, LK_SLOCK, 0, 0, 2, 0);
    t_lockfault("r1-fault-seal-sunlock", S_SEAL, LK_SUNLOCK, 0, 0, 2, 0);
    t_lockfault("r1-fault-seal-eunlock", S_SEAL, LK_EUNLOCK, 0, 0, 3, 1);
    t_lockfault("r1-fault-detach-hook-eunlock", S_DETACH, LK_EUNLOCK, 0, PRG_EFAULT, 3, 0);
    t_lockfault("r1-fault-detach-poll-slock", S_DETACH, LK_SLOCK, 1, PRG_EFAULT, 3, 0);
    t_lockfault("r1-fault-detach-poll-sunlock", S_DETACH, LK_SUNLOCK, 1, PRG_EFAULT, 3, 0);
    t_lockfault("r1-fault-detach-elock", S_DETACH, LK_ELOCK, 1, PRG_EFAULT, 3, 0);
    t_lockfault("r1-fault-detach-final-slock", S_DETACH, LK_SLOCK, 2, PRG_EFAULT, 3, 0);
    t_lockfault("r1-fault-detach-final-sunlock", S_DETACH, LK_SUNLOCK, 2, PRG_EFAULT, 3, 0);
    t_lockfault("r1-fault-detach-eunlock", S_DETACH, LK_EUNLOCK, 1, PRG_EFAULT, 3, 1);
    t_l2_reject_flush();
    t_small_ring_refill();
    t_cal_bad_second_label();
    t_cal_poison();
    t_cal_clock_fail();
    t_cal_tb_order();
    t_cal_seal_during();
    t_cal_frontier_frozen();
    t_pair_desc();
    t_mark_depth64();
    t_pair_suppressed();
    t_loss_budget_exact();
    t_loss_budget_over();
    t_attach_bounds();
    t_cal_budget("r1-cal-epoch-exact", led(100, 2, 100), 2, 2);
    t_cal_budget("r1-cal-epoch-over", led(100, 2, 100), 3, 2);
    t_cal_budget("r1-cal-file-exact", led(100, 100, 2), 2, 2);
    t_cal_budget("r1-cal-file-over", led(100, 100, 2), 3, 2);
    /* the r1c3 carries */
    t_exhaust_admitted();
    t_exhaust_at_seal();
    t_l2_back_beyond_issued("r1-l2-back-beyond-issued", 0);
    t_l2_back_beyond_issued("r1-l2-back-beyond-issued-wrap", 0xFFFFFFFFull); /* L1 = 1, L2 = 0 across the wrap */
    t_pair_suppressed_stale();
    /* R2 core additions */
    t_hold_release();
    t_hold_at_seal();
    t_scratch_split();
    printf("R1 traces %d, failures %d\n", g_traces, g_bad);
    return g_bad ? 1 : 0;
}
