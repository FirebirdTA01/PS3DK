/* overhead-bench host unit controls: the seeded schedule (A-2, D7), the ledger check, the teardown and ownership
 * rules (3.1e) with a failure at every step, and the RSX variant command-word accounting (3.1c).  Model evidence. */
#include <stdio.h>
#include <string.h>

#include "../common/ob_cases.h"
#include "../common/ob_rsx.h"
#include "../common/ob_sched.h"
#include "../common/ob_teardown.h"

static unsigned tests, failures;
static void check(int yes, const char *name)
{
    ++tests;
    if (!yes) {
        ++failures;
        printf("FAIL %s\n", name);
    }
}

static void schedule(void)
{
    static ob_block a[16384], b[16384];
    const uint16_t cases[] = {OB_P_HOT, OB_P_COLD, OB_S_PAIR};
    const int cold[] = {0, 1, 0};
    long na = ob_schedule(7, cases, cold, 3, OB_R, a, 16384), nb = ob_schedule(7, cases, cold, 3, OB_R, b, 16384);
    check(na == 2 * 3 * (OB_R + 1) + 3 * 64 && na == nb, "size: per (case, TU) 1 warmup + R timed; P-cold 64 sites x 3");
    check(memcmp(a, b, (size_t)na * sizeof a[0]) == 0 && ob_sched_digest(a, na) == ob_sched_digest(b, nb),
          "the same seed gives the same order (and digest)");
    ob_schedule(8, cases, cold, 3, OB_R, b, 16384);
    check(ob_sched_digest(a, na) != ob_sched_digest(b, nb), "another seed gives another order");
    unsigned long warm = 0, timed[3][3] = {{0}}, coldx[3] = {0}, cold_warm = 0, switches = 0, first_timed = 0;
    unsigned char site_seen[3][64] = {{0}};
    int dup = 0, warm_after = 0;
    for (long i = 0; i < na; ++i) {
        unsigned ci = a[i].c == OB_P_HOT ? 0 : a[i].c == OB_P_COLD ? 1 : 2;
        if (a[i].kind == OB_BLK_WARM) {
            ++warm;
            cold_warm += ci == 1;
            if (first_timed)
                warm_after = 1;
        } else {
            if (!first_timed)
                first_timed = (unsigned long)i + 1;
            if (a[i].kind == OB_BLK_COLD) {
                ++coldx[a[i].tu];
                dup |= site_seen[a[i].tu][a[i].rep]++;
            } else
                ++timed[ci][a[i].tu];
            if (i && a[i - 1].kind != OB_BLK_WARM && a[i - 1].tu != a[i].tu)
                ++switches;
        }
    }
    check(warm == 6 && cold_warm == 0 && !warm_after, "warmups: one per (case, TU), none for P-cold, all first");
    check(timed[0][0] == OB_R && timed[0][2] == OB_R && timed[2][1] == OB_R, "R timed blocks per (case, TU)");
    check(coldx[0] == 64 && coldx[1] == 64 && coldx[2] == 64 && !dup, "P-cold: 64 distinct sites per TU, each once");
    check(switches > (unsigned long)na / 3, "the TUs of a case are interleaved (many TU switches between blocks)");
    check(ob_schedule(7, cases, cold, 3, OB_R, a, 100) == -1, "a too small output is refused, nothing partial");
}

static void gpu_schedule(void)
{
    static ob_block a[2048], b[2048];
    long na = ob_gpu_schedule(7, OB_R, a, 2048), nb = ob_gpu_schedule(7, OB_R, b, 2048);
    check(na == 1 + 5 * (long)OB_R && na == nb && memcmp(a, b, (size_t)na * sizeof a[0]) == 0,
          "gpu: 1 warmup + 5 x R timed, the same seed gives the same order");
    check(a[0].c == OB_G_G1 && a[0].kind == OB_BLK_WARM, "gpu: the G-g1 warmup comes first");
    unsigned cnt[OB_G_CASES] = {0}, bad = 0, switches = 0;
    for (long i = 1; i < na; ++i) {
        bad += a[i].kind != OB_BLK_TIMED || a[i].c >= OB_G_CASES || a[i].tu != 0;
        if (a[i].c < OB_G_CASES)
            ++cnt[a[i].c];
        switches += i > 1 && a[i].c != a[i - 1].c;
    }
    check(!bad && cnt[0] == OB_R && cnt[1] == OB_R && cnt[2] == OB_R && cnt[3] == OB_R && cnt[4] == OB_R,
          "gpu: R timed blocks of each G case, nothing else");
    check(switches > (unsigned)na / 2, "gpu: the G cases are interleaved");
    ob_gpu_schedule(8, OB_R, b, 2048);
    check(ob_sched_digest(a, na) != ob_sched_digest(b, na), "gpu: another seed gives another order");
    check(ob_gpu_schedule(7, OB_R, a, 100) == -1, "gpu: a too small output is refused");
}

static void ledgers(void)
{
    char why[160];
    for (int c = 0; c < OB_CASES; ++c) {
        ob_ledger p = ob_planned(c);
        check(ob_ledger_check(c, &p, why, sizeof why) == 0, "the plan matches itself");
    }
    ob_ledger p = ob_planned(OB_P_DROP);
    check(p.seq == 768 && p.dropped == 768, "P-drop: produced 0 (seq 3n all dropped), dropped 3n");
    p.dropped -= 1;
    check(ob_ledger_check(OB_P_DROP, &p, why, sizeof why) == -1 && strstr(why, "dropped"), "a ledger one short fails");
    p = ob_planned(OB_S_STAGEFULL);
    check(p.sseq == 128 && p.tail_lost == 128 && p.staged == 0, "S-stagefull: seq +2n, tail_lost +2n, staged unchanged");
    p = ob_planned(OB_S_BACKPRESS);
    check(p.published == 0, "S-backpressure: published 0");
    p = ob_planned(OB_P_COLD);
    check(p.seq == 3 && p.regs == 1, "P-cold: one pair (3 slots) per site, one registration per site");
}

typedef struct tdm {
    int fail_at, calls[5];
    char log[256];
} tdm;
static int t_step(tdm *m, int k) { ++m->calls[k]; return m->fail_at == k + 1 ? -1 : 0; }
static int t_lw(void *c, uint32_t v) { (void)v; return t_step(c, 0); }
static int t_det(void *c) { return t_step(c, 1); }
static int t_rel(void *c) { return t_step(c, 2); }
static int t_join(void *c) { return t_step(c, 3); }
static int t_le(void *c) { return t_step(c, 4); }
static void t_log(void *c, const char *w, int rc) { (void)rc; snprintf(((tdm *)c)->log, 256, "%s", w); }

static void teardown(void)
{
    for (int f = 0; f <= 3; ++f) {
        tdm m;
        memset(&m, 0, sizeof m);
        m.fail_at = f;
        ob_td_port p = {&m, t_lw, t_det, t_rel, t_join, t_le, t_log};
        int r = ob_td_gpu(&p, 77);
        if (f == 0)
            check(r == OB_TD_RELEASED && m.calls[0] == 1 && m.calls[1] == 1 && m.calls[2] == 1, "gpu: wait, detach, release");
        else if (f == 1)
            check(r == OB_TD_RETAINED && m.calls[1] == 0 && m.calls[2] == 0 && strstr(m.log, "retained"),
                  "gpu: a failed label wait retains (no detach, no release), logged");
        else if (f == 2)
            check(r == OB_TD_RETAINED && m.calls[2] == 0, "gpu: a failed detach retains (no release)");
        else
            check(r == OB_TD_RETAINED && m.calls[2] == 1 && strstr(m.log, "release"), "gpu: a failed release is logged");
    }
    for (int f = 0; f <= 2; ++f) {
        tdm m;
        memset(&m, 0, sizeof m);
        m.fail_at = f == 0 ? 0 : f + 3;
        ob_td_port p = {&m, t_lw, t_det, t_rel, t_join, t_le, t_log};
        int r = ob_td_spu(&p);
        if (f == 0)
            check(r == OB_TD_RELEASED && m.calls[3] == 1 && m.calls[4] == 1, "spu: join, then lane_end");
        else if (f == 1)
            check(r == OB_TD_RETAINED && m.calls[4] == 0 && strstr(m.log, "no lane_end"), "spu: a failed join retains");
        else
            check(r == OB_TD_RETAINED && strstr(m.log, "lane_end failed"), "spu: a failed lane_end is logged");
    }
    char why[160];
    ob_owner o = {16, 1024, 240, 8, 2, 241, 3, 2};
    check(ob_owner_check(&o, why, sizeof why) == 0, "ownership: disjoint reports, labels and tags");
    ob_owner o1 = o;
    o1.bench_report_first = 1039;
    check(ob_owner_check(&o1, why, sizeof why) == -1 && strstr(why, "overlap"), "ownership: report overlap refused");
    ob_owner o2 = o;
    o2.bench_label = 240;
    check(ob_owner_check(&o2, why, sizeof why) == -1 && strstr(why, "label"), "ownership: shared label refused");
    ob_owner o3 = o;
    o3.bench_tag = 3;
    check(ob_owner_check(&o3, why, sizeof why) == -1 && strstr(why, "tag"), "ownership: shared DMA tag refused");
}

typedef struct rm {
    unsigned long words, zones, nops, draws, reports, labels, flushes;
    char seq[1024];
    unsigned ns;
} rm;
static void r_add(rm *m, char k, unsigned long w) { m->words += w; if (m->ns < 1023) m->seq[m->ns++] = k; }
static void r_rep(void *c, uint32_t i) { (void)i; ++((rm *)c)->reports; r_add(c, 'R', 4); }
static void r_lab(void *c, uint32_t v) { (void)v; ++((rm *)c)->labels; r_add(c, 'L', 2); }
static void r_draw(void *c, uint32_t i) { (void)i; ++((rm *)c)->draws; r_add(c, 'D', 10); }
static int r_zb(void *c, uint32_t i) { (void)i; ++((rm *)c)->zones; r_add(c, 'b', OB_ZONE_ENDPOINT_WORDS); return 0; }
static int r_ze(void *c, uint32_t i) { (void)i; r_add(c, 'e', OB_ZONE_ENDPOINT_WORDS); return 0; }
static void r_nop(void *c, uint32_t w) { ++((rm *)c)->nops; r_add(c, 'n', w); }
static void r_fl(void *c) { ++((rm *)c)->flushes; r_add(c, 'F', 0); }

static void rsx(void)
{
    rm g[3];
    for (int v = 0; v < 3; ++v) {
        memset(&g[v], 0, sizeof g[v]);
        ob_rsx_port p = {&g[v], r_rep, r_lab, r_draw, r_zb, r_ze, r_nop, r_fl};
        ob_rsx_variant(&p, v, 8, 100 + (uint32_t)v);
    }
    check(g[0].draws == 64 && g[1].draws == 64 && g[2].draws == 64, "64 draws in every variant");
    check(g[1].zones == 64 && (g[1].words - g[0].words) * 4 == 64ul * OB_ZONE_BYTES,
          "g1: 64 zones, the command-buffer delta exactly 64 x 48 bytes");
    check(g[2].words == g[1].words && g[2].zones == 0 && g[2].nops == 128, "g2: the same word count as g1, no zone");
    check(g[0].reports == 2 && g[0].labels == 1 && g[0].flushes == 1 && g[0].seq[0] == 'R' &&
              strncmp(g[0].seq + g[0].ns - 3, "RLF", 3) == 0,
          "each variant bracketed by the bench's own reports, then its label, then a flush");
    check(strncmp(g[1].seq, "RbDe", 4) == 0, "g1: zone begin, draw, zone end per draw");
}

int main(void)
{
    schedule();
    gpu_schedule();
    ledgers();
    teardown();
    rsx();
    printf("test_ob_units: %u checks, %u failures\n", tests, failures);
    return failures ? 1 : 0;
}
