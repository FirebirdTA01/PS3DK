/*
 * PS3 Custom Toolchain — libgcm_cmd / ps3tc_fifo_wrap.c
 *
 * Native FIFO-wrap callback — replaces the firmware default installed
 * by cellGcmInit / gcmInitBodyEx, which on RPCS3 omits the PUT advance
 * that makes the JUMP command visible to the GPU and deadlocks any app
 * that exercises the wrap path under load.
 *
 * Concurrent full-buffer ring.  The ring spans the whole command buffer
 * (see default_fifo.c), so a wrap fires only once the CPU has produced a
 * full buffer of commands — roughly once per several frames rather than
 * several times per frame.  Between wraps the GPU runs concurrently with
 * command production: cellGcmFlush advances PUT as the app submits work.
 *
 * The wrap itself does NOT drain the FIFO.  It writes a JUMP-to-begin at
 * the write head and lowers PUT to begin.  Because the GPU executes
 * commands while GET != PUT, it keeps reading forward through the rest of
 * the buffer, follows the JUMP, and lands at begin with GET == PUT == begin
 * (idle) — or, if the app has already flushed new commands at begin, GET
 * chases the new PUT and reads them.  Either way there is no re-loop (PUT
 * is at begin, never left high over stale commands) and no PPU busy-wait,
 * so an in-stream GPU wait (semaphore-acquire / wait-label) that blocks GET
 * mid-buffer never stalls the CPU.
 *
 * The only wait is a defensive free-space guard for the rare case where the
 * CPU has lapped the GPU and is about to overwrite the begin region before
 * GET has vacated it.  With a full-buffer ring and frames much smaller than
 * the buffer this is effectively never taken; if it is, it is logged rather
 * than silently capping coverage.
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
    if (!ctx || !ctx->begin || !ctx->current) return -1;

    uint32_t begin_off = 0;   /* offset of ctx->begin (the JUMP target) */
    if (gcmAddressToOffset((void *)ctx->begin, &begin_off) != 0)
        return -1;

    /* Write the JUMP-to-begin at the write head and publish it. */
    *(volatile uint32_t *)ctx->current = begin_off | PS3TC_NV40_JUMP_FLAG;
    __asm__ __volatile__ ("sync" ::: "memory");

    {
        gcmControlRegister volatile *ctrl = gcmGetControlRegister();

        /* Lower PUT to begin.  The GPU keeps reading forward from GET
           (GET != PUT) through the remaining commands and the JUMP, then
           lands at begin.  With PUT already at begin it idles there with no
           re-loop; a subsequent cellGcmFlush advancing PUT wakes it onto the
           freshly written commands.  No drain, so the GPU stays concurrent
           and any in-stream GPU wait blocks only the GPU, never the CPU. */
        ctrl->put = begin_off;
        __asm__ __volatile__ ("sync" ::: "memory");

        /* Defensive free-space guard: do not start overwriting the begin
           region until GET has either caught up (== begin, ring fully
           consumed) or moved past the window we are about to write.  At a
           normal wrap GET is far back in the buffer tail, so this exits
           immediately. */
        {
            /* count is a word count; GET/offsets are byte offsets. */
            uint32_t window_bytes = count * (uint32_t)sizeof(uint32_t);
            uint32_t window_end = begin_off + window_bytes;
            uint32_t spins = 0;
            for (;;) {
                uint32_t g = ctrl->get;
                /* Half-open overwrite window [begin, begin+window_bytes):
                   GET == window_end is the first safe byte. */
                if (g == begin_off || g >= window_end)
                    break;
                if ((++spins & 0x3ffu) == 0u)
                    printf("PSGL-FIFO-WRAP free-space wait: get=0x%x begin=0x%x bytes=%u\n",
                           g, begin_off, window_bytes);
                sys_timer_usleep(30);
            }
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
