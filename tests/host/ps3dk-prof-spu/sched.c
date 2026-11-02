/* The adversarial scheduler of design p3.2 r3 F, as corrected by codex 01:35:
 *  - STRONG model (assumption: a putlluc update is indivisible and coherent as seen by PPU loads): every
 *    interleaving of the producer's shared steps with the drainer's steps, exhaustively for a small configuration;
 *    every accepted snapshot must be a published version; no unread slot is ever overwritten; the emitted streams
 *    must match the producer's truth (the stream check) and FINAL = emitted.
 *  - WEAK model (line B as independently visible groups {gen} {fields} {gen_end}): the gen / gen_end / first-word
 *    recheck does NOT reject every torn snapshot.  The search must FIND an accepted mixed snapshot and keep it.
 *  - seeded random walks of a larger configuration in the strong model.
 * These are properties of the models and their premises, not target evidence. */
#include <stdlib.h>
#include <string.h>

#include "model.h"

static const char *g_dir;
static long g_samples;

/* ---- the producer program: ops expand into shared steps; local records run inside the next shared step ---- */
typedef struct prog {
    const char *ops; /* digits: that many events staged; F flush; X fini */
    int pc, sub;
} prog;

static void records(lane_h *h, prog *p)
{
    while (p->ops[p->pc] >= '1' && p->ops[p->pc] <= '9') {
        for (int i = 0; i < p->ops[p->pc] - '0'; i++) {
            if (h->s.depth & 1)
                ze(h, 0);
            else
                zb(h, 0);
        }
        p->pc++;
    }
}

/* one shared producer step; returns 0 when the program has none left */
static int prog_step(lane_h *h, prog *p)
{
    char op;
    records(h, p);
    op = p->ops[p->pc];
    if (!op)
        return 0;
    switch (p->sub) {
    case 0:
        if (psp_spu_flush_plan(&h->s) != PSP_OK) { /* nothing more for this flush (EBUSY, ENOSPC, FAULT) */
            h->s.phase = PSP_FL_IDLE;
            p->sub = op == 'X' ? 3 : 0;
            if (op == 'F')
                p->pc++;
            return 1;
        }
        p->sub = 1;
        return 1;
    case 1:
        psp_spu_flush_put(&h->s);
        psp_spu_flush_wait(&h->s);
        p->sub = 2;
        return 1;
    case 2:
        psp_spu_flush_publish(&h->s);
        call_ok(h, "flush");
        if (op == 'F') {
            p->sub = 0;
            p->pc++;
        } else {
            p->sub = 3;
        }
        return 1;
    default:
        psp_spu_fini_close(&h->s);
        call_ok(h, "fini");
        p->sub = 0;
        p->pc++;
        return 1;
    }
}

static void prog_finish(lane_h *h, prog *p)
{
    while (prog_step(h, p))
        ;
    records(h, p);
}

/* one drainer step, with the strong-model snapshot invariant */
static int g_badsnap;
static void drain_step(run *r, lane_h *h, int strong)
{
    psp_lane *l = &r->p.lanes[h->idx];
    int before = l->dphase;
    psp_lane_step(&r->p, h->idx);
    if (before == 3 && l->dphase == 4 && strong && !versions_has(l->line)) {
        FAIL("strong model: an accepted snapshot is not a published version");
        g_badsnap++;
    }
}

static void sample(run *r, const char *kind, long i, lane_h *h)
{
    char name[64];
    FILE *e;
    snprintf(name, sizeof name, "sched-%s-%ld", kind, i);
    write_capture(r, g_dir, name);
    e = expect_open(g_dir, name);
    /* a SMOKE sample: the exit code is only checked to be 0 or 4; exact verdicts are the traces' job */
    fprintf(e, "smoke-exit 0 4\nverdict complete\nsources 1\ntruth %u\n", h->lane_id);
    fclose(e);
    g_samples++;
}

/* ---- exhaustive strong-model interleavings ---- */
static long strong_exhaustive(const char *ops, int spu_steps, int drain_steps, long sample_every)
{
    long count = 0;
    uint64_t total = (uint64_t)spu_steps + drain_steps, mask;
    /* every subset of size spu_steps of the total positions (Gosper's hack) */
    for (mask = (1ull << spu_steps) - 1; mask < (1ull << total);) {
        run r;
        lane_h *h;
        prog p = {ops, 0, 0};
        int fails = g_failures;
        g_weak = 0;
        run_init(&r, 8, 4, NULL);
        h = lane_new(&r, 4, 0, 1);
        psp_drain_declare(&r.p);
        for (uint64_t pos = 0; pos < total; pos++) {
            if (mask >> pos & 1)
                prog_step(h, &p);
            else
                drain_step(&r, h, 1);
        }
        prog_finish(h, &p);
        psp_lane_end(&r.p, h->idx);
        psp_seal(&r.p);
        check_streams(&r, "strong");
        if (g_failures != fails || (sample_every && count % sample_every == 0))
            sample(&r, g_failures != fails ? "strong-FAIL" : "strong", count, h);
        run_free(&r);
        count++;
        {
            uint64_t c = mask & -mask, rr = mask + c;
            mask = (((rr ^ mask) >> 2) / c) | rr;
        }
        if (g_failures > 20)
            break;
    }
    return count;
}

/* ---- the weak model: find an accepted snapshot that is not any published version ---- */
static long weak_search(long *found)
{
    static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    long runs = 0;
    *found = 0;
    for (int pi = 0; pi < 6; pi++) {
        /* interleave 3 memory applies with the drainer's 4 snapshot steps: positions of the applies among 7 */
        for (uint32_t mask = 0; mask < (1u << 7); mask++) {
            run r;
            lane_h *h;
            psp_lane *l;
            int applied = 0, drained = 0, accepted = 0;
            uint8_t seen[128];
            if (__builtin_popcount(mask) != 3)
                continue;
            g_weak = 1;
            run_init(&r, 8, 4, NULL);
            h = lane_new(&r, 4, 0, 1);
            mem_apply_all(); /* the init publication is complete */
            psp_drain(&r.p);
            l = &r.p.lanes[h->idx];
            zb(h, 0);
            ze(h, 0);
            psp_spu_flush_plan(&h->s);
            psp_spu_flush_put(&h->s);
            psp_spu_flush_wait(&h->s);
            psp_spu_flush_publish(&h->s); /* three queued groups */
            for (int pos = 0; pos < 7; pos++) {
                if (mask >> pos & 1) {
                    /* apply original group perm[applied]; the queue keeps original order, so its index is the
                     * number of not-yet-applied groups with a smaller original index */
                    int want = perms[pi][applied], before = 0;
                    for (int k = 0; k < want; k++) {
                        int done = 0;
                        for (int a = 0; a < applied; a++)
                            if (perms[pi][a] == k)
                                done = 1;
                        before += !done;
                    }
                    applied++;
                    mem_apply(before);
                } else if (drained < 4) {
                    int before = l->dphase;
                    psp_lane_step(&r.p, h->idx);
                    drained++;
                    if (before == 3 && l->dphase == 4) {
                        accepted = 1;
                        memcpy(seen, l->line, 128);
                    }
                }
            }
            if (accepted && !versions_has(seen)) {
                if (*found == 0) {
                    char path[512];
                    FILE *f;
                    snprintf(path, sizeof path, "%s/weak-counterexample.txt", g_dir);
                    f = fopen(path, "w");
                    if (f) {
                        fprintf(f, "weak model (line B as independently visible groups 0={gen} 1={fields} "
                                   "2={gen_end}); group apply order %d %d %d; schedule mask %#x over 7 positions "
                                   "(bit set = a group apply, clear = a drainer step: ld gen, ld line, ld gen_end, "
                                   "recheck gen)\naccepted snapshot: gen %llu write_idx %llu gen_end %llu\n"
                                   "published versions: old write_idx 0 (gen 1), new write_idx %llu (gen 3)\n"
                                   "the generation check accepted a line that mixes two versions\n",
                                perms[pi][0], perms[pi][1], perms[pi][2], mask,
                                (unsigned long long)psp_rd64(seen), (unsigned long long)psp_rd64(seen + 8),
                                (unsigned long long)psp_rd64(seen + 120), (unsigned long long)h->s.w);
                        fclose(f);
                    }
                }
                (*found)++;
            }
            mem_apply_all();
            g_weak = 0;
            run_free(&r);
            runs++;
        }
    }
    return runs;
}

/* ---- seeded random walks, strong model ---- */
static uint64_t rng(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static long random_walks(const uint64_t *seeds, int nseeds, int steps, long sample_every)
{
    long runs = 0;
    for (int si = 0; si < nseeds; si++) {
        uint64_t s = seeds[si];
        run r;
        lane_h *h;
        int fails = g_failures;
        g_weak = 0;
        run_init(&r, 16, 4, NULL);
        h = lane_new(&r, 8, 1, 1);
        psp_drain_declare(&r.p);
        for (int k = 0; k < steps; k++) {
            uint64_t x = rng(&s) % 100;
            if (x < 35)
                drain_step(&r, h, 1);
            else if (x < 55)
                zb(h, (uint16_t)(rng(&s) % 3));
            else if (x < 75)
                ze(h, (uint16_t)(rng(&s) % 3));
            else if (x < 80)
                zm(h, (uint16_t)(rng(&s) % 3));
            else if (x < 95) {
                psp_spu_flush(&h->s);
                call_ok(h, "flush");
            } else if (x < 98) {
                psp_spu_dec_write(&h->s, (uint32_t)rng(&s));
                call_ok(h, "dec_write");
            } else {
                h->pc.blocking = !h->pc.blocking; /* EBUSY windows */
            }
        }
        h->pc.blocking = 0;
        psp_spu_fini(&h->s);
        call_ok(h, "fini");
        psp_lane_end(&r.p, h->idx);
        psp_seal(&r.p);
        check_streams(&r, "random");
        if (g_failures != fails || (sample_every && runs % sample_every == 0))
            sample(&r, g_failures != fails ? "random-FAIL" : "random", (long)seeds[si], h);
        run_free(&r);
        runs++;
        if (g_failures > 20)
            break;
    }
    return runs;
}

/* ---- the lifetime model (codex 02:18 item 5): every order of D (one drainer step, a saved coroutine between
 * steps), X (end processing), R (end request), P (poll: free, then slot reuse by a new lane), S (seal), for four
 * variants.  The join precondition holds before the schedule (the producer is done).  The shim fails any PPU access
 * to freed memory; every lane must be freed at the end with the cap fully refunded.  One abstract lock: each action
 * is one call, and frees happen only in P, outside every drainer call. ---- */
static int next_perm(char *a, int n)
{
    int i = n - 2, j = n - 1;
    while (i >= 0 && a[i] >= a[i + 1])
        i--;
    if (i < 0)
        return 0;
    while (a[j] <= a[i])
        j--;
    char t = a[i];
    a[i] = a[j];
    a[j] = t;
    for (int x = i + 1, y = n - 1; x < y; x++, y--) {
        t = a[x];
        a[x] = a[y];
        a[y] = t;
    }
    return 1;
}

static void produce(lane_h *h, int close)
{
    zb(h, 0);
    ze(h, 0);
    psp_spu_flush(&h->s);
    call_ok(h, "flush");
    if (close) {
        psp_spu_fini(&h->s);
        call_ok(h, "fini");
    }
}

static long lifetime_search(long *reuses)
{
    static const char *variants[4] = {"closed", "sink-failure", "abandoned", "timeout-0"};
    long runs = 0;
    *reuses = 0;
    for (int v = 0; v < 4; v++) {
        char seq[] = "DDDDDDPPRSX"; /* sorted: D < P < R < S < X */
        int n = (int)strlen(seq);
        do {
            run r;
            lane_h *h;
            uint32_t slot;
            int fails = g_failures;
            g_weak = 0;
            run_init(&r, 8, 4, NULL);
            if (v == 1)
                r.fail_sink_at = 6;
            h = lane_new(&r, 4, 0, 1);
            slot = h->idx;
            psp_drain_declare(&r.p);
            produce(h, v != 2); /* v 2: the producer was terminated without fini, then joined */
            for (int k = 0; k < n; k++) {
                psp_lane *l = &r.p.lanes[slot];
                switch (seq[k]) {
                case 'D':
                    psp_lane_step(&r.p, slot);
                    break;
                case 'X':
                    psp_drain_ends(&r.p);
                    break;
                case 'R':
                    if (l->state != PSP_L_FREE)
                        psp_lane_end_request(&r.p, slot);
                    break;
                case 'S':
                    psp_seal(&r.p);
                    break;
                default: /* P */
                    if (l->state != PSP_L_FREE) {
                        int rc = psp_lane_end_poll(&r.p, slot, v == 3 ? 0 : 2);
                        if (rc == PSP_P_OK || rc == PSP_P_ESTOPPED) {
                            lane_h *h2 = lane_new(&r, 4, 0, 1); /* reuse: refused after the seal */
                            if (h2) {
                                (*reuses)++;
                                if (h2->idx != slot)
                                    FAIL("lifetime: the freed slot was not the one reused");
                                produce(h2, 1);
                            }
                        }
                    }
                }
            }
            /* cleanup: end and free everything still allocated */
            for (int round = 0; round < 6; round++) {
                for (uint32_t i = 0; i < r.p.cfg.lanes_max; i++) {
                    if (r.p.lanes[i].state == PSP_L_FREE)
                        continue;
                    psp_lane_end_request(&r.p, i);
                    psp_drain(&r.p); /* after the seal it does nothing: the seal released every lane */
                    psp_lane_end_poll(&r.p, i, 8);
                }
            }
            for (uint32_t i = 0; i < r.p.cfg.lanes_max; i++)
                if (r.p.lanes[i].state != PSP_L_FREE)
                    FAIL("lifetime %s %s: lane %u never freed", variants[v], seq, i);
            if (r.p.mem_used != 0)
                FAIL("lifetime %s %s: %llu bytes still charged", variants[v], seq, (unsigned long long)r.p.mem_used);
            /* a lane is freed only after its release, which follows its FINAL: every lane that ever existed has
             * exactly one accepted FINAL unless the sink failed */
            for (uint32_t j = 0; j < r.nl; j++) {
                uint64_t nx, ev, lo;
                int fin;
                accepted_counts(&r, r.lanes[j]->lane_id, &nx, &ev, &lo, &fin);
                if (!r.p.sink_failed && !fin)
                    FAIL("lifetime %s %s: lane %u was freed without an accepted FINAL", variants[v], seq,
                         r.lanes[j]->lane_id);
            }
            psp_seal(&r.p);
            check_streams(&r, "lifetime");
            if (g_failures != fails) {
                char name[64];
                FILE *e;
                snprintf(name, sizeof name, "lifetime-FAIL-%d-%s", v, seq);
                write_capture(&r, g_dir, name);
                e = expect_open(g_dir, name);
                fprintf(e, "smoke-exit 0 1 4\n"); /* kept for inspection; the failure is already counted */
                fclose(e);
            }
            run_free(&r);
            runs++;
            if (g_failures > 20)
                return runs;
        } while (next_perm(seq, n));
    }
    return runs;
}

int sched_main(const char *dir)
{
    long life, reuses;
    long strong, weak, found, walks;
    uint64_t seeds[2000];
    char path[512];
    FILE *f;
    g_dir = dir;
    /* producer: 3 events, flush, 5 events (LS 4: one drop), flush, 2 events, fini = 3 + 3 + 4 shared steps;
     * drainer: 12 steps (about 1.7 drain cycles) */
    strong = strong_exhaustive("3F5F2X", 10, 12, 4000);
    weak = weak_search(&found);
    for (int i = 0; i < 2000; i++)
        seeds[i] = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1);
    walks = random_walks(seeds, 2000, 600, 20);
    life = lifetime_search(&reuses);
    snprintf(path, sizeof path, "%s/sched-summary.txt", dir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "strong exhaustive: producer \"3F5F2X\" (10 shared steps), drainer 12 steps, all C(22,10) "
                   "interleavings: %ld runs, bad snapshots %d\n", strong, g_badsnap);
        fprintf(f, "weak search: 6 group orders x C(7,3) placements = %ld runs, accepted mixed snapshots %ld\n",
                weak, found);
        fprintf(f, "random walks (strong): seeds 0x9E3779B97F4A7C15 * k for k = 1..2000, 600 steps, L 8, C 16: "
                   "%ld runs\nlifetime: every order of D x6 X R P x2 S (11!/(6!2!) = 27720) x 4 variants (closed, "
                   "sink failure at chunk 6, abandoned producer, poll timeout 0) = %ld runs, slot reuses %ld; no PPU "
                   "access to freed memory, every lane freed, cap refunded to 0\nsamples written %ld\nfailures %d\n",
                walks, life, reuses, g_samples, g_failures);
        fclose(f);
    }
    printf("spu-sched strong=%ld weak=%ld weak_counterexamples=%ld random=%ld lifetime=%ld reuses=%ld samples=%ld "
           "failures=%d\n", strong, weak, found, walks, life, reuses, g_samples, g_failures);
    if (g_order_faults)
        FAIL("%d ring loads without the post-acceptance barrier", g_order_faults);
    if (!found)
        FAIL("the weak model found no accepted mixed snapshot: the negative control did not work");
    if (g_failures)
        return 1;
    printf("spu-sched: PASS\n");
    return 0;
}
