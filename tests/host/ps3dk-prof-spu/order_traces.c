/* Deterministic traces of the SPU_ORDER records (spu-ticks slice design r1 10.1, delta D3.7: controls C3-1 .. C3-7
 * and the runtime mutants).  Each trace writes <name>.prof, .truth and .expect like traces.c; a trace with a
 * reader-side state expectation also writes <name>.ticks-expect (lines "has <text>" / "lacks <text>" over the output
 * of `spu-ticks --observations`).  Reader fixtures of C3-5 / C3-6 are the accepted chunks of a trace with one change,
 * framed by the same writer.  Model evidence only. */
#include <stdlib.h>
#include <string.h>

#include "model.h"

void order_traces(const char *dir, int *ntraces);

static const char *g_odir;
static int *g_ont;

static void owant(const char *trace, const char *what, long long got, long long exp)
{
    if (got != exp)
        FAIL("%s: %s = %lld, want %lld", trace, what, got, exp);
}

static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }
static void w32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void w64(uint8_t *p, uint64_t v)
{
    w32(p, (uint32_t)v);
    w32(p + 4, (uint32_t)(v >> 32));
}

/* ---- SPU-side wrappers: each notes the CAL a call published (producer ground truth for events_before) ---- */

static int of(lane_h *h)
{
    uint16_t a = h->s.cal_answered;
    int rc = psp_spu_flush(&h->s);
    call_ok(h, "flush");
    if (rc == PSP_OK)
        cal_note(h, a);
    return rc;
}

static int ofini(lane_h *h)
{
    uint16_t a = h->s.cal_answered;
    int rc = psp_spu_fini(&h->s);
    call_ok(h, "fini");
    if (rc == PSP_OK)
        cal_note(h, a);
    return rc;
}

static int odecw(lane_h *h, uint32_t v)
{
    uint16_t a = h->s.cal_answered;
    int rc = psp_spu_dec_write(&h->s, v);
    call_ok(h, "dec_write");
    if (rc == PSP_OK)
        cal_note(h, a);
    return rc;
}

static void ozones(lane_h *h, int k)
{
    for (int i = 0; i < k; i++) {
        zb(h, 0);
        ze(h, 0);
    }
}

static void odrain(run *r)
{
    psp_drain(&r->p);
    order_ledger_ok(r, "drain");
}

static void oinit(run *r, uint32_t ring, uint32_t lanes_max, const psp_limits *lim)
{
    g_order_next = 1;
    run_init(r, ring, lanes_max, lim);
    order_ledger_ok(r, "init");
}

static void oend(run *r, lane_h *h)
{
    owant("end", "lane_end", psp_lane_end(&r->p, h->idx), PSP_P_OK);
    order_ledger_ok(r, "end");
}

/* seal, the stream check, the order check (its class mask must be `want`), the capture and its expectation */
static FILE *odone(run *r, const char *name, int flags, unsigned want_mask)
{
    unsigned m;
    psp_seal(&r->p);
    order_ledger_ok(r, "seal");
    check_streams(r, name);
    m = check_order(r, name, flags);
    if (m != want_mask)
        FAIL("%s: order check classes %#x, want %#x", name, m, want_mask);
    write_capture(r, g_odir, name);
    (*g_ont)++;
    return expect_open(g_odir, name);
}

static FILE *ticks_open(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ticks-expect", g_odir, name);
    return fopen(path, "w");
}

/* ---- accepted-chunk copies for the reader fixtures ---- */

typedef struct chv {
    chunk_rec *c;
    size_t n;
} chv;

static chv chv_of(const run *r)
{
    chv v = {malloc(sizeof(chunk_rec) * (r->nch + 4)), r->nch};
    for (size_t i = 0; i < r->nch; i++) {
        v.c[i] = r->ch[i];
        v.c[i].data = malloc(r->ch[i].len ? r->ch[i].len : 1);
        memcpy(v.c[i].data, r->ch[i].data, r->ch[i].len);
    }
    return v;
}

static void chv_free(chv *v)
{
    for (size_t i = 0; i < v->n; i++)
        free(v->c[i].data);
    free(v->c);
}

static void chv_del(chv *v, size_t i)
{
    free(v->c[i].data);
    memmove(&v->c[i], &v->c[i + 1], sizeof *v->c * (v->n - i - 1));
    v->n--;
}

/* insert a copy of chunk `src` (an index into v) at position `at` (capacity: 4 extra) */
static void chv_dup(chv *v, size_t src, size_t at)
{
    chunk_rec c = v->c[src];
    c.data = malloc(c.len);
    memcpy(c.data, v->c[src].data, c.len);
    memmove(&v->c[at + 1], &v->c[at], sizeof *v->c * (v->n - at));
    v->c[at] = c;
    v->n++;
}

/* write the chunks of `v` as the capture `name` (torn: no TRAILER) with the run's truth, and its expectation */
static void write_fixture(run *r, chv *v, const char *name, int torn, const char *expect)
{
    chunk_rec *ch = r->ch;
    size_t nch = r->nch;
    int sf = r->p.sink_failed;
    FILE *e;
    r->ch = v->c;
    r->nch = v->n;
    r->p.sink_failed = torn;
    write_capture(r, g_odir, name);
    r->ch = ch;
    r->nch = nch;
    r->p.sink_failed = sf;
    e = expect_open(g_odir, name);
    fputs(expect, e);
    fclose(e);
    (*g_ont)++;
}

/* index of the k-th (0-based) SPU_ORDER chunk whose first record has `kind`; -1 if none */
static long find_ord(const chv *v, uint32_t kind, int k)
{
    for (size_t i = 0; i < v->n; i++)
        if (v->c[i].type == 18 && r32(v->c[i].data + 8) == kind && k-- == 0)
            return (long)i;
    return -1;
}

static long find_type(const chv *v, uint32_t type, int k)
{
    for (size_t i = 0; i < v->n; i++)
        if (v->c[i].type == type && k-- == 0)
            return (long)i;
    return -1;
}

/* the first CALIBRATION bracket's tb0/tb1 of a domain and epoch, from the accepted chunks */
static int bracket_tbs(const run *r, uint32_t domain, uint32_t epoch, uint64_t seq, uint64_t *tb0, uint64_t *tb1)
{
    for (size_t i = 0; i < r->nch; i++) {
        const uint8_t *d = r->ch[i].data;
        if (r->ch[i].type != 10 || r32(d) != domain || r32(d + 4) != epoch)
            continue;
        for (uint32_t k = 0; k < r32(d + 16); k++)
            if (r64(d + 8) + k == seq) {
                *tb0 = r64(d + 20 + 24 * (size_t)k + 8);
                *tb1 = r64(d + 20 + 24 * (size_t)k + 16);
                return 1;
            }
    }
    return 0;
}

/* tb_copy of the k-th accepted DRAINED + DRAIN_TB pair of a lane */
static uint64_t pair_tb(const run *r, uint32_t lane, int k)
{
    for (size_t i = 0; i < r->nch; i++) {
        const uint8_t *d = r->ch[i].data;
        if (r->ch[i].type == 18 && r32(d) == 2 && r32(d + 12) == lane && k-- == 0)
            return r64(d + 32 + 8);
    }
    return UINT64_MAX;
}

static uint64_t created_tb(const run *r, uint32_t lane)
{
    for (size_t i = 0; i < r->nch; i++) {
        const uint8_t *d = r->ch[i].data;
        if (r->ch[i].type == 18 && r32(d) == 1 && r32(d + 8) == 4 && r32(d + 12) == lane)
            return r64(d + 16);
    }
    return UINT64_MAX;
}

static void obs_line(FILE *t, lane_h *h, uint64_t seq, uint64_t L, const char *u1, uint64_t u2, uint64_t u)
{
    fprintf(t, "has event seq %llu raw %u | L %llu U1 %s U2 %llu U %llu\n", (unsigned long long)seq,
            h->truth[seq - h->tbase].dec, (unsigned long long)L, u1, (unsigned long long)u2, (unsigned long long)u);
}

/* ---- the clean order run: two walks with events and brackets, one bracket-only walk at the end ---- */

static void clean_run(run *r, lane_h **hp, int mutant)
{
    lane_h *h;
    oinit(r, 16, 4, NULL);
    r->p.test_order_mutant = mutant;
    h = lane_new(r, 8, 1, 1);
    odrain(r);    /* LANE_CREATED, EPOCH 1, request 1 */
    ozones(h, 3); /* seq 0..5 */
    owant("clean", "flush", of(h), PSP_OK);
    odrain(r);    /* walk 1: events + CAL 1, BRACKET_AT, pair; request 2 */
    ozones(h, 2); /* seq 6..9 */
    of(h);
    odrain(r);    /* walk 2 */
    ofini(h);     /* CAL 3 */
    oend(r, h);   /* the final cycle: a bracket-only walk, ORDER_FINAL, DOMAIN_FINAL */
    *hp = h;
}

static void t_o_clean(void)
{
    run r;
    lane_h *h;
    FILE *e, *t;
    uint64_t em, dr, tb0, tb1;
    chv v;
    long i, j;
    char ex[256];
    clean_run(&r, &h, PSP_OM_NONE);
    e = odone(&r, "t-o-clean", CK_RUN, 0);
    fprintf(e, "exit 0\nverdict complete\nlane %u COMPLETE 1 1 10 0\n", h->lane_id);
    fclose(e);
    owant("o-clean", "ORDER_FINAL present", order_final(&r, h->lane_id, &em, &dr), 1);
    owant("o-clean", "records_emitted (kinds 1-4, not itself)", (long long)em,
          (long long)(order_count(&r, h->lane_id, 1) + order_count(&r, h->lane_id, 2) +
                      order_count(&r, h->lane_id, 3) + order_count(&r, h->lane_id, 4)));
    owant("o-clean", "records_emitted", (long long)em, 1 + 3 + 4);
    owant("o-clean", "records_dropped", (long long)dr, 0);
    owant("o-clean", "LANE_CREATED", (long long)order_count(&r, h->lane_id, 4), 1);
    owant("o-clean", "ledger emitted", (long long)r.p.order_emitted, 9);
    owant("o-clean", "ledger reserved (lanes_life 1024 minus this lane)", (long long)r.p.order_reserved, 2 * 1024 - 2);
    /* DRAIN_TB = the walk's tb1 = the tb1 of the bracket whose CAL the walk loaded */
    bracket_tbs(&r, h->domain_id, 1, 0, &tb0, &tb1);
    owant("o-clean", "walk 1 DRAIN_TB = bracket 0 tb1", (long long)pair_tb(&r, h->lane_id, 0), (long long)tb1);
    bracket_tbs(&r, h->domain_id, 1, 1, &tb0, &tb1);
    owant("o-clean", "walk 2 DRAIN_TB = bracket 1 tb1", (long long)pair_tb(&r, h->lane_id, 1), (long long)tb1);
    t = ticks_open("t-o-clean");
    fprintf(t, "has SPU lane %u (domain %u)\nlacks ORDER COVERAGE UNKNOWN\nlacks TICKS-UNAVAILABLE(order coverage)\n",
            h->lane_id, h->domain_id);
    fclose(t);

    /* C3-5: a decreasing tb_copy is an ORDER CONTRADICTION finding, not MALFORMED */
    v = chv_of(&r);
    i = find_ord(&v, 2, 1);
    w64(v.c[i].data + 32 + 8, pair_tb(&r, h->lane_id, 0) - 1);
    write_fixture(&r, &v, "t-o-c35-tb-decrease", 0, "exit 0\nverdict complete\n");
    chv_free(&v);
    t = ticks_open("t-o-c35-tb-decrease");
    fprintf(t, "has ORDER CONTRADICTION\n");
    fclose(t);

    /* C3-6 (a) DOMAIN_FINAL present, ORDER_FINAL absent */
    v = chv_of(&r);
    chv_del(&v, (size_t)find_ord(&v, 5, 0));
    write_fixture(&r, &v, "t-o-c36a-no-order-final", 0, "exit 2\nverdict malformed DOMAIN_FINAL without its ORDER_FINAL\n");
    chv_free(&v);
    /* (b) two ORDER_FINALs */
    v = chv_of(&r);
    i = find_ord(&v, 5, 0);
    chv_dup(&v, (size_t)i, (size_t)i + 1);
    write_fixture(&r, &v, "t-o-c36b-two-finals", 0, "exit 2\nverdict malformed an order record after its ORDER_FINAL\n");
    chv_free(&v);
    /* (c) ORDER_FINAL after DOMAIN_FINAL: moved after it, and a second copy after it */
    v = chv_of(&r);
    i = find_ord(&v, 5, 0);
    j = find_type(&v, 16, 0);
    chv_dup(&v, (size_t)i, (size_t)j + 1);
    chv_del(&v, (size_t)i);
    write_fixture(&r, &v, "t-o-c36c-final-after-domain-final", 0,
                  "exit 2\nverdict malformed DOMAIN_FINAL without its ORDER_FINAL\n");
    chv_free(&v);
    v = chv_of(&r);
    i = find_ord(&v, 5, 0);
    j = find_type(&v, 16, 0);
    chv_dup(&v, (size_t)i, (size_t)j + 1);
    write_fixture(&r, &v, "t-o-c36c2-copy-after-domain-final", 0,
                  "exit 2\nverdict malformed data after its DOMAIN_FINAL\n");
    chv_free(&v);
    /* (d) an order record of the lane after its ORDER_FINAL */
    v = chv_of(&r);
    i = find_ord(&v, 1, 0);
    chv_dup(&v, (size_t)i, (size_t)find_ord(&v, 5, 0) + 1);
    write_fixture(&r, &v, "t-o-c36d-record-after-final", 0,
                  "exit 2\nverdict malformed an order record after its ORDER_FINAL\n");
    chv_free(&v);
    /* (e) records_emitted one more, one less */
    for (int d = -1; d <= 1; d += 2) {
        v = chv_of(&r);
        i = find_ord(&v, 5, 0);
        w64(v.c[i].data + 16, em + (uint64_t)(long long)d);
        write_fixture(&r, &v, d > 0 ? "t-o-c36e-emitted-plus1" : "t-o-c36e-emitted-minus1", 0,
                      "exit 2\nverdict malformed records_emitted\n");
        chv_free(&v);
    }
    /* (f) ORDER_FINAL naming an undeclared lane */
    v = chv_of(&r);
    i = find_ord(&v, 5, 0);
    w32(v.c[i].data + 12, 99);
    write_fixture(&r, &v, "t-o-c36f-undeclared", 0, "exit 2\nverdict malformed SPU_ORDER names undeclared lane 99\n");
    chv_free(&v);
    /* (g) the file ends after the last pair: both finals torn off */
    v = chv_of(&r);
    i = find_ord(&v, 2, 1);
    while (v.n > (size_t)i + 1)
        chv_del(&v, v.n - 1);
    snprintf(ex, sizeof ex, "exit 1\nverdict incomplete\ntruth %u\n", h->lane_id);
    write_fixture(&r, &v, "t-o-c36g-torn-after-pair", 1, ex);
    chv_free(&v);
    t = ticks_open("t-o-c36g-torn-after-pair");
    fprintf(t, "has ORDER COVERAGE UNKNOWN\nhas NOT DETERMINED: \nhas lanes without ORDER_FINAL 1\nlacks SPU TICKS[\n");
    fclose(t);
    /* (h) records_dropped = 1: every segment of the lane fails T-ORDER */
    v = chv_of(&r);
    i = find_ord(&v, 5, 0);
    w64(v.c[i].data + 24, 1);
    write_fixture(&r, &v, "t-o-c36h-dropped-1", 0, "exit 0\nverdict complete\n");
    chv_free(&v);
    t = ticks_open("t-o-c36h-dropped-1");
    fprintf(t, "has TICKS-UNAVAILABLE(order coverage)\nlacks SPU TICKS[\n");
    fclose(t);
    run_free(&r);

    /* C3-5 runtime mutant: the walk-end pair decided before close_events (a stale cursor) */
    clean_run(&r, &h, PSP_OM_STALE_CURSOR);
    e = odone(&r, "t-o-c35-m-stale-cursor", CK_RUN, OC_CURSOR);
    fprintf(e, "exit 2\nverdict malformed the cursor is\n");
    fclose(e);
    run_free(&r);
}

/* ---- C3-1: the atomic pair with an odd remaining budget ---- */

static void c31(int mutant)
{
    run r;
    lane_h *h;
    FILE *e, *t;
    psp_limits lim = psp_reader_limits();
    uint64_t em, dr, tbA, tb0, tb1, tc;
    const char *name = mutant ? "t-o-c31-m-one-at-a-time" : "t-o-c31";
    lim.lanes_life = 1;
    lim.order_records = 5; /* 2 reserved, 3 for walks */
    lim.brackets_epoch = 1; /* one bracket: no request after walk C */
    oinit(&r, 8, 4, &lim);
    r.p.test_order_mutant = mutant ? PSP_OM_ONE_AT_A_TIME : PSP_OM_NONE;
    owant(name, "reserved at init", (long long)r.p.order_reserved, 2);
    h = lane_new(&r, 8, 0, 1);
    ozones(h, 2); /* seq 0..3 */
    of(h);
    odrain(&r); /* walk A: LANE_CREATED, EPOCH 1, the pair admitted (1 left); request 1 */
    owant(name, "after walk A emitted", (long long)r.p.order_emitted, 3);
    ozones(h, 4); /* seq 4..11: fills the ring, so the CAL does not fit (a dropped CAL) */
    of(h);
    owant(name, "walk B publication has no CAL", h->s.cal_answered, 0);
    odrain(&r); /* walk B: events only, 1 left: the pair is refused */
    owant(name, "after walk B emitted", (long long)r.p.order_emitted, mutant ? 4 : 3);
    owant(name, "after walk B CALIBRATION chunks", (long long)order_count(&r, h->lane_id, 1), 0);
    ozones(h, 1); /* seq 12..13 */
    of(h);
    odrain(&r); /* walk C: events + CAL: BRACKET_AT admitted (0 left), the pair refused */
    ofini(h);
    oend(&r, h);
    if (mutant) {
        e = odone(&r, name, 0, OC_LONE);
        fprintf(e, "exit 2\nverdict malformed without its partner\n");
        fclose(e);
        run_free(&r);
        return;
    }
    e = odone(&r, name, 0, 0);
    fprintf(e, "exit 0\nverdict complete\nlane %u COMPLETE 1 1 14 0\n", h->lane_id);
    fclose(e);
    owant(name, "ORDER_FINAL", order_final(&r, h->lane_id, &em, &dr), 1);
    owant(name, "records_emitted (LANE_CREATED, the pair, BRACKET_AT; not itself)", (long long)em, 4);
    owant(name, "records_dropped", (long long)dr, 4);
    owant(name, "BRACKET_AT", (long long)order_count(&r, h->lane_id, 1), 1);
    owant(name, "DRAINED", (long long)order_count(&r, h->lane_id, 2), 1);
    owant(name, "DRAIN_TB", (long long)order_count(&r, h->lane_id, 3), 1);
    owant(name, "ledger emitted at the end", (long long)r.p.order_emitted, 5);
    owant(name, "ledger reserved at the end", (long long)r.p.order_reserved, 0);
    tbA = pair_tb(&r, h->lane_id, 0);
    tc = created_tb(&r, h->lane_id);
    bracket_tbs(&r, h->domain_id, 1, 0, &tb0, &tb1);
    t = ticks_open(name);
    fprintf(t, "has TICKS-UNAVAILABLE(order coverage)\nlacks ORDER COVERAGE UNKNOWN\n");
    {
        char ua[32];
        snprintf(ua, sizeof ua, "%llu", (unsigned long long)tbA);
        for (uint64_t s = 0; s < 4; s++) /* walk A: Upper-1 = walk A's tb; Upper-2 = walk C's bracket */
            obs_line(t, h, s, tc, ua, tb1, tbA < tb1 ? tbA : tb1);
    }
    for (uint64_t s = 4; s < 14; s++) /* walks B and C: no DRAIN_TB after them; Upper-2 = walk C's bracket tb1 */
        obs_line(t, h, s, tc, "none", tb1, tb1);
    fclose(t);
    run_free(&r);
}

/* ---- C3-2: the reservation ---- */

static uint32_t g_uncovered = 7;
static int covered_but(void *ctx, uint32_t inc, uint32_t slot, uint32_t gen)
{
    (void)ctx;
    (void)slot;
    (void)gen;
    return inc != g_uncovered;
}

static void t_o_c32(void)
{
    run r;
    lane_h *a, *b, *c;
    FILE *e;
    psp_limits lim = psp_reader_limits();
    static psp_ppu tmp;
    uint32_t never;
    uint64_t em, dr, res, d0;
    lim.lanes_life = 4;
    lim.order_records = 2 * 4 + 2; /* 2 for walks */
    lim.brackets_epoch = 0;        /* no request: every walk record is a pair */
    oinit(&r, 8, 4, &lim);
    {
        psp_storage st = {r.st_scratch, PSP_SCRATCH_BYTES, r.st_copy, 8 * PSP_SLOT, r.st_arena, 1u << 20};
        psp_limits l7 = lim;
        l7.order_records = 2 * 4 - 1;
        owant("o-c32", "init", psp_ppu_init(&tmp, &r.p.cfg, &l7, &r.m, &r.o, &st), PSP_P_OK);
        owant("o-c32", "order with order_records = 2 lanes_life - 1", psp_ppu_order(&tmp), PSP_P_EINVAL);
        owant("o-c32", "no reservation after the refusal", (long long)tmp.order_reserved, 0);
        psp_ppu_fini(&tmp);
        l7.order_records = 2 * 4;
        owant("o-c32", "init", psp_ppu_init(&tmp, &r.p.cfg, &l7, &r.m, &r.o, &st), PSP_P_OK);
        owant("o-c32", "order with order_records = 2 lanes_life", psp_ppu_order(&tmp), PSP_P_OK);
        owant("o-c32", "reserved", (long long)tmp.order_reserved, 8);
        owant("o-c32", "order twice", psp_ppu_order(&tmp), PSP_P_EINVAL);
        psp_ppu_fini(&tmp);
    }
    r.p.covered = covered_but;
    a = lane_new(&r, 8, 0, 1);
    b = lane_new(&r, 8, 0, 1);
    c = lane_new(&r, 8, 0, 1);
    owant("o-c32", "never-declared create", psp_lane_create(&r.p, "never", r.image, g_uncovered, 1, 1, &never),
          PSP_P_OK);
    ozones(a, 1);
    of(a);
    odrain(&r); /* LANE_CREATED x3, a's pair: the walk budget is spent */
    owant("o-c32", "a's pair admitted", (long long)order_count(&r, a->lane_id, 2), 1);
    ozones(b, 1);
    of(b);
    odrain(&r);
    owant("o-c32", "b's pair refused", (long long)order_count(&r, b->lane_id, 2), 0);
    res = r.p.order_reserved;
    owant("o-c32", "the never-declared lane ends", psp_lane_end(&r.p, never), PSP_P_OK);
    owant("o-c32", "its two reservations released", (long long)(res - r.p.order_reserved), 2);
    ozones(c, 1);
    of(c);
    d0 = order_count(&r, c->lane_id, 2);
    odrain(&r);
    owant("o-c32", "c's pair admitted from the released reservations", (long long)(order_count(&r, c->lane_id, 2) - d0), 1);
    ofini(a);
    ofini(b);
    ofini(c);
    oend(&r, a);
    oend(&r, b);
    oend(&r, c);
    e = odone(&r, "t-o-c32", 0, 0);
    fprintf(e, "exit 0\nverdict complete\nsources 3\n");
    fclose(e);
    for (int k = 0; k < 3; k++) {
        lane_h *h = k == 0 ? a : k == 1 ? b : c;
        owant("o-c32", "LANE_CREATED", (long long)order_count(&r, h->lane_id, 4), 1);
        owant("o-c32", "ORDER_FINAL", order_final(&r, h->lane_id, &em, &dr), 1);
        owant("o-c32", "records_emitted excludes ORDER_FINAL", (long long)em,
              (long long)(order_count(&r, h->lane_id, 2) * 2 + order_count(&r, h->lane_id, 4)));
        owant("o-c32", "records_dropped", (long long)dr, h == b ? 2 : 0);
    }
    owant("o-c32", "no record for the never-declared lane", (long long)order_count(&r, 4, 4) + (long long)order_count(&r, 4, 5), 0);
    owant("o-c32", "ledger reserved at the end", (long long)r.p.order_reserved, 0);
    run_free(&r);
}

/* ---- LANE_CREATED: none for a failed create ---- */

static void t_o_create(void)
{
    run r;
    lane_h *h;
    FILE *e;
    psp_limits lim = psp_reader_limits();
    uint32_t x;
    uint64_t tb, em, rs, made;
    lim.lanes_live = 1;
    oinit(&r, 8, 4, &lim);
    h = lane_new(&r, 8, 0, 1);
    tb = g_tb;
    em = r.p.order_emitted;
    rs = r.p.order_reserved;
    made = r.p.lanes_created;
    owant("o-create", "the second create fails", psp_lane_create(&r.p, "second", r.image, 0, 0, 0, &x), PSP_P_ENOSPC);
    owant("o-create", "no tb read by the failed create", (long long)g_tb, (long long)tb);
    owant("o-create", "no order record consumed", (long long)(r.p.order_emitted + r.p.order_reserved), (long long)(em + rs));
    owant("o-create", "no lane id consumed", (long long)r.p.lanes_created, (long long)made);
    ozones(h, 1);
    of(h);
    odrain(&r);
    ofini(h);
    oend(&r, h);
    e = odone(&r, "t-o-create", CK_RUN, 0);
    fprintf(e, "exit 0\nverdict complete\nsources 1\n");
    fclose(e);
    owant("o-create", "LANE_CREATED", (long long)order_count(&r, h->lane_id, 4), 1);
    owant("o-create", "LANE_CREATED tb", (long long)created_tb(&r, h->lane_id) > 0, 1);
    run_free(&r);
}

/* ---- C3-3: the sink-failure sweep over every chunk of a two-lane, two-epoch run ---- */

static void c33_run(run *r, lane_h **ha, lane_h **hb, long fail_at)
{
    lane_h *h[2];
    oinit(r, 16, 4, NULL);
    r->fail_sink_at = fail_at;
    h[0] = lane_new(r, 8, 1, 1);
    h[1] = lane_new(r, 8, 1, 1);
    odrain(r);
    for (int k = 0; k < 2; k++) {
        ozones(h[k], 2);
        of(h[k]);
    }
    odrain(r);
    for (int k = 0; k < 2; k++) {
        odecw(h[k], 0xF0000000u - 0x1000u * (uint32_t)k);
        ozones(h[k], 1 + k);
        of(h[k]);
    }
    odrain(r);
    for (int k = 0; k < 2; k++)
        ofini(h[k]);
    for (int k = 0; k < 2; k++)
        psp_lane_end(&r->p, h[k]->idx);
    order_ledger_ok(r, "c33 end");
    *ha = h[0];
    *hb = h[1];
}

static void t_o_c33(void)
{
    run base, r;
    lane_h *a, *b, *h[2];
    FILE *e, *t;
    size_t n;
    chunk_rec *bc;
    c33_run(&base, &a, &b, -1);
    e = odone(&base, "t-o-c33-full", CK_RUN, 0);
    fprintf(e, "exit 0\nverdict complete\nsources 2\ntruth %u\ntruth %u\n", a->lane_id, b->lane_id);
    fclose(e);
    n = base.nch;
    bc = base.ch;
    for (size_t k = 0; k < n; k++) {
        char name[64];
        uint64_t em, dr;
        int nofinal = 0, unknown_ok = 1;
        snprintf(name, sizeof name, "t-o-c33-k%02zu", k);
        c33_run(&r, &h[0], &h[1], (long)k);
        e = odone(&r, name, CK_RUN, 0);
        fprintf(e, "exit 1\nverdict incomplete\n");
        for (size_t i = 0; i < r.nch; i++)
            if (r.ch[i].type == 8) { /* the lanes are sources from their DOMAIN entries on (bound or not) */
                fprintf(e, "truth %u\ntruth %u\n", h[0]->lane_id, h[1]->lane_id);
                break;
            }
        fclose(e);
        if (r.nch != k)
            FAIL("%s: %zu accepted chunks, want %zu", name, r.nch, k);
        for (int j = 0; j < 2; j++) {
            uint64_t nx, ev, lo;
            int fin, declared = 0;
            accepted_counts(&r, h[j]->lane_id, &nx, &ev, &lo, &fin);
            /* the refused chunk was this lane's ORDER_FINAL: no DOMAIN_FINAL may follow */
            if (bc[k].type == 18 && r32(bc[k].data) == 1 && r32(bc[k].data + 8) == 5 &&
                r32(bc[k].data + 12) == h[j]->lane_id && fin)
                FAIL("%s lane %u: a DOMAIN_FINAL after its refused ORDER_FINAL", name, h[j]->lane_id);
            for (size_t i = 0; i < r.nch; i++)
                if (r.ch[i].type == 12)
                    declared = 1; /* both lanes are declared by the one SPU_LANES chunk */
            if (!declared)
                unknown_ok = 0; /* domains without their SPU_LANES entry: torn sources the reader lists too */
            if (declared && !order_final(&r, h[j]->lane_id, &em, &dr)) {
                nofinal++;
                if (order_count(&r, h[j]->lane_id, 4) + order_count(&r, h[j]->lane_id, 2) +
                        order_count(&r, h[j]->lane_id, 1) == 0)
                    unknown_ok = 0; /* a lane without any order record: the count's population is the reader's */
            }
        }
        t = ticks_open(name);
        fprintf(t, "has SPU ticks coverage\nlacks SPU TICKS[\n");
        if (unknown_ok)
            fprintf(t, "has lanes without ORDER_FINAL %d\n", nofinal);
        if (nofinal && unknown_ok)
            fprintf(t, "has ORDER COVERAGE UNKNOWN\nhas NOT DETERMINED: \n");
        fclose(t);
        run_free(&r);
    }
    run_free(&base);
}

/* ---- C3-4: one walk across EPOCH slots, its variants and mutants ---- */

static void c34_run(run *r, lane_h **hp, int mutant)
{
    lane_h *h;
    oinit(r, 64, 4, NULL);
    r->p.test_order_mutant = mutant;
    h = lane_new(r, 16, 1, 1);
    odrain(r); /* EPOCH 1, request 1 */
    zb(h, 0);
    ze(h, 0);
    zm(h, 1); /* seq 0..2 */
    of(h);    /* events + the CAL of the epoch-1 request */
    odecw(h, 0xE0000000u); /* EPOCH 2 */
    zb(h, 1);
    ze(h, 1); /* seq 3..4 */
    odecw(h, 0xD0000000u); /* EPOCH 3 */
    odecw(h, 0xC0000000u); /* EPOCH 4 */
    zm(h, 2);              /* seq 5 */
    of(h);
    odrain(r); /* the one walk */
    of(h);     /* variant: a CAL-only publication (request 2) */
    odrain(r); /* a walk that loads only CAL and PAD: no pair */
    ofini(h);
    oend(r, h);
    *hp = h;
}

static void t_o_c34(void)
{
    run r;
    lane_h *h;
    FILE *e, *t;
    chv v;
    long i;
    uint64_t tb0, tb1, tbB0, tbB1, tc;
    size_t at;
    c34_run(&r, &h, PSP_OM_NONE);
    e = odone(&r, "t-o-c34", CK_RUN, 0);
    fprintf(e, "exit 0\nverdict complete\nlane %u COMPLETE 1 1 6 0\nepochs 4\n", h->lane_id);
    fclose(e);
    /* the exact file order of the walk */
    {
        static const uint32_t want_t[] = {13, 10, 18, 18, 9, 13, 18, 9, 9, 13, 18};
        for (at = 0; at < r.nch && !(r.ch[at].type == 13); at++)
            ;
        for (size_t k = 0; k < sizeof want_t / sizeof *want_t; k++)
            if (at + k >= r.nch || r.ch[at + k].type != want_t[k])
                FAIL("o-c34: chunk %zu after the walk start has type %u, want %u", k,
                     at + k < r.nch ? r.ch[at + k].type : 0, want_t[k]);
    }
    bracket_tbs(&r, h->domain_id, 1, 0, &tb0, &tb1);
    {
        const uint8_t *d;
        d = r.ch[at].data;
        owant("o-c34", "events 1 first", (long long)r64(d + 8), 0);
        owant("o-c34", "events 1 n", r32(d + 16), 3);
        d = r.ch[at + 2].data;
        owant("o-c34", "BRACKET_AT kind", r32(d + 8), 1);
        owant("o-c34", "BRACKET_AT epoch", (long long)(uint32_t)r64(d + 16), 1);
        owant("o-c34", "BRACKET_AT events_before", (long long)r64(d + 24), 3);
        for (int k = 0; k < 3; k++) {
            static const size_t off[3] = {3, 6, 10};
            static const uint32_t ep[3] = {1, 2, 4};
            static const uint64_t th[3] = {3, 5, 6};
            d = r.ch[at + off[k]].data;
            owant("o-c34", "pair n", r32(d), 2);
            owant("o-c34", "DRAINED epoch", (long long)(uint32_t)r64(d + 16), ep[k]);
            owant("o-c34", "DRAINED events_through", (long long)r64(d + 24), (long long)th[k]);
            owant("o-c34", "DRAIN_TB tb_copy = the walk's tb1 = bracket 0 tb1", (long long)r64(d + 40), (long long)tb1);
        }
        owant("o-c34", "EPOCH 2", r32(r.ch[at + 4].data + 8), 2);
        owant("o-c34", "EPOCH 3", r32(r.ch[at + 7].data + 8), 3);
        owant("o-c34", "EPOCH 4", r32(r.ch[at + 8].data + 8), 4);
        owant("o-c34", "no pair from the CAL-only walk", (long long)order_count(&r, h->lane_id, 2), 3);
        owant("o-c34", "three BRACKET_AT (walk, CAL-only walk, final cycle)", (long long)order_count(&r, h->lane_id, 1), 3);
    }
    tc = created_tb(&r, h->lane_id);
    bracket_tbs(&r, h->domain_id, 4, 0, &tbB0, &tbB1);
    t = ticks_open("t-o-c34");
    {
        char u1[32];
        snprintf(u1, sizeof u1, "%llu", (unsigned long long)tb1);
        for (uint64_t s = 0; s < 3; s++)
            obs_line(t, h, s, tc, u1, tb1, tb1);
        for (uint64_t s = 3; s < 6; s++) /* Lower-1: bracket 0's tb0; Upper-2: the epoch-4 bracket */
            obs_line(t, h, s, tb0, u1, tbB1, tb1 < tbB1 ? tb1 : tbB1);
    }
    fclose(t);

    /* C3-5 reader fixtures derived from C3-4, each MALFORMED with the named reason */
    for (int d = -1; d <= 1; d += 2) { /* events_through = cursor +- 1 (both records) */
        v = chv_of(&r);
        i = find_ord(&v, 2, 0);
        w64(v.c[i].data + 24, 3 + (uint64_t)(long long)d);
        w64(v.c[i].data + 48, 3 + (uint64_t)(long long)d);
        write_fixture(&r, &v, d > 0 ? "t-o-c35-through-plus1" : "t-o-c35-through-minus1", 0,
                      "exit 2\nverdict malformed the cursor is\n");
        chv_free(&v);
    }
    for (int d = -1; d <= 1; d += 2) { /* DRAINED epoch = current -+ 1 (the epoch-2 pair) */
        v = chv_of(&r);
        i = find_ord(&v, 2, 1);
        w32(v.c[i].data + 16, (uint32_t)(2 + d));
        write_fixture(&r, &v, d > 0 ? "t-o-c35-epoch-plus1" : "t-o-c35-epoch-minus1", 0,
                      "exit 2\nverdict malformed the current epoch is\n");
        chv_free(&v);
    }
    v = chv_of(&r); /* DRAIN_TB events_through different from DRAINED's */
    i = find_ord(&v, 2, 0);
    w64(v.c[i].data + 48, 4);
    write_fixture(&r, &v, "t-o-c35-drain-tb-through", 0, "exit 2\nverdict malformed paired with DRAIN_TB\n");
    chv_free(&v);
    v = chv_of(&r); /* n = 2 holding (DRAIN_TB, DRAINED) */
    i = find_ord(&v, 2, 0);
    {
        uint8_t tmp[24];
        memcpy(tmp, v.c[i].data + 8, 24);
        memcpy(v.c[i].data + 8, v.c[i].data + 32, 24);
        memcpy(v.c[i].data + 32, tmp, 24);
    }
    write_fixture(&r, &v, "t-o-c35-swapped-pair", 0,
                  "exit 2\nverdict malformed only DRAINED then DRAIN_TB may share a chunk\n");
    chv_free(&v);
    v = chv_of(&r); /* a DRAINED alone (n = 1) */
    i = find_ord(&v, 2, 0);
    w32(v.c[i].data, 1);
    v.c[i].len = 8 + 24;
    write_fixture(&r, &v, "t-o-c35-drained-alone", 0, "exit 2\nverdict malformed without its partner\n");
    chv_free(&v);
    v = chv_of(&r); /* events_through equal to the previous DRAINED's: a duplicate pair */
    i = find_ord(&v, 2, 0);
    chv_dup(&v, (size_t)i, (size_t)i + 1);
    write_fixture(&r, &v, "t-o-c35-duplicate-pair", 0, "exit 2\nverdict malformed not above the previous\n");
    chv_free(&v);
    v = chv_of(&r); /* BRACKET_AT.events_before = cursor + 1 */
    i = find_ord(&v, 1, 0);
    w64(v.c[i].data + 24, 4);
    write_fixture(&r, &v, "t-o-c35-before-plus1", 0, "exit 2\nverdict malformed outside\n");
    chv_free(&v);
    v = chv_of(&r); /* BRACKET_AT.events_before below the previous DRAINED's events_through (the epoch-4 bracket) */
    i = find_ord(&v, 1, 1);
    w64(v.c[i].data + 24, 5);
    write_fixture(&r, &v, "t-o-c35-before-below-drained", 0, "exit 2\nverdict malformed outside\n");
    chv_free(&v);
    run_free(&r);

    /* mutants */
    c34_run(&r, &h, PSP_OM_END_START_EPOCH); /* one pair at the walk's end, walk-start epoch */
    e = odone(&r, "t-o-c34-m-end-start-epoch", CK_RUN, OC_EPOCH | OC_RUN);
    fprintf(e, "exit 2\nverdict malformed the current epoch is\n");
    fclose(e);
    run_free(&r);
    c34_run(&r, &h, PSP_OM_END_CUR_EPOCH); /* one pair at the walk's end, current epoch: format-valid */
    e = odone(&r, "t-o-c34-m-end-cur-epoch", CK_RUN, OC_RUN);
    fprintf(e, "exit 0\nverdict complete\nlane %u COMPLETE 1 1 6 0\n", h->lane_id);
    fclose(e);
    owant("o-c34-m-end-cur-epoch", "one pair from the walk", (long long)order_count(&r, h->lane_id, 2), 1);
    run_free(&r);
    c34_run(&r, &h, PSP_OM_EVENTS_BEFORE_CURSOR); /* r1 10.1 negative control: the equality test fails */
    e = odone(&r, "t-o-m-events-before", CK_RUN, OC_BEFORE);
    fprintf(e, "exit 0\nverdict complete\nlane %u COMPLETE 1 1 6 0\n", h->lane_id);
    fclose(e);
    run_free(&r);
    c34_run(&r, &h, PSP_OM_TB_BEFORE_LOADS); /* r1 10.1 negative control: the ordering assertion fails */
    e = odone(&r, "t-o-m-tb-before-loads", CK_RUN, OC_TBORDER);
    fprintf(e, "exit 0\nverdict complete\nlane %u COMPLETE 1 1 6 0\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

/* C3-4 variant: an old-epoch run holding only a LOSS of 4 (its CAL follows the LOSS) */
static void t_o_c34_loss(void)
{
    run r;
    lane_h *h;
    FILE *e;
    chv v;
    long i;
    oinit(&r, 64, 4, NULL);
    h = lane_new(&r, 16, 1, 1);
    odrain(&r); /* EPOCH 1, request 1 */
    inject_older(h, 4);
    owant("o-c34-loss", "dec_write", odecw(h, 0xE0000000u), PSP_OK); /* LOSS(4) + CAL, then EPOCH */
    ozones(h, 1); /* seq 4..5 */
    of(h);
    odrain(&r);
    ofini(h);
    oend(&r, h);
    e = odone(&r, "t-o-c34-loss", CK_RUN, 0);
    fprintf(e, "exit 4\nverdict complete\nlane %u COMPLETE 0 1 2 4\nlosses %u 0:4\n", h->lane_id, h->lane_id);
    fclose(e);
    v = chv_of(&r);
    i = find_ord(&v, 2, 0);
    owant("o-c34-loss", "the LOSS-only run's pair epoch", (long long)(uint32_t)r64(v.c[i].data + 16), 1);
    owant("o-c34-loss", "the LOSS-only run's events_through", (long long)r64(v.c[i].data + 24), 4);
    i = find_ord(&v, 1, 0);
    owant("o-c34-loss", "BRACKET_AT after the LOSS: events_before", (long long)r64(v.c[i].data + 24), 4);
    /* C3-5: a LOSS chunk of 4 before the DRAINED not counted (events_through = cursor - 4) */
    i = find_ord(&v, 2, 0);
    w64(v.c[i].data + 24, 0);
    w64(v.c[i].data + 48, 0);
    write_fixture(&r, &v, "t-o-c35-loss-not-counted", 0, "exit 2\nverdict malformed the cursor is\n");
    chv_free(&v);
    run_free(&r);
}

/* ---- r1 10.1: LOSS(tail) before the CAL; C3-7: the same with its flush_loss refused by the LOSS ledger ---- */

static void losstail_run(run *r, lane_h **hp, int overflow)
{
    lane_h *h;
    psp_limits lim = psp_reader_limits();
    if (overflow)
        lim.loss_chunks = 1; /* the lane's terminal reservation takes it: every other LOSS chunk is refused */
    oinit(r, 16, 4, &lim);
    h = lane_new(r, 4, 1, 1);
    odrain(r);    /* EPOCH 1, request 1 */
    ozones(h, 3); /* seq 0..5: 0..3 staged, 4..5 tail-lost */
    owant("losstail", "flush", of(h), PSP_OK); /* events, LOSS(tail 2), CAL */
    owant("losstail", "dec_write", odecw(h, 0xE0000000u), PSP_OK);
    odrain(r);
    ofini(h);
    oend(r, h);
    *hp = h;
}

static void t_o_losstail(void)
{
    run r;
    lane_h *h;
    FILE *e, *t;
    uint64_t em, dr;
    chv v;
    long i;
    losstail_run(&r, &h, 0);
    e = odone(&r, "t-o-losstail", CK_RUN, 0);
    fprintf(e, "exit 4\nverdict complete\nlane %u COMPLETE 0 1 4 2\nlosses %u 4:2\n", h->lane_id, h->lane_id);
    fclose(e);
    v = chv_of(&r);
    i = find_ord(&v, 1, 0);
    owant("o-losstail", "events_before = the last lost seq + 1", (long long)r64(v.c[i].data + 24), 6);
    chv_free(&v);
    run_free(&r);

    losstail_run(&r, &h, 1);
    e = odone(&r, "t-o-c37", CK_RUN, 0);
    fprintf(e, "exit 4\nverdict complete\nlane %u INCOMPLETE 0 3 4 0\n", h->lane_id);
    fclose(e);
    owant("o-c37", "CALIBRATION emitted", (long long)(find_type(&(chv){r.ch, r.nch}, 10, 0) >= 0), 1);
    owant("o-c37", "no BRACKET_AT (events_before beyond the cursor)", (long long)order_count(&r, h->lane_id, 1), 0);
    owant("o-c37", "the pair (the cursor advanced)", (long long)order_count(&r, h->lane_id, 2), 1);
    owant("o-c37", "ORDER_FINAL", order_final(&r, h->lane_id, &em, &dr), 1);
    owant("o-c37", "records_dropped", (long long)dr, 1);
    t = ticks_open("t-o-c37");
    fprintf(t, "has unpositioned brackets 1\nhas TICKS-UNAVAILABLE(\n");
    fclose(t);
    run_free(&r);
}

void order_traces(const char *dir, int *ntraces)
{
    g_odir = dir;
    g_ont = ntraces;
    t_o_clean();
    c31(0);
    c31(1);
    t_o_c32();
    t_o_create();
    t_o_c33();
    t_o_c34();
    t_o_c34_loss();
    t_o_losstail();
}
