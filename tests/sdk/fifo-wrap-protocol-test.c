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
 * The model also asserts two invariants of the current two-phase protocol:
 *
 *   - within a single wrap, the pause calls come in a non-increasing
 *     phase number (i.e. all 1s first, then all 2s; a phase 1 after a
 *     phase 2 would mean the protocol got out of order);
 *   - within a phase, the elapsed_us value the protocol hands to pause is
 *     non-decreasing (the protocol's clock is monotone; a decrease means
 *     either the clock went backwards or the protocol measured one pause
 *     against a different origin than the rest).
 *
 * These invariants are checked per wrap: pause_fn calls model_check_pause
 * to update the model's running state, and the scenario (or repeated-laps
 * block) calls model_validate after the wrap returns.
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
#define MAX_PAUSES_PER_WRAP 1000000u   /* overflow guard per wrap; 1 s of
                                         * 30-us sleeps at ~33 spins per pause
                                         * is 33 * 1e5, so 1e6 is already 30
                                         * seconds; well above what any wrap
                                         * should take. */

typedef struct {
    uint32_t fifo[WORDS];
    volatile uint32_t put, get;
    uint32_t fetched[4 * WORDS];
    unsigned nfetched;
    unsigned step;                 /* words fetched per pause() */
    unsigned stalls;               /* pause() calls that fetch nothing first */

    /* Protocol clock: advances by 1 microsecond on each pause() call, so
     * the protocol sees a strictly increasing (and therefore non-decreasing)
     * clock.  The model never calls the clock directly; it bumps only inside
     * pause() (the protocol is the caller). */
    uint64_t clock_us;

    /* Per-wrap pause call tracking (reset by model_begin_wrap, validated by
     * model_validate; checked incrementally by model_check_pause). */
    unsigned   npause_seen;
    unsigned   phase1_pauses;     /* pauses in this wrap that reported phase 1 */
    unsigned   phase2_pauses;     /* pauses in this wrap that reported phase 2 */
    int        phase_order_ok;         /* 0 = a phase 1 came after a phase 2 */
    int        elapsed_monotonic_ok;   /* 0 = elapsed_us decreased in a phase */
    unsigned   prev_phase;
    uint64_t   prev_elapsed;
    int        seen_phase2;
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

/* Reset the per-wrap pause tracking before a wrap call. */
static void model_begin_wrap(model *m)
{
    m->npause_seen          = 0;
    m->phase1_pauses        = 0;
    m->phase2_pauses        = 0;
    m->phase_order_ok       = 1;
    m->elapsed_monotonic_ok = 1;
    m->prev_phase           = 0;
    m->prev_elapsed         = 0;
    m->seen_phase2          = 0;
}

/* Check one pause call against this wrap's running state.  Bumps the clock
 * first, then validates the phase and elapsed invariants. */
static void model_check_pause(model *m, unsigned int phase, uint64_t elapsed_us)
{
    m->clock_us++;
    if (m->npause_seen >= MAX_PAUSES_PER_WRAP) {
        fprintf(stderr, "model: %u pause calls in one wrap; giving up\n", MAX_PAUSES_PER_WRAP);
        _Exit(3);
    }
    if (phase != 1u && phase != 2u) {
        fprintf(stderr, "model: pause with unknown phase %u\n", phase);
        _Exit(3);
    }
    if (phase == 2u) {
        m->seen_phase2 = 1;
        ++m->phase2_pauses;
    } else {  /* phase == 1 */
        if (m->seen_phase2) m->phase_order_ok = 0;
        ++m->phase1_pauses;
    }
    if (m->npause_seen > 0u && m->prev_phase == phase &&
        elapsed_us < m->prev_elapsed)
        m->elapsed_monotonic_ok = 0;
    m->prev_phase   = phase;
    m->prev_elapsed = elapsed_us;
    ++m->npause_seen;
}

/* Return 1 iff all pause invariants held for this wrap; 0 otherwise. */
static int model_phase_ok(const model *m)
{
    return m->phase_order_ok && m->elapsed_monotonic_ok;
}

static void pause_fn(void *arg, uint32_t want, uint32_t spins,
                     unsigned int phase, uint64_t elapsed_us)
{
    model *m = arg;
    (void)want;
    (void)spins;
    model_check_pause(m, phase, elapsed_us);
    if (m->stalls) { m->stalls--; return; }
    fetch(m, m->step);
}

#ifdef ONE_PHASE_CONTROL
/* The protocol this replaces, kept only as the test's red control.  It has
 * no phases and no elapsed argument: it just parks the consumer at begin
 * and spins until GET == begin.  It reports phase 1 and elapsed 0 to the
 * model, which records no invariants violation (no phase 2 was ever seen,
 * elapsed is flat at 0).  The scenario's other checks (did every lap word
 * get fetched, did the fetcher land at begin, was the JUMP fetched) are
 * what FAIL for the never-flushed E5 case - that is the signal this
 * control build exists to provide. */
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
 * covered by PUT before the wrap; consumed = words the GPU already fetched. */
static void scenario(const char *name, unsigned lap, unsigned published,
                     unsigned consumed, unsigned step, unsigned stalls)
{
    static model m;
    memset(&m, 0, sizeof m);
    for (unsigned i = 0; i < lap; ++i) m.fifo[i] = 0x00040000u | i;  /* distinct */
    m.put = BASE + 4u * published;
    m.get = BASE;
    m.step = step;
    m.stalls = stalls;
    fetch(&m, consumed);

    const ps3tc_fifo_port port = { &m.put, &m.get, barrier, now_fn, pause_fn, &m };
    model_begin_wrap(&m);
    wrap(&m.fifo[lap], BASE + 4u * lap, BASE, &port);

    /* The caller now reuses begin: anything not yet fetched is lost. */
    int ok = m.get == BASE && m.put == BASE;
    unsigned lapFetched = 0;
    for (unsigned i = 0; i < m.nfetched; ++i)
        if (!(m.fetched[i] & PS3TC_NV40_JUMP_FLAG)) {
            if (m.fetched[i] != (0x00040000u | lapFetched)) ok = 0;
            ++lapFetched;
        }
    int sawJump = m.nfetched > 0 && (m.fetched[m.nfetched - 1] & PS3TC_NV40_JUMP_FLAG);
    ok = ok && lapFetched == lap && sawJump;
    if (!model_phase_ok(&m))
        ok = 0;   /* pause phase/elapsed invariants failed in this wrap */
    printf("  %-44s %s (fetched %u of %u lap words, jump %s, get=0x%x put=0x%x, pauses=%u, phase_ok=%d elapsed_ok=%d)\n",
           name, ok ? "ok" : "FAIL", lapFetched, lap, sawJump ? "yes" : "no",
           (unsigned)m.get, (unsigned)m.put,
           m.npause_seen, m.phase_order_ok, m.elapsed_monotonic_ok);
    if (!ok) ++failures;
}

int main(void)
{
    /* The E5 state: nothing flushed since the last wrap, GPU idle at begin. */
    scenario("idle, never-flushed lap (EMP E5)",          40, 0, 0, 1, 0);
    scenario("idle, never-flushed lap, slow consumer",     40, 0, 0, 1, 50);
    scenario("partial flush, GPU drained to the flush",    40, 16, 16, 1, 0);
    scenario("partial flush, GPU still mid-flush",         40, 16, 5, 1, 3);
    scenario("whole lap flushed, GPU drained",             40, 40, 40, 4, 0);
    scenario("whole lap flushed, GPU not started",         40, 40, 0, 8, 2);
    scenario("one-word lap",                               1, 0, 0, 1, 0);
    scenario("lap ends at the last word of the ring",      WORDS - 1, 0, 0, 3, 0);
    /* Multiple laps through the same storage, with no explicit flush and
     * a stalled consumer each time. Reuse is permitted only after wrap. */
    {
        static model m;
        memset(&m, 0, sizeof m);
        m.get = m.put = BASE;
        m.step = 19;
        const ps3tc_fifo_port port = { &m.put, &m.get, barrier, now_fn, pause_fn, &m };
        int ok = 1;
        for (unsigned lap = 0; lap < 5; ++lap) {
            m.nfetched = 0;
            m.stalls = 7;
            for (unsigned i = 0; i < WORDS - 1; ++i)
                m.fifo[i] = 0x10000000u | (lap << 20) | i;
            model_begin_wrap(&m);
            wrap(&m.fifo[WORDS-1], BASE+4u*(WORDS-1), BASE, &port);
            if (m.get != BASE || m.put != BASE || m.nfetched != WORDS) ok = 0;
            for (unsigned i = 0; i < WORDS - 1; ++i)
                if (m.fetched[i] != (0x10000000u | (lap << 20) | i)) ok = 0;
            if (m.fetched[WORDS-1] != (BASE | PS3TC_NV40_JUMP_FLAG)) ok = 0;
            if (!model_phase_ok(&m)) ok = 0;
        }
        printf("  repeated lagged laps: %s (%u words)\n", ok?"ok":"FAIL", WORDS);
        if (!ok) ++failures;
    }
    if (failures) { printf("fifo-wrap-protocol: %d scenario(s) FAILED\n", failures); return 1; }
    printf("fifo-wrap-protocol: all scenarios passed\n");
    return 0;
}
