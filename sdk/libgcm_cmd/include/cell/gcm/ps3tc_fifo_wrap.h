/*
 * PS3 Custom Toolchain — <cell/gcm/ps3tc_fifo_wrap.h>
 *
 * Native FIFO-wrap callback — replaces the firmware default installed by
 * cellGcmInit / gcmInitBodyEx.  The callback writes a tail JUMP-to-begin,
 * publishes the lap up to that JUMP and waits for GET to reach it, then
 * releases the JUMP and waits for GET to park at begin (two phases; see
 * src/ps3tc_fifo_wrap_protocol.h, t_38e8bf5a).
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

/* Internal cellGcmInit step, before installing the callback/returning success.
 * Validates the requested byte extent and its IO mapping,
 * then expands end only. Returns -1 without mutation on failed validation.
 * This is not a live resize API; calling it on an active context is unsafe.
 * The caller installs the native callback only after successful admission.
 * rsxInit has already initialized firmware and gGcmContext: rejection preserves
 * that post-rsxInit state; it does not roll initialization back. */
int32_t ps3tc_fifo_init_extent(gcmContextData *ctx, uint32_t command_bytes,
                             uint32_t io_bytes, const void *io_address);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_GCM_FIFO_WRAP_H */
