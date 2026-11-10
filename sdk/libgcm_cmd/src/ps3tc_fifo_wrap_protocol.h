/*
 * PS3 Custom Toolchain — libgcm_cmd / ps3tc_fifo_wrap_protocol.h
 *
 * The FIFO wrap protocol, separated from the hardware so the same code runs
 * on the PPU (ps3tc_fifo_wrap.c) and in the host model test
 * (tests/sdk/fifo-wrap-protocol-test.c).  Internal; not installed.
 *
 * WHY TWO PHASES.  The wrap used to write the tail JUMP, set PUT = begin and
 * wait for GET == begin.  That is only correct if the lap being retired has
 * already been published - if PUT already covers it.  When the application
 * has not flushed since the ring last wrapped, PUT and GET both still sit at
 * begin: setting PUT = begin changes nothing, GET == begin is already true
 * (it is the IDLE state), the wait returns at once, and rewinding current to
 * begin overwrites a whole lap the GPU never fetched.  Found by the EMP team
 * (E5 log, wrap#51: current near the tail with put = get = begin, and the
 * words at begin replaced by the next packet).  GET never passing PUT is
 * true, but it says nothing about a lap that was never published.
 *
 * The protocol:
 *   1. write the JUMP-to-begin at the tail (the current write position);
 *   2. publish the lap: PUT = tail, the JUMP word itself and not past it,
 *      so the GPU fetches everything before the JUMP; wait GET == tail;
 *   3. PUT = begin: the GPU now fetches the JUMP, lands at begin and parks
 *      there (GET == PUT == begin); wait GET == begin - unambiguous now,
 *      because GET was at the tail, not at begin, when PUT moved;
 *   4. only then may the caller rewind current to begin.
 * Each phase's wait is a fetch-position wait: GET cannot pass PUT, so it
 * comes to rest exactly at the value waited for.  The waits are deliberately
 * unbounded (see ps3tc_fifo_wrap.c for why a failing return would be worse).
 *
 * A barrier separates the JUMP store from the first PUT store and follows
 * each PUT store: the FIFO is cacheable main memory and PUT an uncached
 * register, so an eieio would not order them.
 */

#ifndef PS3TC_FIFO_WRAP_PROTOCOL_H
#define PS3TC_FIFO_WRAP_PROTOCOL_H

#include <stdint.h>

#define PS3TC_NV40_JUMP_FLAG   0x20000000u

typedef struct ps3tc_fifo_port {
    volatile uint32_t *put;        /* the PUT register */
    volatile uint32_t *get;        /* the GET register */
    void (*barrier)(void *arg);    /* full memory barrier */
    uint64_t (*now_us)(void *arg);/* monotone non-decreasing microseconds; the protocol
                                     measures each phase's wait against it. The PPU
                                     fills it from the timebase; a host model fills it
                                     with a counter (a wall clock is fine too). */
    /* One wait iteration; the host model advances its consumer here. `phase`
     * is PS3TC_FIFO_PHASE_1 or PS3TC_FIFO_PHASE_2; `elapsed_us` is the time
     * since the START of that phase's wait (re-measured when phase 2 begins). */
    void (*pause)(void *arg, uint32_t want, uint32_t spins,
                  unsigned int phase, uint64_t elapsed_us);
    void *arg;
#ifdef PS3TC_FIFO_OBSERVE
    /* The observed variant only (libgcm_cmd_prof_fifo.a; docs/design/ps3dk-prof-fifo.md).  observe NULL: no
     * observation and no extra clock read, the trace of the plain protocol.  The protocol reports the four phase
     * boundaries with their timebase brackets; the caller reports WRAP_ENTER and WRAP_EXIT itself. */
    void (*observe)(void *arg, unsigned int kind, uint64_t tb_before, uint64_t tb_after);
    uint64_t (*tb)(void *arg);
#endif
} ps3tc_fifo_port;

/* Observation points.  In the default build every one expands to an empty statement, so the protocol's stores,
 * barriers, waits and pause calls are exactly the plain protocol's.  In the observed build they read the clock and
 * report: BEGIN = [before the PUT store, after the barrier that follows it]; END = [after the last wait iteration
 * (or the BEGIN's after, with none), after the GET check that succeeded].  No store, barrier or wait changes. */
#ifdef PS3TC_FIFO_OBSERVE
#define PS3TC_FIFO_OBSERVE_POINTS 1
#define PS3TC_FIFO_OBS_TB(p) ((p)->observe ? (p)->tb((p)->arg) : 0u)
#define PS3TC_FIFO_OBS_DECL uint64_t ps3tc_ob_before = 0, ps3tc_ob_last = 0
#define PS3TC_FIFO_OBS_PRE(p) (ps3tc_ob_before = PS3TC_FIFO_OBS_TB(p))
#define PS3TC_FIFO_OBS_BEGIN(p, kind) \
    do { ps3tc_ob_last = PS3TC_FIFO_OBS_TB(p); \
         if ((p)->observe) (p)->observe((p)->arg, (kind), ps3tc_ob_before, ps3tc_ob_last); } while (0)
#define PS3TC_FIFO_OBS_SPIN(p) (ps3tc_ob_last = PS3TC_FIFO_OBS_TB(p))
#define PS3TC_FIFO_OBS_END(p, kind) \
    do { if ((p)->observe) (p)->observe((p)->arg, (kind), ps3tc_ob_last, (p)->tb((p)->arg)); } while (0)
#else
#define PS3TC_FIFO_OBS_DECL do { } while (0)
#define PS3TC_FIFO_OBS_PRE(p) do { } while (0)
#define PS3TC_FIFO_OBS_BEGIN(p, kind) do { } while (0)
#define PS3TC_FIFO_OBS_SPIN(p) do { } while (0)
#define PS3TC_FIFO_OBS_END(p, kind) do { } while (0)
#endif

/* Phase numbers passed to pause.  1 = waiting for GET to reach tail after
 * the lap is published: the lap is retired up to the JUMP, and the JUMP
 * itself is not fetched yet; 2 = the JUMP is released (PUT = begin), and
 * the wait is for the GPU to fetch it and park at begin.  Kept as plain
 * values (not an enum) so the header stays dependency-free. */
#define PS3TC_FIFO_PHASE_1 1u
#define PS3TC_FIFO_PHASE_2 2u

/* Retire the current lap and leave the GPU parked at begin.  `tail` points
 * at the word the JUMP is written to (ctx->current); tail_off and begin_off
 * are the IO offsets of tail and ctx->begin.
 *
 * The protocol computes, for each phase, how long the wait on that phase
 * has lasted and hands that (plus the phase number) to pause; the hook
 * decides how to report it (the PPU prints it, the model test records it to
 * assert on it).  The wait time for a phase is measured from the moment
 * the protocol enters that phase - not from the start of the wrap - so
 * that two different stalls (e.g. a 29 s phase-1 wait vs. a later
 * phase-2 wait) cannot be confused in a log line. */
static inline void ps3tc_fifo_wrap_protocol(volatile uint32_t *tail,
                                            uint32_t tail_off,
                                            uint32_t begin_off,
                                            const ps3tc_fifo_port *p)
{
    uint32_t spins;

    PS3TC_FIFO_OBS_DECL;

    *tail = begin_off | PS3TC_NV40_JUMP_FLAG;
    p->barrier(p->arg);

    /* Phase 1: publish the lap up to, not including, the JUMP.
     * The wait's start is here (when PUT is set to the tail), not earlier. */
    const uint64_t phase1_start = p->now_us(p->arg);
    PS3TC_FIFO_OBS_PRE(p);
    *p->put = tail_off;
    p->barrier(p->arg);
    PS3TC_FIFO_OBS_BEGIN(p, 2u);
    for (spins = 0; *p->get != tail_off; ++spins) {
        p->pause(p->arg, tail_off, spins,
                 PS3TC_FIFO_PHASE_1, p->now_us(p->arg) - phase1_start);
        PS3TC_FIFO_OBS_SPIN(p);
    }
    PS3TC_FIFO_OBS_END(p, 3u);

    /* Phase 2: release the JUMP; the GPU follows it and parks at begin.
     * Re-measure the start of this phase's own wait. */
    const uint64_t phase2_start = p->now_us(p->arg);
    PS3TC_FIFO_OBS_PRE(p);
    *p->put = begin_off;
    p->barrier(p->arg);
    PS3TC_FIFO_OBS_BEGIN(p, 4u);
    for (spins = 0; *p->get != begin_off; ++spins) {
        p->pause(p->arg, begin_off, spins,
                 PS3TC_FIFO_PHASE_2, p->now_us(p->arg) - phase2_start);
        PS3TC_FIFO_OBS_SPIN(p);
    }
    PS3TC_FIFO_OBS_END(p, 5u);
}

#endif /* PS3TC_FIFO_WRAP_PROTOCOL_H */
