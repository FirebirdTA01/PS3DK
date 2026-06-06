/*
 * PS3 Custom Toolchain — libgcm_cmd / ps3tc_fifo_wrap.c
 *
 * Native FIFO-wrap callback — replaces the firmware default installed
 * by cellGcmInit / gcmInitBodyEx.
 *
 * The full-buffer ring wraps by writing a JUMP-to-begin at the tail, publishing
 * PUT at begin, and draining until GET follows the JUMP to begin.  The drain is
 * deliberate: returning before GET reaches begin lets the CPU overwrite the
 * low FIFO region while the GPU can still fetch stale prior-lap words there.
 */

#include <stdint.h>
#include <stdio.h>
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

    uint32_t begin_off = 0;   /* offset of ctx->begin (the JUMP target) */
    if (gcmAddressToOffset((void *)ctx->begin, &begin_off) != 0)
        return -1;

    *(volatile uint32_t *)ctx->current = begin_off | PS3TC_NV40_JUMP_FLAG;
    __asm__ __volatile__ ("sync" ::: "memory");

    {
        gcmControlRegister volatile *ctrl = gcmGetControlRegister();
        uint32_t spins = 0;

        ctrl->put = begin_off;
        __asm__ __volatile__ ("sync" ::: "memory");

        while (ctrl->get != begin_off) {
            if ((++spins & 0x3ffu) == 0u)
                printf("PSGL-FIFO-WRAP drain wait: get=0x%lx begin=0x%lx\n",
                       (unsigned long)ctrl->get, (unsigned long)begin_off);
            sys_timer_usleep(30);
        }
    }

    ctx->current = ctx->begin;
    return 0;
}

void ps3tc_fifo_wrap_install(gcmContextData *ctx)
{
    if (!ctx) return;
    ctx->callback = (gcmContextCallback)(uintptr_t)lv2_fn_to_callback_ea(ps3tc_fifo_wrap_callback);
}
