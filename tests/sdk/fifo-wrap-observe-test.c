/*
 * Host model of the OBSERVED FIFO wrap (docs/design/ps3dk-prof-fifo.md).
 *
 * The same simulated fetcher as fifo-wrap-protocol-test.c drives the real
 * protocol header.  Each run prints the protocol's externally visible trace -
 * every barrier with the PUT value and the tail word it orders, every pause
 * with its want/spins/phase/elapsed - as "T" lines; fifo-wrap-observe-test.sh
 * requires that trace to be byte-identical between the plain build and the
 * observed build with every observer mode (none, no-op, dropping, reentrant).
 *
 * In the observed build the run also checks the six boundaries per wrap
 * (WRAP_ENTER and WRAP_EXIT emitted by this file exactly as ps3tc_fifo_wrap.c
 * does, the four phase boundaries by the protocol):
 *   events    exactly kinds 1..6 in order per wrap, with the kind-specific
 *             target and current offsets;
 *   brackets  before <= after; each BEGIN bracket contains the barrier that
 *             follows its PUT store; each END bracket starts after the phase's
 *             last pause and contains the moment GET was seen at the target;
 *   guard     a reentrant observer's nested boundary is suppressed, counted,
 *             and never delivered.
 * A plain build run with an observer mode must FAIL `events` (the old code
 * emits nothing) while its trace stays the baseline.
 *
 * Usage: model <none|noop|drop|reentrant>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3tc_fifo_wrap_protocol.h"
#include "ps3tc_fifo_observe.h"

#define WORDS 64u
#define BASE  0x1000u
#define MAXEV 128u

typedef struct {
    uint32_t fifo[WORDS];
    volatile uint32_t put, get;
    unsigned step, stalls;
    uint64_t mt;                   /* model time: +1 per clock read, barrier and pause */
    uint64_t now;                  /* the protocol clock: +3 per pause only */
    uint64_t last_pause[3];        /* model time of the last pause of phase 1 / 2 */
    uint64_t reached[3];           /* model time GET was first equal to the phase target */
    uint64_t put_barrier[3];       /* model time of the barrier that followed PUT = target */
    uint32_t tail_off;
    unsigned nbarrier;             /* barriers in this wrap: 1 after the JUMP, 2 after PUT = tail, 3 after PUT = begin */
} model;

static model M;
static int mode;                   /* 0 none, 1 noop, 2 drop, 3 reentrant */
static ps3tc_fifo_obs_state S;
static ps3tc_fifo_boundary ev[MAXEV];
static unsigned nev, nested_delivered, depth;

static uint32_t idx(uint32_t off) { return (off - BASE) / 4u; }

static void fetch(unsigned n)
{
    while (n-- && M.get != M.put) {
        uint32_t w = M.fifo[idx(M.get)];
        M.get = (w & PS3TC_NV40_JUMP_FLAG) ? (w & ~PS3TC_NV40_JUMP_FLAG) : M.get + 4u;
    }
    if (M.get == M.tail_off && !M.reached[1])
        M.reached[1] = M.mt;
    if (M.get == BASE && M.reached[1] && !M.reached[2] && M.put == BASE)
        M.reached[2] = M.mt;
}

static void barrier(void *arg)
{
    (void)arg;
    M.mt++;
    printf("T barrier put=0x%x tail=0x%x\n", (unsigned)M.put, (unsigned)M.fifo[idx(M.tail_off)]);
    if (++M.nbarrier == 2u)
        M.put_barrier[1] = M.mt;
    else if (M.nbarrier == 3u)
        M.put_barrier[2] = M.mt;
}
static uint64_t now_us(void *arg) { (void)arg; return M.now; }

static void pause_fn(void *arg, uint32_t want, uint32_t spins, unsigned int phase, uint64_t elapsed)
{
    (void)arg;
    printf("T pause want=0x%x spins=%u phase=%u elapsed=%llu\n", (unsigned)want, (unsigned)spins, phase,
           (unsigned long long)elapsed);
    M.mt++;
    M.now += 3;
    if (phase < 3)
        M.last_pause[phase] = M.mt;
    if (M.stalls) {
        M.stalls--;
        return;
    }
    fetch(M.step);
}

static void observer(void *user, const ps3tc_fifo_boundary *b)
{
    (void)user;
    if (depth) {
        nested_delivered++; /* a guard failure: counted, not recorded as a boundary */
        return;
    }
    if (mode == 2)
        return; /* a dropping observer: it records nothing */
    if (nev < MAXEV)
        ev[nev++] = *b;
    if (mode == 3 && !depth) {
        /* reentry: a boundary reached while the observer runs must be suppressed */
        ps3tc_fifo_obs_wrap w = { b->begin_off, b->tail_off, b->requested_words, 0u };
        depth++;
        ps3tc_fifo_obs_emit(&S, b->kind, b->tb_before, b->tb_after, &w);
        depth--;
    }
}

#ifdef PS3TC_FIFO_OBSERVE_POINTS
static uint64_t tb(void *arg) { (void)arg; return ++M.mt; }
static ps3tc_fifo_obs_wrap g_wrap;
static void observe_phase(void *arg, unsigned int kind, uint64_t before, uint64_t after)
{
    (void)arg;
    ps3tc_fifo_obs_emit(&S, kind, before, after, &g_wrap);
}
#endif

/* ps3tc_fifo_wrap_callback's order: ENTER, the protocol, current rewound, EXIT. */
static void wrap(unsigned lap, uint32_t words)
{
    const uint32_t tail_off = BASE + 4u * lap;
    M.tail_off = tail_off;
    memset(M.last_pause, 0, sizeof M.last_pause);
    memset(M.reached, 0, sizeof M.reached);
    memset(M.put_barrier, 0, sizeof M.put_barrier);
    M.nbarrier = 0;
    if (M.get == tail_off)
        M.reached[1] = M.mt;
#ifdef PS3TC_FIFO_OBSERVE_POINTS
    const int observed = S.reg.event != NULL;
    g_wrap = (ps3tc_fifo_obs_wrap){ BASE, tail_off, words, 0u };
    if (observed) {
        const uint64_t t = tb(NULL);
        ps3tc_fifo_obs_emit(&S, PS3TC_FIFO_B_WRAP_ENTER, t, t, &g_wrap);
    }
    const ps3tc_fifo_port port = { &M.put, &M.get, barrier, now_us, pause_fn, &M,
                                   observed ? observe_phase : NULL, tb };
#else
    (void)words;
    const ps3tc_fifo_port port = { &M.put, &M.get, barrier, now_us, pause_fn, &M };
#endif
    ps3tc_fifo_wrap_protocol(&M.fifo[lap], tail_off, BASE, &port);
#ifdef PS3TC_FIFO_OBSERVE_POINTS
    if (observed) {
        const uint64_t t = tb(NULL);
        ps3tc_fifo_obs_emit(&S, PS3TC_FIFO_B_WRAP_EXIT, t, t, &g_wrap);
    }
#endif
}

static int events_ok = 1, brackets_ok = 1, guard_ok = 1;

static void check_wrap(unsigned first, uint32_t tail_off, uint32_t words)
{
    unsigned k;
    if (nev - first != 6u) {
        events_ok = 0;
        return;
    }
    for (k = 0; k < 6u; k++) {
        const ps3tc_fifo_boundary *b = &ev[first + k];
        uint32_t target = (k == 1 || k == 2) ? tail_off : (k == 3 || k == 4) ? BASE : 0u;
        uint32_t current = k == 5 ? BASE : tail_off;
        if (b->kind != k + 1u || b->size != sizeof *b || b->flags || b->begin_off != BASE || b->tail_off != tail_off ||
            b->target_off != target || b->current_off != current || b->requested_words != words || b->reserved)
            events_ok = 0;
        if (b->tb_before > b->tb_after || (k && b->tb_before < ev[first + k - 1].tb_after))
            brackets_ok = 0;
    }
    for (k = 1; k <= 2; k++) {
        const ps3tc_fifo_boundary *bg = &ev[first + 2 * k - 1], *en = &ev[first + 2 * k];
        /* the PUT store's barrier lies inside BEGIN */
        if (!(bg->tb_before < M.put_barrier[k] && M.put_barrier[k] < bg->tb_after))
            brackets_ok = 0;
        /* END starts after the phase's last pause, and GET had reached the target before END ends */
        if (M.last_pause[k] && en->tb_before < M.last_pause[k])
            brackets_ok = 0;
        if (!M.last_pause[k] && en->tb_before != bg->tb_after)
            brackets_ok = 0;
        if (M.reached[k] > en->tb_after)
            brackets_ok = 0;
    }
}

static void scenario(const char *name, unsigned lap, unsigned published, unsigned consumed, unsigned step,
                     unsigned stalls, uint32_t words)
{
    unsigned i, first = nev;
    printf("T scenario %s\n", name);
    for (i = 0; i < lap; ++i)
        M.fifo[i] = 0x00040000u | i;
    M.put = BASE + 4u * published;
    M.get = BASE;
    M.step = step;
    M.stalls = stalls;
    M.tail_off = BASE + 4u * lap;
    fetch(consumed);
    wrap(lap, words);
    if (mode == 1 || mode == 3)
        check_wrap(first, BASE + 4u * lap, words);
}

int main(int argc, char **argv)
{
    static const char *const modes[] = { "none", "noop", "drop", "reentrant" };
    unsigned lap;
    if (argc != 2)
        return 2;
    for (mode = 0; mode < 4 && strcmp(argv[1], modes[mode]); mode++) {
    }
    if (mode == 4)
        return 2;
    if (mode) {
        const ps3tc_fifo_observer o = { sizeof o, 0u, &M, observer };
        uint32_t b = 0, e = 0;
        /* the bridge's install rules: misaligned, inverted and non-contiguous rings refuse; a second install refuses */
        if (ps3tc_fifo_obs_install(&S, &M, &o, BASE + 2u, BASE + 4u * WORDS, 4u * WORDS, &b, &e) == 0 ||
            ps3tc_fifo_obs_install(&S, &M, &o, BASE + 4u * WORDS, BASE, 4u * WORDS, &b, &e) == 0 ||
            ps3tc_fifo_obs_install(&S, &M, &o, BASE, BASE + 4u * WORDS, 8u * WORDS, &b, &e) == 0 ||
            ps3tc_fifo_obs_install(&S, &M, &o, BASE, 0x20000000u, 0x20000000u - BASE, &b, &e) == 0 ||
            ps3tc_fifo_obs_install(&S, &M, &o, BASE, BASE + 4u * WORDS, 4u * WORDS, &b, &e) != 0 ||
            b != BASE || e != BASE + 4u * WORDS ||
            ps3tc_fifo_obs_install(&S, &M, &o, BASE, BASE + 4u * WORDS, 4u * WORDS, &b, &e) == 0)
            guard_ok = 0;
    }
    scenario("idle-never-flushed", 40, 0, 0, 1, 0, 9);
    scenario("slow-consumer", 40, 0, 0, 1, 50, 9);
    scenario("partial-flush-mid", 40, 16, 5, 1, 3, 17);
    scenario("whole-lap-drained", 40, 40, 40, 4, 0, 1);
    scenario("one-word-lap", 1, 0, 0, 1, 0, 3);
    scenario("last-word", WORDS - 1, 0, 0, 3, 0, 63);
    for (lap = 0; lap < 5; ++lap) {
        unsigned i, first = nev;
        printf("T scenario repeated-%u\n", lap);
        M.stalls = 7;
        M.step = 19;
        for (i = 0; i < WORDS - 1; ++i)
            M.fifo[i] = 0x10000000u | (lap << 20) | i;
        wrap(WORDS - 1, 5);
        if (mode == 1 || mode == 3)
            check_wrap(first, BASE + 4u * (WORDS - 1), 5);
    }
    if (mode == 3 && (S.suppressed == 0 || nested_delivered))
        guard_ok = 0;
    if (mode == 2 && nev)
        events_ok = 0;
    if (mode) {
        const ps3tc_fifo_observer o = { sizeof o, 0u, &M, observer };
        if (ps3tc_fifo_obs_remove(&S, &M, &o) != 0 || S.reg.event || ps3tc_fifo_obs_remove(&S, &M, &o) == 0)
            guard_ok = 0;
    }
    printf("observe mode=%s events=%s brackets=%s guard=%s delivered=%u suppressed=%llu\n", modes[mode],
           events_ok ? "ok" : "FAIL", brackets_ok ? "ok" : "FAIL", guard_ok ? "ok" : "FAIL", nev,
           (unsigned long long)S.suppressed);
    return events_ok && brackets_ok && guard_ok ? 0 : 1;
}
