/* Deterministic traces of the SPU lane transport model (design p3.2 r3 sections 4, A, C, D, E and the review 01:35
 * corrections).  Each trace writes <name>.prof, .truth and .expect; the stream check runs here, and the driver runs
 * the unchanged reader and the checker on every file.  Limits are the reader's unless a trace is named "model-limit".
 * Model evidence only. */
#define _GNU_SOURCE /* strdup, memmem (test code only) */
#include <stdlib.h>
#include <string.h>

#include "model.h"

int sched_main(const char *dir);
void order_traces(const char *dir, int *ntraces);

static const char *g_dir;
static int g_traces;

static void zones(lane_h *h, int k)
{
    for (int i = 0; i < k; i++) {
        zb(h, 0);
        ze(h, 0);
    }
}

static int flush(lane_h *h)
{
    int rc = psp_spu_flush(&h->s);
    call_ok(h, "flush");
    return rc;
}

static int fini(lane_h *h)
{
    int rc = psp_spu_fini(&h->s);
    call_ok(h, "fini");
    return rc;
}

static int decw(lane_h *h, uint32_t v)
{
    int rc = psp_spu_dec_write(&h->s, v);
    call_ok(h, "dec_write");
    return rc;
}

static void want(const char *trace, const char *what, long long got, long long exp)
{
    if (got != exp)
        FAIL("%s: %s = %lld, want %lld", trace, what, got, exp);
}

static FILE *done(run *r, const char *name)
{
    FILE *e;
    psp_seal(&r->p);
    check_streams(r, name);
    write_capture(r, g_dir, name);
    e = expect_open(g_dir, name);
    g_traces++;
    return e;
}

static void lane_line(FILE *e, lane_h *h, const char *rest) { fprintf(e, "lane %u %s\n", h->lane_id, rest); }

/* ---- r2 section 4, as amended ---- */

static void t_clean(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 2);
    want("clean", "flush", flush(h), PSP_OK);
    psp_drain(&r.p);
    zm(h, 1);
    want("clean", "fini", fini(h), PSP_OK);
    want("clean", "end", psp_lane_end(&r.p, h->idx), PSP_P_OK);
    e = done(&r, "t-clean");
    fprintf(e, "exit 0\nverdict complete\n");
    lane_line(e, h, "COMPLETE 1 1 5 0");
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_full_ring(void) /* r2 trace 1 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 2);
    flush(h);
    zones(h, 2);
    flush(h); /* w 8, r 0: full */
    zones(h, 2);
    want("full", "flush rc", flush(h), PSP_OK);
    want("full", "older_lost", (long long)h->s.older_lost, 4); /* seq 8..11: nothing fitted */
    zb(h, 0);                                                 /* E12 */
    psp_drain(&r.p);
    flush(h); /* [PAD LOSS(4) E12 CAL(1)]: the first drain issued request 1 */
    want("full", "w", (long long)h->s.w, 12);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-full-ring");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 9 4");
    fprintf(e, "losses %u 8:4\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_partial_even(void) /* r2 trace 2 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    for (int i = 0; i < 3; i++) {
        zones(h, 1);
        flush(h);
    } /* w 6, r 0: space 2 */
    zb(h, 0);
    ze(h, 0);
    zb(h, 0); /* E6 E7 E8 */
    flush(h); /* [E6 E7]; E8 -> older 1 */
    want("partial", "w", (long long)h->s.w, 8);
    want("partial", "older_lost", (long long)h->s.older_lost, 1);
    psp_drain(&r.p);
    ze(h, 0); /* E9 */
    flush(h); /* [LOSS(1) PAD E9 PAD]: an odd front marker takes a PAD for 16-byte alignment */
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-partial-even");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 9 1");
    fprintf(e, "losses %u 8:1\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_odd_pad(void) /* r2 trace 3 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zb(h, 0);
    ze(h, 0);
    zb(h, 0);
    flush(h); /* [E0 E1 E2 PAD] */
    want("odd", "w", (long long)h->s.w, 4);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-odd-pad");
    fprintf(e, "exit 0\nverdict complete\n");
    lane_line(e, h, "COMPLETE 1 1 3 0"); /* one BEGIN open at FINAL, verified (history known) */
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_loss_split(void) /* r2 trace 4: an injected older loss above 0xFFFFFFFF */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    inject_older(h, (1ull << 32) + 5);
    zb(h, 0);
    ze(h, 0);
    flush(h); /* [LOSS(0xFFFFFFFF) LOSS(6) E E] */
    want("split", "w", (long long)h->s.w, 4);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-loss-split");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 2 4294967301");
    fprintf(e, "losses %u 0:4294967301\n", h->lane_id); /* the two adjacent markers merge into one LOSS */
    fclose(e);
    run_free(&r);
}

static void t_wrap(void) /* r2 trace 5 */
{
    run r;
    lane_h *h;
    FILE *e;
    int before;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);        /* [E E] w 2 */
    psp_drain(&r.p); /* r 2, request 1 */
    zones(h, 1);
    flush(h); /* [E E CAL(1) PAD] w 6, r 2 */
    want("wrap", "w", (long long)h->s.w, 6);
    zones(h, 2);
    before = h->pc.dma_count;
    flush(h); /* space 4: [E E E E] at slots 6, 7, 0, 1 */
    want("wrap", "dma commands", h->pc.dma_count - before, 2);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-wrap");
    fprintf(e, "exit 0\nverdict complete\n");
    lane_line(e, h, "COMPLETE 1 1 8 0");
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_fini_pending(void) /* r2 trace 6 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 2);
    flush(h);
    zones(h, 2);
    flush(h); /* full */
    zb(h, 0);
    ze(h, 0);
    zb(h, 0);
    want("fini-pending", "fini", fini(h), PSP_OK); /* nothing fits: CLOSED carries older 3 */
    want("fini-pending", "older", (long long)h->s.older_lost, 3);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-fini-pending");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 8 3");
    fprintf(e, "losses %u 8:3\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_abandoned(void) /* r2 trace 7 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 2);
    zb(h, 0);
    flush(h); /* LS 4: four events staged, the fifth dropped: [E0..E3 LOSS(1) PAD] */
    psp_drain(&r.p);
    zones(h, 1); /* staged, never published: the thread is terminated */
    want("abandoned", "end", psp_lane_end(&r.p, h->idx), PSP_P_OK);
    e = done(&r, "t-abandoned");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 4 1");
    fprintf(e, "losses %u 4:1\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void steps(run *r, uint32_t lane, int n)
{
    for (int i = 0; i < n; i++)
        psp_lane_step(&r->p, lane);
}

static void t_epoch_race(void) /* r2 trace 8, as amended */
{
    run r;
    lane_h *h;
    FILE *e;
    psp_lane *pl;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 1, 1);
    pl = &r.p.lanes[h->idx];
    zones(h, 1);
    flush(h);
    psp_drain(&r.p); /* EPOCH 1, issues id 1 */
    flush(h);        /* [CAL(1) PAD] */
    psp_drain(&r.p); /* bracket epoch 1 seq 0; issues id 2 */
    flush(h);        /* [CAL(2) PAD] */
    psp_drain_declare(&r.p);
    steps(&r, h->idx, 4); /* drain 3 snapshots before the decrementer write */
    want("race", "dec_write", decw(h, 0x7FFFFFFFu), PSP_OK);
    steps(&r, h->idx, 3); /* walk (CAL(2) -> epoch-1 bracket seq 1), read_idx, request id 3 with epoch 1 */
    want("race", "outstanding", pl->out_id, 3);
    zb(h, 1);
    flush(h);        /* [E CAL(3)] after the EPOCH */
    psp_drain(&r.p); /* cancels id 3 at the EPOCH, EPOCH 2, E, CAL(3) stale; issues id 4 */
    want("race", "stale", (long long)pl->stale, 1);
    want("race", "cancelled", (long long)pl->cancelled, 1);
    flush(h);        /* [CAL(4) PAD] */
    psp_drain(&r.p); /* bracket epoch 2 seq 0 */
    ze(h, 1);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    want("race", "decrementer writes", h->pc.nwr, 1);
    e = done(&r, "t-epoch-race");
    fprintf(e, "exit 0\nverdict complete\nepochs 2\nbrackets 4\n"); /* ids 1, 2, 4, and 5 (answered by fini) */
    /* the write happened at depth 0 with a known history: the depth-zero rule keeps it known across EPOCH 2 */
    lane_line(e, h, "COMPLETE 1 1 4 0");
    fprintf(e, "open %u verified 0\n", h->lane_id);
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_dec_refused(void) /* r2 trace 9 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 1, 1);
    zones(h, 2);
    flush(h);
    zones(h, 2);
    flush(h); /* full */
    want("dec-refused", "dec_write", decw(h, 5), PSP_EAGAIN);
    want("dec-refused", "decrementer writes", h->pc.nwr, 0);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-dec-refused");
    fprintf(e, "exit 0\nverdict complete\nepochs 1\n");
    lane_line(e, h, "COMPLETE 1 1 8 0");
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_depth(void) /* r2 trace 10 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 256, 4, NULL);
    h = lane_new(&r, 160, 0, 1);
    for (int i = 0; i < 64; i++)
        zb(h, 0);  /* depths 0..63 recorded */
    zb(h, 1);      /* depth 64: suppressed */
    ze(h, 1);      /* its END: suppressed, pops nothing */
    want("depth", "depth", h->s.depth, 64);
    for (int i = 0; i < 64; i++)
        ze(h, 0);
    want("depth", "refused", (long long)h->s.refused, 2);
    flush(h);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-depth");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 3 128 0"); /* any refusal latches OVERFLOWED */
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_never_started(void) /* r2 trace 11 */
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 0);
    want("never", "end", psp_lane_end(&r.p, h->idx), PSP_P_OK);
    want("never", "epochs reserved", (long long)r.p.epochs_reserved, 0);
    want("never", "loss reserved", (long long)r.p.loss_reserved, 0);
    e = done(&r, "t-never-started");
    fprintf(e, "exit 4\nverdict complete\nepochs 0\n");
    lane_line(e, h, "INCOMPLETE 0 2 0 0");
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_ebusy(void) /* r2 trace 12 */
{
    run r;
    lane_h *h;
    FILE *e;
    int dma;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    h->pc.blocking = 1;
    dma = h->pc.dma_count;
    want("ebusy", "flush", flush(h), PSP_EBUSY);
    want("ebusy", "dma", h->pc.dma_count - dma, 0);
    want("ebusy", "staged", h->s.n, 2);
    want("ebusy", "refused_calls", (long long)h->s.refused_calls, 1);
    h->pc.blocking = 0;
    flush(h);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-ebusy");
    fprintf(e, "exit 0\nverdict complete\n");
    lane_line(e, h, "COMPLETE 1 1 2 0");
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

/* ---- A: ordered loss ---- */

static void t_a1(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 2); /* E0..E3 staged */
    zb(h, 0);    /* E4 dropped behind them */
    want("a1", "tail_lost", (long long)h->s.tail_lost, 1);
    flush(h); /* [E0 E1 E2 E3 LOSS(1) PAD] */
    want("a1", "w", (long long)h->s.w, 6);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-a1-ls-overflow");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 4 1");
    fprintf(e, "losses %u 4:1\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_a2(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    inject_older(h, 2); /* seq 0..1 */
    zones(h, 2);        /* E2..E5 */
    zb(h, 0);
    ze(h, 0);           /* E6 E7 dropped */
    flush(h);           /* [LOSS(2) PAD E2..E5 LOSS(2) PAD] = 8 */
    want("a2", "w", (long long)h->s.w, 8);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-a2-old-new-loss");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 4 4");
    fprintf(e, "losses %u 0:2 6:2\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_a3(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    inject_older(h, 2); /* seq 0..1 */
    zones(h, 2);        /* E2..E5 */
    zb(h, 0);
    ze(h, 0); /* E6 E7 dropped */
    /* make space 2: a reader index two slots short of the ring (written as the PPU would) */
    h->s.w = 6;
    h->s.r_seen = 0;
    r.p.lanes[h->idx].r = 0;
    flush(h); /* space 2: [LOSS(2) PAD] (alignment); everything after the cut merges: older' = 4 + 2 = 6 */
    want("a3", "older'", (long long)h->s.older_lost, 6);
    /* restore a consistent ring: the 6 phantom slots are PADs the drainer skips */
    for (uint32_t i = 0; i < 6; i++)
        psp_slot(g_mem + r.p.lanes[h->idx].ea_ring + i * 8, 0, 0, PSP_K_PAD, 0);
    psp_drain(&r.p);
    zb(h, 0); /* E8 */
    flush(h); /* [LOSS(6) PAD E8 PAD] */
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-a3-partial-publication");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 1 8");
    fprintf(e, "losses %u 0:2 2:6\n", h->lane_id); /* separate chunks: drains between them */
    fclose(e);
    run_free(&r);
}

static void t_a4(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 2);
    flush(h);
    zones(h, 2);
    flush(h); /* seq 0..7 published, ring full */
    /* older 1 at seq 8: a staged event that could not be published */
    zb(h, 0);
    flush(h); /* space 0: E8 -> older 1 */
    zones(h, 2); /* E9..E12 staged */
    zb(h, 0);    /* E13 dropped */
    want("a4", "fini", fini(h), PSP_OK); /* space 0: nothing published; CLOSED older = 1 + 4 + 1 */
    want("a4", "older", (long long)h->s.older_lost, 6);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-a4-fini-unpublished");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 8 6");
    fprintf(e, "losses %u 8:6\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_a5(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    inject_older(h, 1); /* the older loss at seq 0 */
    psp_drain(&r.p);    /* EPOCH 1; request id 1 */
    zb(h, 0);
    ze(h, 0);
    zb(h, 0); /* E1 E2 E3 */
    /* space 4: four phantom PAD slots the drainer will skip, written as an earlier publication would */
    h->s.w = 4;
    for (uint32_t i = 0; i < 4; i++)
        psp_slot(g_mem + r.p.lanes[h->idx].ea_ring + i * 8, 0, 0, PSP_K_PAD, 0);
    flush(h); /* [PAD LOSS(1) E1 E2]: cut; E3 merges into older; the CAL does not fit and stays pending */
    want("a5", "cal answered", h->s.cal_answered, 0);
    want("a5", "older", (long long)h->s.older_lost, 1);
    psp_drain(&r.p);
    flush(h); /* [PAD LOSS(1) CAL(1) PAD]: answered at the next flush, never dropped */
    want("a5", "cal answered", h->s.cal_answered, 1);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-a5-cal-deferred");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "COMPLETE 0 1 2 2");
    fprintf(e, "losses %u 0:1 3:1\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

/* ---- C: capacity (reader limits unless named model-limit) ---- */

static void t_c1(void)
{
    run r;
    FILE *e;
    lane_h *h, *live[64];
    char name[] = "t-c1-live-64";
    run_init(&r, 8, 64, NULL);
    for (int i = 0; i < 64; i++)
        live[i] = lane_new(&r, 4, 0, 1);
    if (lane_new(&r, 4, 0, 1))
        FAIL("c1: a 65th live lane was created");
    for (int i = 0; i < 64; i++) {
        fini(live[i]);
        psp_lane_end(&r.p, live[i]->idx);
    }
    e = done(&r, name);
    fprintf(e, "exit 0\nverdict complete\nsources 64\n"); /* started and closed with no events: COMPLETE, VALID */
    fclose(e);
    run_free(&r);

    run_init(&r, 8, 64, NULL);
    for (int i = 0; i < 1024; i++) {
        h = lane_new(&r, 4, 0, 1);
        if (!h) {
            FAIL("c1: lane %d refused", i);
            break;
        }
        zones(h, 1);
        flush(h);
        fini(h);
        psp_lane_end(&r.p, h->idx);
        lane_drop(&r, h);
    }
    if (lane_new(&r, 4, 0, 1))
        FAIL("c1: lane 1025 was created");
    e = done(&r, "t-c1-lifetime-1024");
    fprintf(e, "exit 0\nverdict complete\nsources 1024\n");
    fclose(e);
    run_free(&r);
}

/* one cycle on lane h: four events, one drop, flush, drain: one SPU_EVENTS and one LOSS chunk */
static void loss_cycle(run *r, lane_h *h)
{
    zones(h, 2);
    zm(h, 0); /* dropped: a MARK, so the producer's depth never grows across cycles */
    flush(h);
    psp_drain(&r->p);
}

static void c2(int control)
{
    run r;
    lane_h *a, *b;
    FILE *e;
    const char *name = control ? "t-c2-loss-budget-no-reservation-control" : "t-c2-loss-budget";
    uint64_t cycles = 0;
    run_init(&r, 8, 4, NULL);
    r.p.test_no_terminal = control;
    a = lane_new(&r, 4, 0, 1);
    b = lane_new(&r, 4, 0, 1);
    while (r.p.lanes[a->idx].state != PSP_L_FINAL && cycles < 70000) {
        loss_cycle(&r, a);
        cycles++;
    }
    want(name, "lane 1 loss chunks", (long long)r.p.loss_emitted, control ? 65536 : 65534);
    /* lane 2 closes with pending loss: its ring filled without a drain, then a staged event */
    while (b->s.w - b->s.r_seen < 8) {
        zones(b, 1);
        flush(b);
    }
    zb(b, 0);
    fini(b);
    psp_lane_end(&r.p, b->idx);
    fini(a);
    psp_lane_end(&r.p, a->idx);
    e = done(&r, name);
    if (control) {
        fprintf(e, "exit 2\nverdict malformed more LOSS chunks than the per-file bound\n");
    } else {
        want(name, "total loss chunks", (long long)r.p.loss_emitted, 65535);
        fprintf(e, "exit 4\nverdict complete\n");
        fprintf(e, "lane %u INCOMPLETE 0 3 %llu %llu\n", a->lane_id, (unsigned long long)(cycles * 4),
                (unsigned long long)(cycles - 1));
        /* lane 2 from the producer's own counts: everything but its CLOSED older range was published */
        fprintf(e, "lane %u COMPLETE 0 1 %llu %llu\n", b->lane_id, (unsigned long long)(b->s.seq - b->s.older_lost),
                (unsigned long long)b->s.older_lost);
        fprintf(e, "losses %u %llu:%llu\n", b->lane_id, (unsigned long long)(b->s.seq - b->s.older_lost),
                (unsigned long long)b->s.older_lost);
    }
    fclose(e);
    run_free(&r);
}

static void t_c3(void)
{
    run r;
    lane_h *a, *b;
    FILE *e;
    int writes = 0;
    run_init(&r, 8, 4, NULL);
    a = lane_new(&r, 4, 1, 1);
    b = lane_new(&r, 4, 0, 0); /* reserves its EPOCH 1, starts later */
    psp_drain(&r.p);           /* lane 1 EPOCH 1 */
    for (int iter = 0; r.p.lanes[a->idx].state != PSP_L_FINAL && iter < 40000; iter++) {
        if (decw(a, (uint32_t)(0x10000u + writes)) == PSP_OK) {
            writes++;
            if (writes % 2 == 0)
                psp_drain(&r.p);
        } else {
            psp_drain(&r.p); /* EAGAIN: the ring (or a CAL answer) needs a drain first */
        }
    }
    want("c3", "dec writes accepted before the budget ran out", writes >= 4095, 1);
    psp_drain(&r.p);
    want("c3", "epochs emitted after lane 1 finished", (long long)r.p.epochs_emitted, 4095);
    want("c3", "lane 1 final state", r.p.lanes[a->idx].fin_state, PSP_FIN_OVERFLOWED);
    if (psp_spu_init(&b->s, &b->port, r.p.lanes[b->idx].ea_ring, 8, r.p.lanes[b->idx].ea_ctl, 5, b->buf, 4, 0))
        FAIL("c3: lane 2 init");
    zones(b, 1);
    flush(b);
    psp_drain(&r.p); /* lane 2 EPOCH 1 = number 4096 */
    fini(b);
    psp_lane_end(&r.p, b->idx);
    fini(a);
    psp_lane_end(&r.p, a->idx);
    e = done(&r, "t-c3-epoch-budget");
    fprintf(e, "exit 4\nverdict complete\nepochs 4096\n");
    lane_line(e, a, "INCOMPLETE 0 3 0 0");
    lane_line(e, b, "COMPLETE 1 1 2 0");
    fclose(e);
    run_free(&r);
}

/* answer requests until the ledger stops issuing them; returns the brackets emitted */
static void t_c4(void)
{
    run r;
    lane_h *h[5];
    FILE *e;
    int round;
    run_init(&r, 8, 8, NULL);
    for (int i = 0; i < 5; i++)
        h[i] = lane_new(&r, 4, 0, 1);
    psp_drain(&r.p);
    for (round = 0; round < 70000; round++) {
        for (int i = 0; i < 5; i++)
            flush(h[i]);
        psp_drain(&r.p);
        if (r.p.brackets_file == 262144 && !r.p.lanes[h[4]->idx].res_bracket)
            break;
    }
    for (int i = 0; i < 5; i++)
        flush(h[i]);
    psp_drain(&r.p);
    want("c4", "brackets", (long long)r.p.brackets_file, 262144);
    for (int i = 0; i < 5; i++) {
        fini(h[i]);
        psp_lane_end(&r.p, h[i]->idx);
    }
    e = done(&r, "t-c4-brackets-per-file");
    fprintf(e, "exit 0\nverdict complete\nbrackets 262144\n"); /* lanes with no events: COMPLETE, VALID */
    fclose(e);
    run_free(&r);
}

static void t_c4_model_epoch(void)
{
    run r;
    lane_h *h;
    FILE *e;
    psp_limits lim = psp_reader_limits();
    lim.brackets_epoch = 3; /* MODEL LIMIT: the reader's per-epoch bound (65536) is above the u16 id space */
    run_init(&r, 8, 4, &lim);
    h = lane_new(&r, 4, 1, 1);
    psp_drain(&r.p);
    for (int i = 0; i < 6; i++) {
        flush(h);
        psp_drain(&r.p);
    }
    want("c4b", "brackets in epoch 1", (long long)r.p.brackets_file, 3);
    decw(h, 9);
    for (int i = 0; i < 8; i++) {
        flush(h);
        psp_drain(&r.p);
    }
    want("c4b", "brackets", (long long)r.p.brackets_file, 6);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-c4b-model-limit-brackets-epoch");
    /* no events and a depth-0 write: the depth-zero rule keeps the history known, the source is RAW VALID */
    fprintf(e, "exit 0\nverdict complete\nbrackets 6\nepochs 2\nopen %u verified 0\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_c5(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    r.p.lanes[h->idx].next_req = 65534; /* injected starting id; the bound itself is the u16 id space */
    psp_drain(&r.p);
    for (int i = 0; i < 6; i++) {
        flush(h);
        psp_drain(&r.p);
    }
    want("c5", "brackets", (long long)r.p.brackets_file, 2);
    want("c5", "next id", r.p.lanes[h->idx].next_req, 65536);
    want("c5", "outstanding", r.p.lanes[h->idx].res_bracket, 0);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-c5-request-id-exhaustion");
    fprintf(e, "exit 0\nverdict complete\nbrackets 2\n");
    lane_line(e, h, "COMPLETE 1 1 0 0");
    fclose(e);
    run_free(&r);
}

/* ---- D: call transitions and faults ---- */

static void t_d1_kill_after_write(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 1, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    want("d1", "flush", flush(h), PSP_OK);
    want("d1", "preflight", psp_spu_dec_preflight(&h->s), PSP_OK);
    psp_spu_dec_commit(&h->s, 77); /* the write happened ... */
    /* ... and the thread is terminated before its EPOCH is published */
    want("d1", "decrementer writes", h->pc.nwr, 1);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d1-kill-after-dec-write");
    fprintf(e, "exit 4\nverdict complete\nepochs 1\n"); /* no claim of a write: no EPOCH 2 */
    lane_line(e, h, "INCOMPLETE 0 2 2 0");
    fclose(e);
    run_free(&r);
}

static void t_d2_kill_before_publish(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    zones(h, 1);
    psp_spu_flush_plan(&h->s);
    psp_spu_flush_put(&h->s);
    psp_spu_flush_wait(&h->s); /* the data is in the ring; line B never published it */
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d2-kill-before-publish");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 2 0");
    fclose(e);
    run_free(&r);
}

static void t_d3_fini_ebusy_retry(void)
{
    run r;
    lane_h *h;
    FILE *e;
    int dma;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zb(h, 0);
    ze(h, 0);
    zb(h, 0);
    h->pc.blocking = 1;
    dma = h->pc.dma_count;
    want("d3", "fini", fini(h), PSP_EBUSY);
    want("d3", "state", (long long)psp_rd32(g_mem + r.p.lanes[h->idx].ea_ctl + PSP_B_STATE), PSP_ST_STARTED);
    want("d3", "staged", h->s.n, 3);
    want("d3", "dma", h->pc.dma_count - dma, 0);
    h->pc.blocking = 0;
    want("d3", "fini retry", fini(h), PSP_OK);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d3-fini-ebusy-retry");
    fprintf(e, "exit 0\nverdict complete\n");
    lane_line(e, h, "COMPLETE 1 1 3 0");
    fclose(e);
    run_free(&r);
}

static void t_d4_fini_efault(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    h->pc.fail_closed_store = 1; /* the flush succeeds; the CLOSED putlluc never completes */
    want("d4", "fini", fini(h), PSP_EFAULT);
    want("d4", "fini again", fini(h), PSP_EFAULT);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d4-fini-efault");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 2 0"); /* the PPU never saw the producer's fault: ABANDONED at the prefix */
    fclose(e);
    run_free(&r);
}

static void t_d5_local_fault(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    zones(h, 1);
    h->pc.fault_at_op = h->pc.ops + 1; /* the getllar status is missing */
    want("d5", "flush", flush(h), PSP_EFAULT);
    want("d5", "flush again", flush(h), PSP_EFAULT);
    want("d5", "fini", fini(h), PSP_EFAULT);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d5-local-fault");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 2 0");
    fclose(e);
    run_free(&r);
}

static void t_d6_published_corruption(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    zones(h, 1);
    flush(h);
    g_mem[r.p.lanes[h->idx].ea_ring + 2 * 8 + 6] = 0x55; /* a published slot with an unknown discriminator */
    psp_drain(&r.p);
    want("d6", "corrupt", r.p.corrupt, 1);
    fini(h);
    want("d6", "end", psp_lane_end(&r.p, h->idx), PSP_P_OK);
    e = done(&r, "t-d6-published-corruption");
    fprintf(e, "exit 1\nverdict incomplete\n"); /* the sink failed: no trailer */
    fclose(e);
    run_free(&r);

    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_be32(g_mem + r.p.lanes[h->idx].ea_ctl + PSP_B_STATE, 7); /* a published state outside the enum */
    psp_drain(&r.p);
    want("d6b", "corrupt", r.p.corrupt, 1);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d6b-published-state-corruption");
    fprintf(e, "exit 1\nverdict incomplete\n");
    fclose(e);
    run_free(&r);
}

static void t_d7_last_generation(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 1, 1);
    h->s.gen = PSP_GEN_MAX - 4; /* injected: two credits left, the last one reserved for CLOSED */
    zones(h, 1);
    flush(h); /* gen MAX-2 */
    psp_drain(&r.p);
    want("d7", "dec_write at the last normal credit", decw(h, 3), PSP_EAGAIN);
    want("d7", "decrementer writes", h->pc.nwr, 0);
    want("d7", "exhausted by dec_write's flush", (h->s.flags & PSP_F_GEN_EXHAUSTED) != 0, 1);
    zb(h, 0); /* refused: terminal */
    want("d7", "flush", flush(h), 7 /* ENOSPC: generation exhausted */);
    want("d7", "fini", fini(h), PSP_OK);
    want("d7", "closing gen", (long long)(h->s.gen == PSP_GEN_MAX), 1);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d7-last-generation");
    fprintf(e, "exit 4\nverdict complete\nepochs 1\n");
    /* dec_write found no normal credit (EAGAIN, nothing written); the exhaustion at that flush stopped admission,
     * so the zb after it was refused without a sequence number: 2 events, no loss, OVERFLOWED */
    lane_line(e, h, "INCOMPLETE 0 3 2 0");
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);

    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 1, 1);
    h->s.gen = PSP_GEN_MAX - 6; /* the EPOCH publication takes the last normal credit */
    want("d7b", "dec_write", decw(h, 11), PSP_OK);
    want("d7b", "gen", (long long)(h->s.gen == PSP_GEN_MAX - 4), 1);
    zones(h, 1);
    flush(h); /* MAX-2 */
    zb(h, 0);
    want("d7b", "flush", flush(h), 7);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d7b-dec-write-last-credit");
    fprintf(e, "exit 4\nverdict complete\nepochs 2\n");
    lane_line(e, h, "INCOMPLETE 0 3 2 1");
    fclose(e);
    run_free(&r);
}

static void t_d7c_write_index(void)
{
    run r;
    lane_h *h;
    FILE *e;
    uint64_t base = PSP_W_MAX - 4;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    h->s.w = h->s.r_seen = base; /* injected near the write-index bound, consistently on both sides */
    psp_be64(g_mem + r.p.lanes[h->idx].ea_ctl + PSP_A_READ_IDX, base);
    r.p.lanes[h->idx].r = base;
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    zones(h, 1);
    flush(h); /* w = 2^63 */
    psp_drain(&r.p);
    zones(h, 1);
    want("d7c", "flush beyond the index bound", flush(h), 7);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-d7c-write-index-bound");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 3 4 2");
    fprintf(e, "losses %u 4:2\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

static void t_d8_bad_read_index(void)
{
    static const char *names[3] = {"t-d8-read-index-odd", "t-d8-read-index-decreasing", "t-d8-read-index-beyond-w"};
    for (int v = 0; v < 3; v++) {
        run r;
        lane_h *h;
        FILE *e;
        uint64_t bad[3] = {3, 2, 100};
        run_init(&r, 8, 4, NULL);
        h = lane_new(&r, 4, 0, 1);
        zones(h, 2);
        flush(h);
        psp_drain(&r.p); /* r 4 */
        zones(h, 1);
        flush(h); /* the producer observes r 4 (and answers request 1) */
        psp_be64(g_mem + r.p.lanes[h->idx].ea_ctl + PSP_A_READ_IDX, bad[v]);
        zones(h, 1);
        want(names[v], "flush", flush(h), PSP_EFAULT);
        psp_lane_end(&r.p, h->idx);
        e = done(&r, names[v]);
        fprintf(e, "exit 4\nverdict complete\n");
        lane_line(e, h, "INCOMPLETE 0 2 6 0");
        fclose(e);
        run_free(&r);
    }
}

/* ---- E: lifetime ---- */

static void t_e1_timeout(void)
{
    run r;
    lane_h *h;
    FILE *e;
    uint64_t used;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    fini(h);
    used = r.p.mem_used;
    psp_lane_end_request(&r.p, h->idx);
    want("e1", "poll 1", psp_lane_end_poll(&r.p, h->idx, 2), PSP_P_EBUSY);
    want("e1", "poll 2", psp_lane_end_poll(&r.p, h->idx, 2), PSP_P_EBUSY);
    want("e1", "poll 3", psp_lane_end_poll(&r.p, h->idx, 2), PSP_P_ETIMEDOUT);
    want("e1", "still charged", (long long)r.p.mem_used, (long long)used);
    psp_drain(&r.p);
    want("e1", "poll after the drain released", psp_lane_end_poll(&r.p, h->idx, 2), PSP_P_OK);
    want("e1", "refunded", (long long)r.p.mem_used, (long long)(used - (8 * 8 + 256)));
    e = done(&r, "t-e1-end-timeout-retry");
    fprintf(e, "exit 0\nverdict complete\n");
    lane_line(e, h, "COMPLETE 1 1 2 0");
    fclose(e);
    run_free(&r);
}

static void t_e2_seal_races_end(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_lane_end_request(&r.p, h->idx);
    psp_seal(&r.p); /* the seal finalizes it first */
    want("e2", "poll", psp_lane_end_poll(&r.p, h->idx, 2), PSP_P_ESTOPPED);
    want("e2", "freed", (long long)r.p.mem_used, 0);
    e = done(&r, "t-e2-seal-races-end");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 2 0");
    fclose(e);
    run_free(&r);
}

static void t_e3_sink_failure(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    r.fail_sink_at = 4;
    h = lane_new(&r, 4, 0, 1);
    for (int i = 0; i < 4; i++) {
        zones(h, 1);
        flush(h);
        psp_drain(&r.p);
    }
    want("e3", "sink failed", r.p.sink_failed, 1);
    fini(h);
    want("e3", "end", psp_lane_end(&r.p, h->idx), PSP_P_OK);
    want("e3", "freed", (long long)r.p.mem_used, 0);
    e = done(&r, "t-e3-sink-failure");
    fprintf(e, "exit 1\nverdict incomplete\n");
    fclose(e);
    run_free(&r);
}

static void t_e4_finalized_live_and_cap(void)
{
    run r;
    lane_h *a, *b, *c;
    FILE *e;
    psp_limits lim = psp_reader_limits();
    lim.loss_chunks = 4; /* MODEL LIMIT */
    run_init_cap(&r, 8, 2, 2 * (8 * 8 + 256), &lim); /* the cap holds exactly two lanes */
    a = lane_new(&r, 4, 0, 1);
    b = lane_new(&r, 4, 0, 1);
    for (int i = 0; i < 3; i++)
        loss_cycle(&r, a); /* the third LOSS would exceed 4 with two terminals reserved */
    want("e4", "lane 1 file-final", r.p.lanes[a->idx].state, PSP_L_FINAL);
    loss_cycle(&r, a); /* the producer is still live; the drainer no longer touches its memory */
    c = lane_new(&r, 4, 0, 1);
    if (c)
        FAIL("e4: a third lane fitted the cap while lane 1 is unfreed");
    fini(a);
    psp_lane_end(&r.p, a->idx);
    c = lane_new(&r, 4, 0, 1);
    if (!c)
        FAIL("e4: the cap did not refund on the actual free");
    fini(b);
    psp_lane_end(&r.p, b->idx);
    if (c) {
        fini(c);
        psp_lane_end(&r.p, c->idx);
    }
    e = done(&r, "t-e4-model-limit-finalized-live-cap");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, a, "INCOMPLETE 0 3 12 2");
    fclose(e);
    run_free(&r);
}

static void t_e5_post_seal_stats(void)
{
    run r;
    lane_h *h;
    FILE *e;
    psp_lane_stats st;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_seal(&r.p);
    if (lane_new(&r, 4, 0, 1))
        FAIL("e5: a lane was created after seal");
    zones(h, 1);
    flush(h); /* the producer outlives the seal; its memory was kept */
    fini(h);
    psp_lane_stats_get(&r.p, h->idx, 1, &st);
    want("e5", "frozen next", (long long)st.next_seq, 2);
    want("e5", "post-join next (not in file)", (long long)st.post_join.next_seq, 4);
    want("e5", "have post join", st.have_post_join, 1);
    want("e5", "poll", psp_lane_end_poll(&r.p, h->idx, 1), PSP_P_ESTOPPED);
    e = done(&r, "t-e5-post-seal-stats");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 2 0");
    fclose(e);
    run_free(&r);
}

/* ---- r2 of slice 1: accepted versus attempted output (review 02:18 item 1) ---- */

/* the sink fails at the nth chunk of a type, after an accepted prefix; stats must equal the accepted chunks and
 * final_written must be false unless a FINAL was accepted */
static void sink_fail_at(uint32_t type, uint32_t nth, const char *name)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    r.fail_type = type;
    r.fail_nth = nth;
    h = lane_new(&r, 4, 0, 1);
    for (int i = 0; i < 3; i++)
        loss_cycle(&r, h); /* SPU_EVENTS, LOSS and (from the second cycle) CALIBRATION each cycle */
    fini(h);
    psp_lane_end_request(&r.p, h->idx);
    psp_drain(&r.p);
    want(name, "sink failed", r.p.sink_failed, 1);
    psp_seal(&r.p);
    check_stats(&r, h, name); /* before the free: the counts the stats report against what was accepted */
    want(name, "poll (the end, not the seal, retired it)", psp_lane_end_poll(&r.p, h->idx, 1), PSP_P_OK);
    check_streams(&r, name);
    write_capture(&r, g_dir, name);
    e = expect_open(g_dir, name);
    g_traces++;
    fprintf(e, "exit 1\nverdict incomplete\n");
    fclose(e);
    run_free(&r);
}

static void t_sink_failures(void)
{
    sink_fail_at(13, 2, "t-s1-sink-fails-second-events");
    sink_fail_at(15, 2, "t-s2-sink-fails-second-loss");
    sink_fail_at(10, 2, "t-s3-sink-fails-second-calibration");
    sink_fail_at(16, 1, "t-s4-sink-fails-final");
}

/* item 3: descriptor strings are owned copies; the caller's strings are changed and freed before the drain */
static void t_image_owned_strings(void)
{
    run r;
    lane_h *h;
    FILE *e;
    uint32_t idx;
    psp_desc d[2];
    char *n0 = strdup("heap_zone_0"), *n1 = strdup("heap_zone_1"), *f0 = strdup("heap/file.c");
    run_init(&r, 8, 4, NULL);
    d[0] = (psp_desc){0x200, 20, n0, f0};
    d[1] = (psp_desc){0x210, 21, n1, NULL};
    want("owned", "image add", psp_image_add(&r.p, (const uint8_t *)"\x01heap", 5, d, 2, &idx), PSP_P_OK);
    memset(n0, 'X', strlen(n0)); /* the caller reuses its buffers ... */
    free(n0);                    /* ... and frees them: a borrowed pointer would now be a use after free */
    free(n1);
    free(f0);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    {
        int found = 0;
        for (size_t i = 0; i < r.nch; i++)
            if (r.ch[i].type == 11 && memmem(r.ch[i].data, r.ch[i].len, "heap_zone_0", 11) &&
                memmem(r.ch[i].data, r.ch[i].len, "heap_zone_1", 11) && memmem(r.ch[i].data, r.ch[i].len, "heap/file.c", 11))
                found = 1;
        want("owned", "the registered strings were emitted", found, 1);
    }
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-image-owned-strings");
    fprintf(e, "exit 0\nverdict complete\n");
    lane_line(e, h, "COMPLETE 1 1 2 0");
    fclose(e);
    run_free(&r);
}

/* the published-state consistency checks */
static void t_published_state(void)
{
    static const char *names[3] = {"t-d6c-published-closed-fault", "t-d6d-published-gen0-started",
                                   "t-d6e-published-none-with-gen"};
    for (int v = 0; v < 3; v++) {
        run r;
        lane_h *h;
        FILE *e;
        uint8_t *b;
        run_init(&r, 8, 4, NULL);
        h = lane_new(&r, 4, 0, 1);
        zones(h, 1);
        flush(h);
        if (v == 0)
            fini(h);
        b = g_mem + r.p.lanes[h->idx].ea_ctl;
        if (v == 0)
            psp_be32(b + PSP_B_FLAGS, psp_rd32(b + PSP_B_FLAGS) | PSP_F_FAULT); /* CLOSED + FAULT: forbidden */
        else if (v == 1) {
            psp_be64(b + PSP_B_GEN, 0); /* gen 0 with STARTED data */
            psp_be64(b + PSP_B_GEN_END, 0);
        } else
            psp_be32(b + PSP_B_STATE, PSP_ST_NONE); /* NONE with a generation */
        psp_drain(&r.p);
        want(names[v], "corrupt", r.p.corrupt, 1);
        psp_lane_end(&r.p, h->idx);
        e = done(&r, names[v]);
        fprintf(e, "exit 1\nverdict incomplete\n");
        fclose(e);
        run_free(&r);
    }
}

/* ---- checked counters ---- */

static void t_k1_suppressed(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    h->s.depth = 64;
    h->s.suppressed = UINT32_MAX; /* injected at the counter's bound */
    zb(h, 0);
    want("k1", "fault", (h->s.flags & PSP_F_FAULT) != 0, 1);
    want("k1", "suppressed did not wrap", (long long)h->s.suppressed, UINT32_MAX);
    ze(h, 0);
    want("k1", "depth kept", h->s.depth, 64);
    want("k1", "flush", flush(h), PSP_EFAULT);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-k1-suppressed-bound");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 2 0");
    fclose(e);
    run_free(&r);
}

static void t_k2_refused(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    h->s.refused = UINT64_MAX;
    h->s.depth = 64;
    zb(h, 0);
    want("k2", "fault", (h->s.flags & PSP_F_FAULT) != 0, 1);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-k2-refused-bound");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 2 0 0");
    fclose(e);
    run_free(&r);
}

static void t_k3_seq(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 8, 4, NULL);
    h = lane_new(&r, 4, 0, 1);
    inject_older(h, PSP_SEQ_MAX - 1);
    zb(h, 0); /* seq 2^62 - 1: the last number */
    zb(h, 0); /* refused: terminal at the bound */
    want("k3", "seq", (long long)(h->s.seq == PSP_SEQ_MAX), 1);
    flush(h); /* four front markers only */
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-k3-seq-bound");
    fprintf(e, "exit 4\nverdict complete\n");
    lane_line(e, h, "INCOMPLETE 0 3 0 4611686018427387904");
    /* four front markers per flush: the first drain, fini's flush, then the trailing range from CLOSED */
    fprintf(e, "losses %u 0:17179869180 17179869180:17179869180 34359738360:4611685984067649544\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

/* ---- the depth-zero epoch rule and the dec_write depth refusal (clock-mapping plan B r2 5.4, 5.5) ----
 * Runtime side only: each trace asserts the FINAL's open_begins field read from the accepted chunk bytes and the
 * drain's lane history; check_streams also rebuilds the expected field from the accepted chunks on every trace.  The
 * reader's verdict on the same files is the driver's separate check (exit code and the "open" directive). */

static void want_open(const char *trace, run *r, lane_h *h, long long exp)
{
    uint32_t open = 0;
    want(trace, "FINAL accepted", final_open(r, h->lane_id, &open), 1);
    want(trace, "FINAL open_begins", open, exp);
}

/* a decrementer write between balanced zones: the history stays known, the FINAL states the true count 0 */
static void t_b1_depth0(void)
{
    run r;
    lane_h *h;
    FILE *e;
    psp_lane *pl;
    run_init(&r, 16, 4, NULL);
    h = lane_new(&r, 8, 1, 1);
    pl = &r.p.lanes[h->idx];
    zones(h, 2);
    want("b1-depth0", "flush", flush(h), PSP_OK);
    psp_drain(&r.p); /* EPOCH 1 and the four events */
    want("b1-depth0", "dec_write", decw(h, 0x40000000u), PSP_OK);
    psp_drain(&r.p); /* EPOCH 2 */
    want("b1-depth0", "epochs", pl->epoch, 2);
    want("b1-depth0", "history known after EPOCH 2", pl->known && !pl->ever_unknown && !pl->protocol, 1);
    zones(h, 1);
    want("b1-depth0", "fini", fini(h), PSP_OK);
    want("b1-depth0", "end", psp_lane_end(&r.p, h->idx), PSP_P_OK);
    want_open("b1-depth0", &r, h, 0);
    want("b1-depth0", "decrementer writes", h->pc.nwr, 1);
    e = done(&r, "t-b1-depth0");
    fprintf(e, "exit 0\nverdict complete\nepochs 2\n");
    lane_line(e, h, "COMPLETE 1 1 6 0");
    fprintf(e, "open %u verified 0\nlosses %u\n", h->lane_id, h->lane_id);
    fclose(e);
    run_free(&r);
}

/* the same, with a zone begun after the write and still open at fini: the FINAL states 1, not unknown */
static void t_b1_depth0_open(void)
{
    run r;
    lane_h *h;
    FILE *e;
    run_init(&r, 16, 4, NULL);
    h = lane_new(&r, 8, 1, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    want("b1-depth0-open", "dec_write", decw(h, 0x40000000u), PSP_OK);
    psp_drain(&r.p);
    zb(h, 1); /* never ended */
    want("b1-depth0-open", "fini", fini(h), PSP_OK);
    psp_lane_end(&r.p, h->idx);
    want_open("b1-depth0-open", &r, h, 1);
    e = done(&r, "t-b1-depth0-open");
    fprintf(e, "exit 0\nverdict complete\nepochs 2\n");
    lane_line(e, h, "COMPLETE 1 1 3 0");
    fprintf(e, "open %u verified 1\nlosses %u\n", h->lane_id, h->lane_id);
    fclose(e);
    run_free(&r);
}

/* the pending-event replay at the boundary: the END that empties the stack is published by dec_write's own flush and
 * reaches the drain in the SAME walk as the EPOCH; it is replayed before the EPOCH is judged, so the depth is 0 */
static void t_b1_pending_end(void)
{
    run r;
    lane_h *h;
    FILE *e;
    psp_lane *pl;
    run_init(&r, 16, 4, NULL);
    h = lane_new(&r, 8, 1, 1);
    pl = &r.p.lanes[h->idx];
    zb(h, 0);
    flush(h);
    psp_drain(&r.p); /* EPOCH 1, the BEGIN: the drain's stack holds one zone */
    want("b1-pending-end", "drain depth before", pl->sp, 1);
    ze(h, 0);        /* staged, not published */
    want("b1-pending-end", "dec_write", decw(h, 0x40000000u), PSP_OK); /* flush [END], then the EPOCH publication */
    psp_drain(&r.p); /* one walk: END, EPOCH, PAD */
    want("b1-pending-end", "epochs", pl->epoch, 2);
    want("b1-pending-end", "history known after EPOCH 2", pl->known && !pl->ever_unknown && !pl->protocol, 1);
    want("b1-pending-end", "drain depth after", pl->sp, 0);
    zones(h, 1);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    want_open("b1-pending-end", &r, h, 0);
    e = done(&r, "t-b1-pending-end");
    fprintf(e, "exit 0\nverdict complete\nepochs 2\n");
    lane_line(e, h, "COMPLETE 1 1 4 0");
    fprintf(e, "open %u verified 0\nlosses %u\n", h->lane_id, h->lane_id);
    fclose(e);
    run_free(&r);
}

/* a LOSS, then a depth-0 resynchronisation, then a depth-0 write: the history was unknown once, so it stays
 * unknown and the FINAL keeps the UINT32_MAX placeholder (the rule never rewrites an unknown history) */
static void t_b1_loss_then_depth0(void)
{
    run r;
    lane_h *h;
    FILE *e;
    psp_lane *pl;
    run_init(&r, 16, 4, NULL);
    h = lane_new(&r, 4, 1, 1);
    pl = &r.p.lanes[h->idx];
    zones(h, 3); /* staging 4: B E B E staged, the third zone's B E are tail losses */
    flush(h);
    psp_drain(&r.p); /* the events and LOSS 4+2 */
    want("b1-loss", "ever unknown after the LOSS", pl->ever_unknown, 1);
    zones(h, 1); /* a depth-0 BEGIN resynchronises: known again, stack empty, history not known from the start */
    flush(h);
    psp_drain(&r.p);
    want("b1-loss", "resynchronised", pl->known && pl->sp == 0, 1);
    want("b1-loss", "dec_write", decw(h, 0x40000000u), PSP_OK);
    psp_drain(&r.p);
    want("b1-loss", "epochs", pl->epoch, 2);
    want("b1-loss", "unknown after EPOCH 2", pl->known, 0);
    zones(h, 1);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    want_open("b1-loss", &r, h, UINT32_MAX);
    e = done(&r, "t-b1-loss-then-depth0");
    fprintf(e, "exit 4\nverdict complete\nepochs 2\n");
    lane_line(e, h, "COMPLETE 0 1 8 2");
    fprintf(e, "open %u unverifiable 4294967295\nlosses %u 4:2\n", h->lane_id, h->lane_id);
    fclose(e);
    run_free(&r);
}

/* everything a refused dec_write could touch, captured before the call and compared after it */
typedef struct side {
    psp_spu s;
    port_ctx pc;
    uint8_t *mem;
    size_t ring_bytes;
    uint8_t ctl[256];
    uint8_t *stage;
    size_t stage_bytes;
} side;

static void side_take(side *x, run *r, lane_h *h)
{
    psp_lane *pl = &r->p.lanes[h->idx];
    memcpy(&x->s, &h->s, sizeof x->s);
    memcpy(&x->pc, &h->pc, sizeof x->pc);
    x->ring_bytes = (size_t)r->p.cfg.ring_slots * PSP_SLOT;
    x->mem = malloc(x->ring_bytes);
    memcpy(x->mem, g_mem + pl->ea_ring, x->ring_bytes);
    memcpy(x->ctl, g_mem + pl->ea_ctl, sizeof x->ctl);
    x->stage_bytes = ((size_t)h->s.ls_cap + 2 * PSP_HEAD) * PSP_SLOT;
    x->stage = malloc(x->stage_bytes);
    memcpy(x->stage, h->buf, x->stage_bytes);
}

static void side_same(const char *trace, side *x, run *r, lane_h *h)
{
    psp_lane *pl = &r->p.lanes[h->idx];
    want(trace, "writer state unchanged", memcmp(&x->s, &h->s, sizeof x->s), 0);
    want(trace, "port: no channel command, DMA, decrementer read or write", memcmp(&x->pc, &h->pc, sizeof x->pc), 0);
    want(trace, "ring unchanged", memcmp(x->mem, g_mem + pl->ea_ring, x->ring_bytes), 0);
    want(trace, "control lines unchanged", memcmp(x->ctl, g_mem + pl->ea_ctl, sizeof x->ctl), 0);
    want(trace, "staging unchanged", memcmp(x->stage, h->buf, x->stage_bytes), 0);
    free(x->mem);
    free(x->stage);
}

/* dec_write and its preflight inside a zone: EDEPTH before the initial flush, nothing flushed, written or published;
 * flushing does not clear it; after the zone ends the same call behaves as before (and the history stays known) */
static void t_b1_edepth(void)
{
    run r;
    lane_h *h;
    FILE *e;
    side x;
    run_init(&r, 16, 4, NULL);
    h = lane_new(&r, 8, 1, 1);
    zones(h, 1);
    flush(h);
    psp_drain(&r.p);
    zb(h, 1); /* depth 1, staged: a flush would publish it */
    side_take(&x, &r, h);
    want("b1-edepth", "dec_write at depth 1", decw(h, 0x40000000u), PSP_EDEPTH);
    side_same("b1-edepth", &x, &r, h);
    want("b1-edepth", "staged event not flushed", h->s.n, 1);
    side_take(&x, &r, h);
    want("b1-edepth", "preflight at depth 1", psp_spu_dec_preflight(&h->s), PSP_EDEPTH);
    side_same("b1-edepth preflight", &x, &r, h);
    want("b1-edepth", "flush", flush(h), PSP_OK);
    psp_drain(&r.p);
    want("b1-edepth", "dec_write after a flush, still at depth 1", decw(h, 0x40000000u), PSP_EDEPTH);
    want("b1-edepth", "no decrementer write", h->pc.nwr, 0);
    ze(h, 1);
    want("b1-edepth", "dec_write at depth 0", decw(h, 0x40000000u), PSP_OK);
    want("b1-edepth", "one decrementer write", h->pc.nwr, 1);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    want_open("b1-edepth", &r, h, 0);
    e = done(&r, "t-b1-edepth");
    fprintf(e, "exit 0\nverdict complete\nepochs 2\n");
    lane_line(e, h, "COMPLETE 1 1 4 0");
    fprintf(e, "open %u verified 0\nlosses %u\n", h->lane_id, h->lane_id);
    fclose(e);
    run_free(&r);
}

/* suppressed depth: 64 open zones and one refused BEGIN (EDEPTH, no side effects); then the suppressed term alone,
 * injected at depth 0 (not reachable through the zone calls, which suppress only at depth 64) */
static void t_b1_edepth_suppressed(void)
{
    run r;
    lane_h *h;
    FILE *e;
    side x;
    run_init(&r, 256, 4, NULL);
    h = lane_new(&r, 160, 1, 1);
    for (int i = 0; i < 64; i++)
        zb(h, 0);
    zb(h, 1); /* suppressed */
    want("b1-suppressed", "suppressed", h->s.suppressed, 1);
    side_take(&x, &r, h);
    want("b1-suppressed", "dec_write at depth 64 + 1 suppressed", decw(h, 7), PSP_EDEPTH);
    side_same("b1-suppressed", &x, &r, h);
    ze(h, 1); /* pops the suppressed BEGIN only */
    want("b1-suppressed", "dec_write at depth 64", decw(h, 7), PSP_EDEPTH);
    for (int i = 0; i < 64; i++)
        ze(h, 0);
    h->s.suppressed = 1; /* injected: the suppressed term alone */
    side_take(&x, &r, h);
    want("b1-suppressed", "dec_write at depth 0 with suppressed 1", decw(h, 7), PSP_EDEPTH);
    side_same("b1-suppressed injected", &x, &r, h);
    h->s.suppressed = 0;
    want("b1-suppressed", "no decrementer write", h->pc.nwr, 0);
    flush(h);
    psp_drain(&r.p);
    fini(h);
    psp_lane_end(&r.p, h->idx);
    e = done(&r, "t-b1-edepth-suppressed");
    fprintf(e, "exit 4\nverdict complete\nepochs 1\n");
    lane_line(e, h, "INCOMPLETE 0 3 128 0"); /* the refused BEGIN latched OVERFLOWED, as in t-depth */
    fprintf(e, "losses %u\n", h->lane_id);
    fclose(e);
    run_free(&r);
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "traces") && strcmp(argv[2], "sched"))) {
        fprintf(stderr, "usage: spu-model <out-dir> traces|sched\n");
        return 2;
    }
    g_dir = argv[1];
    if (!strcmp(argv[2], "sched"))
        return sched_main(g_dir);
    t_clean();
    t_full_ring();
    t_partial_even();
    t_odd_pad();
    t_loss_split();
    t_wrap();
    t_fini_pending();
    t_abandoned();
    t_epoch_race();
    t_dec_refused();
    t_depth();
    t_never_started();
    t_ebusy();
    t_a1();
    t_a2();
    t_a3();
    t_a4();
    t_a5();
    t_c1();
    c2(0);
    c2(1);
    t_c3();
    t_c4();
    t_c4_model_epoch();
    t_c5();
    t_d1_kill_after_write();
    t_d2_kill_before_publish();
    t_d3_fini_ebusy_retry();
    t_d4_fini_efault();
    t_d5_local_fault();
    t_d6_published_corruption();
    t_d7_last_generation();
    t_d7c_write_index();
    t_d8_bad_read_index();
    t_e1_timeout();
    t_e2_seal_races_end();
    t_e3_sink_failure();
    t_e4_finalized_live_and_cap();
    t_e5_post_seal_stats();
    t_k1_suppressed();
    t_k2_refused();
    t_k3_seq();
    t_sink_failures();
    t_image_owned_strings();
    t_published_state();
    t_b1_depth0();
    t_b1_depth0_open();
    t_b1_pending_end();
    t_b1_loss_then_depth0();
    t_b1_edepth();
    t_b1_edepth_suppressed();
    order_traces(g_dir, &g_traces); /* SPU_ORDER records (spu-ticks delta D3.7, r1 10.1) */
    if (g_order_faults)
        FAIL("%d ring loads without the post-acceptance barrier", g_order_faults);
    printf("spu-model traces=%d failures=%d\n", g_traces, g_failures);
    if (g_failures)
        return 1;
    printf("spu-model: PASS\n");
    return 0;
}
