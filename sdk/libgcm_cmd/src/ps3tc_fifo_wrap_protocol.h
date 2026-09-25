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
    void (*pause)(void *arg,       /* one wait iteration; the host model */
                  uint32_t want,   /* advances its consumer here */
                  uint32_t spins);
    void *arg;
} ps3tc_fifo_port;

/* Retire the current lap and leave the GPU parked at begin.  `tail` points
 * at the word the JUMP is written to (ctx->current); tail_off and begin_off
 * are the IO offsets of tail and ctx->begin. */
static inline void ps3tc_fifo_wrap_protocol(volatile uint32_t *tail,
                                            uint32_t tail_off,
                                            uint32_t begin_off,
                                            const ps3tc_fifo_port *p)
{
    uint32_t spins;

    *tail = begin_off | PS3TC_NV40_JUMP_FLAG;
    p->barrier(p->arg);

    /* Phase 1: publish the lap up to, not including, the JUMP. */
    *p->put = tail_off;
    p->barrier(p->arg);
    for (spins = 0; *p->get != tail_off; ++spins)
        p->pause(p->arg, tail_off, spins);

    /* Phase 2: release the JUMP; the GPU follows it and parks at begin. */
    *p->put = begin_off;
    p->barrier(p->arg);
    for (spins = 0; *p->get != begin_off; ++spins)
        p->pause(p->arg, begin_off, spins);
}

#endif /* PS3TC_FIFO_WRAP_PROTOCOL_H */
