/*
 * PS3 Custom Toolchain — libgcm_cmd / ps3tc_fifo_wrap.c
 *
 * Native FIFO-wrap callback — replaces the firmware default installed
 * by cellGcmInit / gcmInitBodyEx, which on RPCS3 omits the PUT
 * advance that makes the JUMP command visible to the GPU and
 * deadlocks any app that exercises the wrap path under load
 * (reproduced with the spinning-cube test at ~frame 145).
 *
 * Single-buffer in-place wrap — matches what PSL1GHT's
 * rsxResetCommandBuffer does end-to-end, wrapped up as a context
 * callback.  Each wrap drains the FIFO until GPU's GET has followed
 * the JUMP back to begin, which introduces an occasional frame-time
 * spike visible as a mild flicker when the wrap lands mid-frame.
 *
 * Three async/double-buffered attempts have been tried and reverted;
 * see docs/known-issues.md ("FIFO wrap drain-wait causes occasional
 * frame flicker") for the iteration log and the constraints the
 * eventual fix has to satisfy.  The single-buffer version here is
 * correct, honors the user's CB_SIZE, and keeps the spinning-cube
 * test running indefinitely with only the mild per-wrap flicker.
 */

#include <stdint.h>
#include <sys/timer.h>
#include <sys/lv2_types.h>
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
    ctx->callback = (gcmContextCallback)(uintptr_t)lv2_fn_to_callback_ea(ps3tc_fifo_wrap_callback);
}
