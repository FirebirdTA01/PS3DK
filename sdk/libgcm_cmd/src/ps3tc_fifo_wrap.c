/*
 * PS3 Custom Toolchain — libgcm_cmd / ps3tc_fifo_wrap.c
 *
 * Native FIFO-wrap callback — replaces the firmware default installed
 * by cellGcmInit / gcmInitBodyEx.
 *
 * The ring wraps in two phases - publish the lap up to the tail JUMP and
 * wait for GET to reach it, then release the JUMP and wait for GET to park at
 * begin.  See ps3tc_fifo_wrap_protocol.h for the protocol and for the
 * command-loss defect of the one-phase version it replaces (t_38e8bf5a).
 */

#include <stdint.h>
#include <stdio.h>
#include <sys/timer.h>
#include <sys/systime.h>
#include <sys/lv2_types.h>
#include <ppu-asm.h>
#include <rsx/gcm_sys.h>
#include <cell/gcm/ps3tc_fifo_wrap.h>

#include "ps3tc_fifo_wrap_protocol.h"

/* Wrap statistics (ps3tc_fifo_wrap_stats).  The callback runs on the thread
 * that emits into the context, so plain counters suffice; ticks are
 * timebase ticks, converted when read. */
static uint64_t wrap_count, wrap_ticks, wrap_max_ticks;

/* Timebase ticks to microseconds without overflowing: ticks * 1000000
 * alone wraps past about 1.8e13 ticks (some 64 hours of accumulated wait at
 * 79.8 MHz). */
static uint64_t ticks_to_us(uint64_t ticks, uint64_t hz)
{
    return (ticks / hz) * 1000000ull + (ticks % hz) * 1000000ull / hz;
}

/* The protocol's clock, in microseconds, measured from a fixed origin:
 * __builtin_ppc_get_timebase() divided into us by the timebase frequency.
 * The function-pointer form (arg ignored here) lets the host model install
 * its own clock.  We re-read the frequency on each call on purpose: the
 * call is cheap (a cached syscall on a PS3 kernel) and it keeps this
 * function stateless across threads. */
static uint64_t ps3tc_fifo_now_us(void *arg)
{
    (void)arg;
    const uint64_t hz = sysGetTimebaseFrequency();
    return hz ? ticks_to_us(__builtin_ppc_get_timebase(), hz) : 0;
}

static void ps3tc_fifo_sync(void *arg)
{
    (void)arg;
    __asm__ __volatile__ ("sync" ::: "memory");
}

/* Wait iteration hook.  The protocol passes the phase (1 or 2) and how
 * long (in microseconds, measured from the start of THAT phase's wait)
 * the wait has already lasted.  The old signature used want/spins only
 * and the diagnostic said nothing about which phase or for how long -
 * a 29 s phase-1 wait during a game's world load looked identical to a
 * later stall.  The rate limit (print every 1024 spins) and the
 * 30-us sleep are unchanged.  Phase 1 = waiting for GET == tail (the
 * JUMP fetch); phase 2 = waiting for GET == begin. */
static void ps3tc_fifo_pause(void *arg, uint32_t want, uint32_t spins,
                             unsigned int phase, uint64_t elapsed_us)
{
    gcmControlRegister volatile *ctrl = (gcmControlRegister volatile *)arg;
    if (spins != 0u && (spins & 0x3ffu) == 0u)
        printf("PS3TC-FIFO-WRAP phase %u waiting for GET=0x%lx: get=0x%lx put=0x%lx (%lu ms)\n",
               (unsigned)phase, (unsigned long)want, (unsigned long)ctrl->get,
               (unsigned long)ctrl->put, (unsigned long)(elapsed_us / 1000ull));
    sys_timer_usleep(30);
}

/*
 * Return contract, and why the waits are unbounded: the reserve macro
 * (RSX_CONTEXT_CURRENT_BEGIN) treats a nonzero return by RETURNING WITHOUT
 * EMITTING - the packet is silently dropped.  So a wait that gave up and
 * returned an error would turn a stall into silent command loss, the very
 * defect this fixes.  The waits spin with periodic diagnostics instead, and
 * the one condition that cannot be satisfied at all - a reserve larger than
 * the empty ring - halts rather than returning.
 *
 * NOT changed by this fix, and still silent drops through that contract: a
 * NULL context, a NULL begin/current, and an address that does not map to
 * an IO offset return -1 as before.  None is reachable from a context that
 * cellGcmInit set up.
 */
int32_t ps3tc_fifo_wrap_callback(gcmContextData *ctx, uint32_t count)
{
    if (!ctx || !ctx->begin || !ctx->current) return -1;

    uint32_t begin_off = 0;   /* offset of ctx->begin (the JUMP target) */
    uint32_t tail_off  = 0;   /* offset of the word the JUMP goes into  */
    if (gcmAddressToOffset((void *)ctx->begin, &begin_off) != 0)
        return -1;
    if (gcmAddressToOffset((void *)ctx->current, &tail_off) != 0)
        return -1;

    /* After the wrap the reserve resumes at begin; a request that does not
     * fit the EMPTY ring would be written past end.  count is in words. */
    if ((uint64_t)(ctx->end - ctx->begin) < (uint64_t)count) {
        printf("PS3TC-FIFO-WRAP: reserve of %lu words exceeds the %lu-word ring; halting\n",
               (unsigned long)count, (unsigned long)(ctx->end - ctx->begin));
        __builtin_trap();
    }

    gcmControlRegister volatile *ctrl = gcmGetControlRegister();
    const ps3tc_fifo_port port = {
        &ctrl->put, &ctrl->get, ps3tc_fifo_sync, ps3tc_fifo_now_us,
        ps3tc_fifo_pause, (void *)ctrl
    };
    const uint64_t t0 = __builtin_ppc_get_timebase();
    ps3tc_fifo_wrap_protocol((volatile uint32_t *)ctx->current,
                             tail_off, begin_off, &port);
    const uint64_t ticks = __builtin_ppc_get_timebase() - t0;
    wrap_count++;
    wrap_ticks += ticks;
    if (ticks > wrap_max_ticks)
        wrap_max_ticks = ticks;

    ctx->current = ctx->begin;
    return 0;
}

void ps3tc_fifo_wrap_stats(uint64_t *wraps, uint64_t *stall_us, uint64_t *max_stall_us)
{
    const uint64_t hz = sysGetTimebaseFrequency();
    if (wraps)
        *wraps = wrap_count;
    if (stall_us)
        *stall_us = hz ? ticks_to_us(wrap_ticks, hz) : 0;
    if (max_stall_us)
        *max_stall_us = hz ? ticks_to_us(wrap_max_ticks, hz) : 0;
}

void ps3tc_fifo_wrap_stats_reset(void)
{
    wrap_count = 0;
    wrap_ticks = 0;
    wrap_max_ticks = 0;
}

void ps3tc_fifo_wrap_install(gcmContextData *ctx)
{
    if (!ctx) return;
    ctx->callback = (gcmContextCallback)(uintptr_t)lv2_fn_to_callback_ea(ps3tc_fifo_wrap_callback);
}
