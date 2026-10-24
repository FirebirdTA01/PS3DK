/*
 * Host model of the GCM FIFO wrap protocol (fifo-wrap-publication).
 *
 * A simulated RSX fetcher consumes a FIFO of words: it fetches only while
 * GET != PUT, records every word it fetches, and follows a JUMP by moving GET
 * to the target.  Each scenario fills one lap, runs the wrap, and checks what
 * a real consumer would have seen:
 *
 *   every word of the retired lap was fetched exactly once, in order,
 *   the JUMP was fetched, and the fetcher is parked at begin (GET == PUT ==
 *   begin) when the wrap returns - so the caller may overwrite begin.
 *
 * Built twice by fifo-wrap-protocol-test.sh: once against the real protocol
 * header (must pass every scenario) and once with -DONE_PHASE_CONTROL, the
 * protocol this replaced (must FAIL the idle, never-flushed lap - the EMP E5
 * command loss).  The second build is what shows the model can see the bug.
 *
 * The model also checks the wait diagnostic the protocol hands to pause, on
 * a deterministic clock that advances by a fixed `tick` on each pause call:
 *
 *   phase    each pause reports the phase of the GET value it waits for
 *            (tail = phase 1, begin = phase 2), the phases come in order,
 *            phase 1 appears exactly when GET was short of tail at entry,
 *            and phase 2 always appears (GET is at tail, not begin, when
 *            the JUMP is released);
 *   spins    the k-th pause of a phase reports spins == k;
 *   progress within a phase, elapsed_us grows by exactly one tick per pause;
 *   origin   the first pause of each phase reports elapsed_us == 0, so
 *            phase 2 is measured from its own start (even after a long
 *            phase 1) and no wrap carries an origin from an earlier one.
 *
 * The lap checks and the pause checks are reported separately, so that the
 * one-phase control fails on the lost lap itself.  The pause checks do not
 * apply to that control (it has no phases); fifo-wrap-protocol-test.sh also
 * builds three mutants of the protocol header (every pause reported as
 * phase 1, a constant zero elapsed, phase 2 timed from phase 1's start) and
 * requires each to fail its own check.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3tc_fifo_wrap_protocol.h"

#ifndef WORDS
#define WORDS 64u
#endif
#define BASE   0x1000u                 /* IO offset of word 0 = begin */
#define MAX_PAUSES_PER_WRAP 1000000u   /* a wait that never finishes ends the test */

typedef struct {
    uint32_t fifo[WORDS];
    volatile uint32_t put, get;
    uint32_t fetched[4 * WORDS];
    unsigned nfetched;
    unsigned step;                 /* words fetched per pause() */
    unsigned stalls;               /* pause() calls that fetch nothing first */

    /* Protocol clock: advances by `tick` microseconds on each pause() call
     * and at no other time, so the elapsed value of every pause is known in
     * advance.  It is not reset between wraps. */
    uint64_t clock_us;
    uint64_t tick;

    /* Per-wrap pause checks (reset by model_begin_wrap, updated by
     * model_check_pause, completed by model_end_wrap). */
    uint32_t   want_tail, want_begin;  /* the GET values of phase 1 and 2 */
    int        expect_phase1;          /* GET was short of tail at entry */
    unsigned   npause_seen;
    unsigned   reported[3];            /* pauses reporting phase 1 / 2 */
    unsigned   in_phase[3];            /* pauses so far in the phase waited for */
    uint64_t   prev_elapsed[3];
    unsigned   last_phase;             /* phase of the previous pause, 0 = none */
    uint64_t   phase1_last_elapsed, phase2_first_elapsed;
    int        phase_ok, spins_ok, progress_ok, origin_ok;
} model;

static uint32_t idx(uint32_t off) { return (off - BASE) / 4u; }

static void fetch(model *m, unsigned n)
{
    while (n-- && m->get != m->put) {
        uint32_t w = m->fifo[idx(m->get)];
        m->fetched[m->nfetched++] = w;
        if (w & PS3TC_NV40_JUMP_FLAG)
            m->get = w & ~PS3TC_NV40_JUMP_FLAG;
        else
            m->get += 4u;
    }
}

static void barrier(void *arg) { (void)arg; }

static uint64_t now_fn(void *arg)
{
    return ((model *)arg)->clock_us;
}

/* Reset the per-wrap pause checks before a wrap that waits for GET to reach
 * tail_off (phase 1) and then begin_off (phase 2). */
static void model_begin_wrap(model *m, uint32_t tail_off, uint32_t begin_off)
{
    m->want_tail            = tail_off;
    m->want_begin           = begin_off;
    m->expect_phase1        = m->get != tail_off;
    m->npause_seen          = 0;
    memset(m->reported, 0, sizeof m->reported);
    memset(m->in_phase, 0, sizeof m->in_phase);
    memset(m->prev_elapsed, 0, sizeof m->prev_elapsed);
    m->last_phase           = 0;
    m->phase1_last_elapsed  = 0;
    m->phase2_first_elapsed = 0;
    m->phase_ok = m->spins_ok = m->progress_ok = m->origin_ok = 1;
}

/* Check one pause call.  The checks are keyed by the phase the pause really
 * waits for (from `want`), not by the phase it reports, so that a wrong
 * phase number shows up as a phase failure alone. */
static void model_check_pause(model *m, uint32_t want, uint32_t spins,
                              unsigned int phase, uint64_t elapsed_us)
{
    m->clock_us += m->tick;
    if (m->npause_seen >= MAX_PAUSES_PER_WRAP) {
        fprintf(stderr, "model: %u pause calls in one wrap; giving up\n", MAX_PAUSES_PER_WRAP);
        _Exit(3);
    }
    ++m->npause_seen;
    if (phase == PS3TC_FIFO_PHASE_1 || phase == PS3TC_FIFO_PHASE_2)
        ++m->reported[phase];
    else
        m->phase_ok = 0;

    unsigned actual = want == m->want_tail  ? 1u
                    : want == m->want_begin ? 2u : 0u;
    if (actual == 0u) {     /* a wait for neither GET value */
        m->phase_ok = 0;
        return;
    }
    if (phase != actual || actual < m->last_phase)
        m->phase_ok = 0;
    m->last_phase = actual;

    unsigned k = m->in_phase[actual]++;
    if (spins != k)
        m->spins_ok = 0;
    if (k == 0u) {
        if (elapsed_us != 0u)
            m->origin_ok = 0;
        if (actual == 2u)
            m->phase2_first_elapsed = elapsed_us;
    } else if (elapsed_us - m->prev_elapsed[actual] != m->tick) {
        m->progress_ok = 0;
    }
    m->prev_elapsed[actual] = elapsed_us;
    if (actual == 1u)
        m->phase1_last_elapsed = elapsed_us;
}

/* The checks that need the whole wrap: which phases appeared. */
static void model_end_wrap(model *m)
{
    if ((m->reported[1] != 0u) != m->expect_phase1 || m->reported[2] == 0u)
        m->phase_ok = 0;
}

/* Return 1 iff every pause check held for this wrap.  The one-phase control
 * has no phases, so the checks do not apply to it. */
static int model_pause_ok(const model *m)
{
#ifdef ONE_PHASE_CONTROL
    (void)m;
    return 1;
#else
    return m->phase_ok && m->spins_ok && m->progress_ok && m->origin_ok;
#endif
}

static const char *okfail(int ok) { return ok ? "ok" : "FAIL"; }

static void print_pause_checks(const model *m)
{
#ifdef ONE_PHASE_CONTROL
    (void)m;
    printf(" pause=n/a");
#else
    printf(" pause=%s phase=%s spins=%s progress=%s origin=%s p1=%u p2=%u",
           okfail(model_pause_ok(m)), okfail(m->phase_ok), okfail(m->spins_ok),
           okfail(m->progress_ok), okfail(m->origin_ok),
           m->reported[1], m->reported[2]);
#endif
}

static void pause_fn(void *arg, uint32_t want, uint32_t spins,
                     unsigned int phase, uint64_t elapsed_us)
{
    model *m = arg;
    model_check_pause(m, want, spins, phase, elapsed_us);
    if (m->stalls) { m->stalls--; return; }
    fetch(m, m->step);
}

#ifdef ONE_PHASE_CONTROL
/* The protocol this replaces, kept only as the test's red control.  It has
 * no phases and no elapsed argument: it just parks the consumer at begin
 * and spins until GET == begin.  It reports phase 1 and elapsed 0; the
 * pause checks do not apply to it.  The lap checks (did every lap word get
 * fetched, did the fetcher land at begin, was the JUMP fetched) are what
 * FAIL for the never-flushed E5 case - that is the signal this control
 * build exists to provide. */
static void wrap(volatile uint32_t *tail, uint32_t tail_off, uint32_t begin_off,
                 const ps3tc_fifo_port *p)
{
    uint32_t spins = 0;
    (void)tail_off;
    *tail = begin_off | PS3TC_NV40_JUMP_FLAG;
    *p->put = begin_off;
    while (*p->get != begin_off)
        p->pause(p->arg, begin_off, spins++, 1u, 0ull);
}
#else
#define wrap ps3tc_fifo_wrap_protocol
#endif

static int failures;

/* lap = words written this lap, from begin; published = words already
 * covered by PUT before the wrap; consumed = words the GPU already fetched;
 * tick = microseconds the protocol clock advances per pause. */
static void scenario(const char *name, unsigned lap, unsigned published,
                     unsigned consumed, unsigned step, unsigned stalls,
                     uint64_t tick)
{
    static model m;
    memset(&m, 0, sizeof m);
    for (unsigned i = 0; i < lap; ++i) m.fifo[i] = 0x00040000u | i;  /* distinct */
    m.put = BASE + 4u * published;
    m.get = BASE;
    m.step = step;
    m.stalls = stalls;
    m.tick = tick;
    fetch(&m, consumed);

    const ps3tc_fifo_port port = { &m.put, &m.get, barrier, now_fn, pause_fn, &m };
    model_begin_wrap(&m, BASE + 4u * lap, BASE);
    wrap(&m.fifo[lap], BASE + 4u * lap, BASE, &port);
    model_end_wrap(&m);

    /* The caller now reuses begin: anything not yet fetched is lost. */
    int lap_ok = m.get == BASE && m.put == BASE;
    unsigned lapFetched = 0;
    for (unsigned i = 0; i < m.nfetched; ++i)
        if (!(m.fetched[i] & PS3TC_NV40_JUMP_FLAG)) {
            if (m.fetched[i] != (0x00040000u | lapFetched)) lap_ok = 0;
            ++lapFetched;
        }
    int sawJump = m.nfetched > 0 && (m.fetched[m.nfetched - 1] & PS3TC_NV40_JUMP_FLAG);
    lap_ok = lap_ok && lapFetched == lap && sawJump;
    int ok = lap_ok && model_pause_ok(&m);
    printf("  %-44s %s (fetched %u of %u lap words, jump %s, get=0x%x put=0x%x, pauses=%u) lap=%s",
           name, okfail(ok), lapFetched, lap, sawJump ? "yes" : "no",
           (unsigned)m.get, (unsigned)m.put, m.npause_seen, okfail(lap_ok));
    print_pause_checks(&m);
    printf("\n");
    if (!ok) ++failures;
}

/* A deliberately long phase 1 (600 stalled pauses at 1 ms each before the
 * consumer starts): phase 2 must still be timed from its own start. */
static void long_phase1(void)
{
    static model m;
    memset(&m, 0, sizeof m);
    for (unsigned i = 0; i < 40; ++i) m.fifo[i] = 0x00040000u | i;
    m.put = m.get = BASE;
    m.step = 1;
    m.stalls = 600;
    m.tick = 1000;
    const ps3tc_fifo_port port = { &m.put, &m.get, barrier, now_fn, pause_fn, &m };
    model_begin_wrap(&m, BASE + 4u * 40u, BASE);
    wrap(&m.fifo[40], BASE + 4u * 40u, BASE, &port);
    model_end_wrap(&m);
    int ok = model_pause_ok(&m);
#ifndef ONE_PHASE_CONTROL
    /* 640 phase-1 pauses: 600 stalls, then one per lap word. */
    ok = ok && m.reported[1] == 640u && m.phase1_last_elapsed == 639u * 1000u
            && m.phase2_first_elapsed == 0u;
#endif
    printf("  %-44s %s (phase 1 ended at %llu us, phase 2 started at %llu us)",
           "long phase 1, then phase 2 from its own start", okfail(ok),
           (unsigned long long)m.phase1_last_elapsed,
           (unsigned long long)m.phase2_first_elapsed);
    print_pause_checks(&m);
    printf("\n");
    if (!ok) ++failures;
}

int main(void)
{
    /* The E5 state: nothing flushed since the last wrap, GPU idle at begin. */
    scenario("idle, never-flushed lap (EMP E5)",          40, 0, 0, 1, 0, 3);
    scenario("idle, never-flushed lap, slow consumer",     40, 0, 0, 1, 50, 3);
    scenario("partial flush, GPU drained to the flush",    40, 16, 16, 1, 0, 3);
    scenario("partial flush, GPU still mid-flush",         40, 16, 5, 1, 3, 3);
    scenario("whole lap flushed, GPU drained",             40, 40, 40, 4, 0, 3);
    scenario("whole lap flushed, GPU not started",         40, 40, 0, 8, 2, 3);
    scenario("one-word lap",                               1, 0, 0, 1, 0, 3);
    scenario("lap ends at the last word of the ring",      WORDS - 1, 0, 0, 3, 0, 3);
    long_phase1();
    /* Multiple laps through the same storage, with no explicit flush and
     * a stalled consumer each time. Reuse is permitted only after wrap.
     * The clock runs on across the laps, so a wrap that kept an earlier
     * wrap's origin would fail the origin check. */
    {
        static model m;
        memset(&m, 0, sizeof m);
        m.get = m.put = BASE;
        m.step = 19;
        m.tick = 7;
        const ps3tc_fifo_port port = { &m.put, &m.get, barrier, now_fn, pause_fn, &m };
        int lap_ok = 1, pause_ok = 1;
        for (unsigned lap = 0; lap < 5; ++lap) {
            m.nfetched = 0;
            m.stalls = 7;
            for (unsigned i = 0; i < WORDS - 1; ++i)
                m.fifo[i] = 0x10000000u | (lap << 20) | i;
            model_begin_wrap(&m, BASE+4u*(WORDS-1), BASE);
            wrap(&m.fifo[WORDS-1], BASE+4u*(WORDS-1), BASE, &port);
            model_end_wrap(&m);
            if (m.get != BASE || m.put != BASE || m.nfetched != WORDS) lap_ok = 0;
            for (unsigned i = 0; i < WORDS - 1; ++i)
                if (m.fetched[i] != (0x10000000u | (lap << 20) | i)) lap_ok = 0;
            if (m.fetched[WORDS-1] != (BASE | PS3TC_NV40_JUMP_FLAG)) lap_ok = 0;
            if (!model_pause_ok(&m)) pause_ok = 0;
            printf("    lap %u:", lap);
            print_pause_checks(&m);
            printf("\n");
        }
        int ok = lap_ok && pause_ok;
        printf("  repeated lagged laps: %s (%u words) lap=%s\n", okfail(ok), WORDS, okfail(lap_ok));
        if (!ok) ++failures;
    }
    if (failures) { printf("fifo-wrap-protocol: %d scenario(s) FAILED\n", failures); return 1; }
    printf("fifo-wrap-protocol: all scenarios passed\n");
    return 0;
}
