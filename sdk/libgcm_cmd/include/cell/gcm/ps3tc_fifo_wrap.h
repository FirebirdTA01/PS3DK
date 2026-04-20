/*
 * PS3 Custom Toolchain — <cell/gcm/ps3tc_fifo_wrap.h>
 *
 * Native FIFO-wrap callback — replaces the firmware default installed
 * by cellGcmInit / gcmInitBodyEx.  On RPCS3 the firmware's wrap
 * callback does an incomplete sequence: it writes the JUMP command
 * back to begin but does not advance the PUT register to make the
 * JUMP visible to the GPU.  If the wrap fires at a moment when PUT
 * is already at the buffer end (which is what happens in the
 * spinning-cube test after ~140 frames of rendering), the GPU is idle
 * at PUT = GET = end, the callback spins in sys_timer_usleep
 * forever waiting for GET to wrap around, and the app deadlocks.
 *
 * Our callback takes full ownership of the sequence — it writes the
 * JUMP, advances PUT to force the GPU past it, and waits for GET to
 * reach the target.  Installed in cellGcmInit by overwriting
 * ctx->callback to point at ps3tc_fifo_wrap_callback via
 * __get_opd32 (PSL1GHT's PRX-OPD extension slot).
 */

#ifndef PS3TC_CELL_GCM_FIFO_WRAP_H
#define PS3TC_CELL_GCM_FIFO_WRAP_H

#include <stdint.h>
#include <rsx/gcm_sys.h>      /* gcmContextData */

#ifdef __cplusplus
extern "C" {
#endif

/* FIFO-wrap callback invoked when a command-emit call finds that
 * ctx->current + count would overrun ctx->end.  Wraps the FIFO
 * back to ctx->begin, waits for the GPU to follow, and returns 0
 * on success (non-zero on failure). */
int32_t  ps3tc_fifo_wrap_callback(gcmContextData *ctx, uint32_t count);

/* One-shot installer.  Overwrites ctx->callback with a PRX-OPD
 * handle to ps3tc_fifo_wrap_callback.  Call this once after
 * cellGcmInit / rsxInit has populated gGcmContext. */
void     ps3tc_fifo_wrap_install(gcmContextData *ctx);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_GCM_FIFO_WRAP_H */
