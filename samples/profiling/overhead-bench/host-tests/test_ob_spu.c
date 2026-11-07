/* overhead-bench host ledger model, SPU cases (Plan A r2 4.2 A5; probe-design-r1 11.2 A-3): the bench's own SPU case
 * functions (spu/bench_spu.inc, TU "on") run against the REAL SPU writer core (psp_spu.c) inside the repository's
 * transport model (tests/host/ps3dk-prof-spu/model.c), one lane, staging ls_cap OB_LS_CAP, its decrementer written
 * once before the blocks.  Ledger fields read from the writer state: seq (sequence numbers assigned), tail_lost,
 * staged (n) and, for flushes, published = the staged events the flush cleared while it reported success.
 *   S-pair       staging 0 before each block (a drained flush first): n pairs -> seq +2n, staged +2n
 *   S-flush-k    staging k (marks, untimed), ring drained: one flush -> published k, staged -k
 *   S-stagefull  ring full (no PPU drain, flushes until the ring refuses) and staging at ls_cap: n pairs -> seq +2n,
 *                tail_lost +2n, staged unchanged (psp_spu.c record(): n == ls_cap takes a sequence, no slot write)
 *   S-backpressure  the same full ring: one flush -> published 0, its result printed and pinned
 * Model evidence only. */
#include <stdio.h>
#include <string.h>

#include "model.h"
#include "../common/ob_cases.h"

#define RING 1024u
static lane_h *g_h;
static uint32_t g_fake_dec = 0xFFFFFFFFu;
#define OB_TU on
#define OB_DEC() (g_fake_dec -= 7u)
#define OB_SB(name) ((void)sizeof(name), psp_spu_begin(&g_h->s, 1), 1u)
#define OB_SE(z) ((void)(z), psp_spu_end(&g_h->s, 1))
#define OB_SM(name) ((void)sizeof(name), psp_spu_mark(&g_h->s, 1))
#define OB_FLUSH() psp_spu_flush(&g_h->s)
#include "../spu/bench_spu.inc"

static unsigned tests, failures;
static void check(int yes, const char *name)
{
    ++tests;
    if (!yes) {
        ++failures;
        printf("FAIL %s\n", name);
    }
}

typedef struct snap {
    uint64_t seq, tail, older, w;
    uint32_t n;
} snap;
static snap take(void) { snap s = {g_h->s.seq, g_h->s.tail_lost, g_h->s.older_lost, g_h->s.w, g_h->s.n}; return s; }

static int drained_flush(run *r)
{
    int rc = psp_spu_flush(&g_h->s);
    psp_drain(&r->p);
    return rc;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    run r;
    run_init(&r, RING, 4, NULL); /* large enough for a 128-event flush; filled on purpose for the full-ring cases */
    g_h = lane_new(&r, OB_LS_CAP, 1, 1);
    check(g_h && psp_spu_dec_write(&g_h->s, 0xFFFFFFFFu) == PSP_OK, "lane attached, decrementer written once");
    psp_drain(&r.p);
    char why[160];
    ob_d2 t;
    /* S-pair: R blocks */
    unsigned bad = 0;
    for (unsigned b = 0; b < 20; ++b) {
        drained_flush(&r);
        snap a = take();
        bench_spu_on_pairs(ob_case_table[OB_S_PAIR].n, &t);
        snap z = take();
        ob_ledger got = {0};
        got.sseq = z.seq - a.seq, got.tail_lost = z.tail - a.tail, got.staged = (int64_t)z.n - (int64_t)a.n;
        if (ob_ledger_check(OB_S_PAIR, &got, why, sizeof why))
            ++bad, printf("  %s\n", why);
    }
    printf("  ledger S-pair 20 blocks %u mismatches\n", bad);
    check(bad == 0, "S-pair ledger exact");
    /* S-flush-k */
    const int fk[4] = {OB_S_FLUSH0, OB_S_FLUSH1, OB_S_FLUSH16, OB_S_FLUSH128};
    for (unsigned i = 0; i < 4; ++i) {
        bad = 0;
        for (unsigned b = 0; b < 20; ++b) {
            drained_flush(&r);
            bench_spu_on_stage(ob_case_table[fk[i]].n);
            snap a = take();
            int rc = bench_spu_on_flush(&t);
            snap z = take();
            psp_drain(&r.p);
            ob_ledger got = {0};
            got.sseq = z.seq - a.seq, got.tail_lost = z.tail - a.tail, got.staged = (int64_t)z.n - (int64_t)a.n;
            got.published = rc == PSP_OK ? (uint64_t)(a.n - z.n) : 0;
            if (b == 0)
                printf("  %s: ring slots written %llu, older_lost %llu -> %llu\n", ob_case_table[fk[i]].name,
                       (unsigned long long)(z.w - a.w), (unsigned long long)a.older, (unsigned long long)z.older);
            got.lost = z.older - a.older;
            if (rc != PSP_OK || ob_ledger_check(fk[i], &got, why, sizeof why))
                ++bad, printf("  %s rc %d\n", why, rc);
        }
        printf("  ledger %s 20 blocks %u mismatches\n", ob_case_table[fk[i]].name, bad);
        check(bad == 0, "S-flush-k ledger exact (published k)");
    }
    /* the full ring: flush without draining until the writer reports the ring cannot take more */
    drained_flush(&r);
    int rc = PSP_OK;
    unsigned fills = 0;
    for (; fills < 1000 && g_h->s.w - g_h->s.r_seen < RING; ++fills) {
        bench_spu_on_stage(16);
        rc = psp_spu_flush(&g_h->s);
    }
    printf("  ring fill: %u flushes, then rc %d with %u staged, ring w-r_seen %llu of %u\n", fills, rc, g_h->s.n,
           (unsigned long long)(g_h->s.w - g_h->s.r_seen), RING);
    check(fills < 1000 && g_h->s.w - g_h->s.r_seen == RING, "the ring fills without a PPU drain");
    /* S-backpressure: one flush on the full ring */
    bench_spu_on_stage(ob_case_table[OB_S_BACKPRESS].n); /* events waiting when the flush meets the full ring */
    snap a = take();
    int brc = bench_spu_on_flush(&t);
    snap z = take();
    printf("  S-backpressure: flush rc %d (PSP_OK=%d PSP_EAGAIN=%d PSP_EBUSY=%d), staged %u -> %u, tail_lost %llu -> "
           "%llu, older_lost %llu -> %llu, ring slots written %llu\n",
           brc, PSP_OK, PSP_EAGAIN, PSP_EBUSY, a.n, z.n, (unsigned long long)a.tail, (unsigned long long)z.tail,
           (unsigned long long)a.older, (unsigned long long)z.older, (unsigned long long)(z.w - a.w));
    ob_ledger got = {0};
    got.published = z.w - a.w; /* on the full ring no slot can be written: nothing published */
    got.sseq = z.seq - a.seq;
    got.staged = (int64_t)z.n - (int64_t)a.n;
    got.lost = z.older - a.older;
    got.tail_lost = z.tail - a.tail;
    check(brc == PSP_OK && ob_ledger_check(OB_S_BACKPRESS, &got, why, sizeof why) == 0,
          "S-backpressure: rc OK, published 0, the staged events become losses");
    /* S-stagefull: staging at ls_cap on the full ring, then n pairs */
    bench_spu_on_stage(OB_LS_CAP - g_h->s.n);
    check(g_h->s.n == OB_LS_CAP, "staging filled to ls_cap");
    bad = 0;
    for (unsigned b = 0; b < 20; ++b) {
        snap a2 = take();
        bench_spu_on_pairs(ob_case_table[OB_S_STAGEFULL].n, &t);
        snap z2 = take();
        ob_ledger g2 = {0};
        g2.sseq = z2.seq - a2.seq, g2.tail_lost = z2.tail - a2.tail, g2.staged = (int64_t)z2.n - (int64_t)a2.n;
        if (ob_ledger_check(OB_S_STAGEFULL, &g2, why, sizeof why))
            ++bad, printf("  %s\n", why);
    }
    printf("  ledger S-stagefull 20 blocks %u mismatches\n", bad);
    check(bad == 0, "S-stagefull ledger exact (seq +2n, tail_lost +2n, staged unchanged)");
    check(g_failures == 0, "transport model: no MFC rule violation");
    run_free(&r);
    printf("test_ob_spu: %u checks, %u failures\n", tests, failures);
    return failures ? 1 : 0;
}
