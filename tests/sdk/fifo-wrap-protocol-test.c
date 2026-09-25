/*
 * Host model of the GCM FIFO wrap protocol (t_38e8bf5a).
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
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ps3tc_fifo_wrap_protocol.h"

#define WORDS 64u
#define BASE  0x1000u              /* IO offset of word 0 = begin */

typedef struct {
    uint32_t fifo[WORDS];
    volatile uint32_t put, get;
    uint32_t fetched[4 * WORDS];
    unsigned nfetched;
    unsigned step;                 /* words fetched per pause() */
    unsigned stalls;               /* pause() calls that fetch nothing first */
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

static void pause_fn(void *arg, uint32_t want, uint32_t spins)
{
    model *m = arg;
    (void)want;
    if (spins > 100000u) { fprintf(stderr, "model: wait never finished\n"); _Exit(3); }
    if (m->stalls) { m->stalls--; return; }
    fetch(m, m->step);
}

#ifdef ONE_PHASE_CONTROL
/* The protocol this replaces, kept only as the test's red control. */
static void wrap(volatile uint32_t *tail, uint32_t tail_off, uint32_t begin_off,
                 const ps3tc_fifo_port *p)
{
    uint32_t spins = 0;
    (void)tail_off;
    *tail = begin_off | PS3TC_NV40_JUMP_FLAG;
    *p->put = begin_off;
    while (*p->get != begin_off) p->pause(p->arg, begin_off, spins++);
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
    model m;
    memset(&m, 0, sizeof m);
    for (unsigned i = 0; i < lap; ++i) m.fifo[i] = 0x00040000u | i;  /* distinct */
    m.put = BASE + 4u * published;
    m.get = BASE;
    m.step = step;
    m.stalls = stalls;
    fetch(&m, consumed);

    const ps3tc_fifo_port port = { &m.put, &m.get, barrier, pause_fn, &m };
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
    printf("  %-44s %s (fetched %u of %u lap words, jump %s, get=0x%x put=0x%x)\n",
           name, ok ? "ok" : "FAIL", lapFetched, lap, sawJump ? "yes" : "no",
           (unsigned)m.get, (unsigned)m.put);
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
    if (failures) { printf("fifo-wrap-protocol: %d scenario(s) FAILED\n", failures); return 1; }
    printf("fifo-wrap-protocol: all scenarios passed\n");
    return 0;
}
