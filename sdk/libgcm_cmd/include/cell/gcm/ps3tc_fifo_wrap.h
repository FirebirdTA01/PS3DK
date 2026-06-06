/*
 * PS3 Custom Toolchain — <cell/gcm/ps3tc_fifo_wrap.h>
 *
 * Native FIFO-wrap callback — replaces the firmware default installed by
 * cellGcmInit / gcmInitBodyEx.  The callback writes a tail JUMP-to-begin,
 * publishes PUT at begin, and drains until GET reaches begin.
 */

#ifndef PS3TC_CELL_GCM_FIFO_WRAP_H
#define PS3TC_CELL_GCM_FIFO_WRAP_H

#include <stdint.h>
#include <rsx/gcm_sys.h>      /* gcmContextData */

#ifdef __cplusplus
extern "C" {
#endif

/* FIFO-wrap callback invoked when a command-emit call finds that
 * ctx->current + count would overrun ctx->end.  Wraps the FIFO back to
 * ctx->begin and returns 0 on success (non-zero on failure). */
int32_t  ps3tc_fifo_wrap_callback(gcmContextData *ctx, uint32_t count);

/* One-shot installer.  Overwrites ctx->callback with a PRX-OPD
 * handle to ps3tc_fifo_wrap_callback.  Call this once after
 * cellGcmInit / rsxInit has populated gGcmContext. */
void     ps3tc_fifo_wrap_install(gcmContextData *ctx);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_GCM_FIFO_WRAP_H */
