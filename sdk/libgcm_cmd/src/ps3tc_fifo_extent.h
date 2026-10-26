/* Internal to libgcm_cmd: default-FIFO sizing within the extent cellGcmInit
 * admitted (ps3tc_fifo_init.c). */
#ifndef PS3TC_FIFO_EXTENT_H
#define PS3TC_FIFO_EXTENT_H

#include <stdint.h>
#include <rsx/gcm_sys.h>

/* Re-admits [admitted base, base + command_bytes) with the same checks as
 * ps3tc_fifo_init_extent and moves only end, to that extent's tail JUMP word.
 * Returns -1 without mutation when nothing was admitted yet, when the request
 * exceeds the admitted command bytes, when it would put end below the live
 * end, or when any bound or translation check fails.  begin, current, the
 * callback and every command byte are never touched. */
int32_t ps3tc_fifo_resize_extent(gcmContextData *ctx, uint32_t command_bytes);

#endif
