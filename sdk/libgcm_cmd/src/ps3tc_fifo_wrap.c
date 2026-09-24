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
#include <sys/lv2_types.h>
#include <ppu-asm.h>
#include <rsx/gcm_sys.h>
#include <cell/gcm/ps3tc_fifo_wrap.h>

#include "ps3tc_fifo_wrap_protocol.h"

static void ps3tc_fifo_sync(void *arg)
{
    (void)arg;
    __asm__ __volatile__ ("sync" ::: "memory");
}

static void ps3tc_fifo_pause(void *arg, uint32_t want, uint32_t spins)
{
    gcmControlRegister volatile *ctrl = (gcmControlRegister volatile *)arg;
    if (spins != 0u && (spins & 0x3ffu) == 0u)
        printf("PS3TC-FIFO-WRAP waiting for GET=0x%lx: get=0x%lx put=0x%lx\n",
               (unsigned long)want, (unsigned long)ctrl->get,
               (unsigned long)ctrl->put);
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
        &ctrl->put, &ctrl->get, ps3tc_fifo_sync, ps3tc_fifo_pause, (void *)ctrl
    };
    ps3tc_fifo_wrap_protocol((volatile uint32_t *)ctx->current,
                             tail_off, begin_off, &port);

    ctx->current = ctx->begin;
    return 0;
}

void ps3tc_fifo_wrap_install(gcmContextData *ctx)
{
    if (!ctx) return;
    ctx->callback = (gcmContextCallback)(uintptr_t)lv2_fn_to_callback_ea(ps3tc_fifo_wrap_callback);
}
