/*
 * PS3 Custom Toolchain — libgcm_cmd / ps3tc_fifo_wrap.c
 *
 * Native FIFO-wrap callback — replaces the firmware default installed
 * by cellGcmInit / gcmInitBodyEx, which on RPCS3 omits the PUT
 * advance that makes the JUMP command visible to the GPU and
 * deadlocks any app that exercises the wrap path under load
 * (reproduced with sony-cube at ~frame 145).
 *
 * Single-buffer in-place wrap — matches what PSL1GHT's
 * rsxResetCommandBuffer does end-to-end, wrapped up as a context
 * callback.  Each wrap drains the FIFO until GPU's GET has followed
 * the JUMP back to begin, which introduces an occasional frame-time
 * spike visible as a mild flicker when the wrap lands mid-frame.
 *
 * A prior double-buffered attempt in this file — splitting the FIFO
 * and alternating halves — actually made the flicker worse: halves
 * were ~2-3x smaller than the full buffer so wraps fired ~2-3x more
 * often, the drain-wait stacked up, and combined with the sample's
 * `cellGcmFinish(1)` constant-ref no-op the PPU/GPU timing went
 * bursty enough that the cube's rotation visibly jumped on every
 * wrap cycle.  Proper fix is async wrap via back-end-label / GPU
 * write-through semaphore: PPU only waits on a label the GPU
 * writes at the half's reuse-safe point, which in steady state is
 * a no-op.  That's a follow-up — for now this single-buffer
 * version is correct, honors the user's CB_SIZE, and keeps the
 * cube running indefinitely with only the mild flicker on each
 * of the ~1.25 s-spaced wraps.
 */

#include <stdint.h>
#include <sys/timer.h>
#include <ppu-asm.h>
#include <rsx/gcm_sys.h>
#include <cell/gcm/ps3tc_fifo_wrap.h>

#define PS3TC_NV40_JUMP_FLAG   0x20000000u

int32_t ps3tc_fifo_wrap_callback(gcmContextData *ctx, uint32_t count)
{
    (void)count;
    if (!ctx || !ctx->begin || !ctx->current) return -1;

    uint32_t target_off = 0;
    if (gcmAddressToOffset((void *)ctx->begin, &target_off) != 0)
        return -1;

    *(volatile uint32_t *)ctx->current = target_off | PS3TC_NV40_JUMP_FLAG;
    __asm__ __volatile__ ("sync" ::: "memory");

    gcmControlRegister volatile *ctrl = gcmGetControlRegister();
    ctrl->put = target_off;

    while (ctrl->get != target_off)
        sys_timer_usleep(30);

    ctx->current = ctx->begin;
    return 0;
}

void ps3tc_fifo_wrap_install(gcmContextData *ctx)
{
    if (!ctx) return;
    ctx->callback = (gcmContextCallback)__get_opd32(ps3tc_fifo_wrap_callback);
}
