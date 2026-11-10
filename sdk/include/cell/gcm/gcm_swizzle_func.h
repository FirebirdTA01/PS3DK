/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_swizzle_func.h>
 *
 * cellGcmConvertSwizzleFormat: CPU-side linear -> Morton-swizzled
 * texture block copy.
 *
 * Canonical platform API (matches the PS3 firmware's cellGcmConvert-
 * SwizzleFormat).  Implemented in sdk/libgcm_cmd/src/convert_swizzle.c
 * (libgcm_cmd.a).
 *
 * NOT declared in cell/texture.h — that header is not part of this
 * SDK and the lead has explicitly rejected this placement (room
 * message 2026-11-07 19:50, t_a229ec8f).
 *
 * Parameter list is the canonical 20-uint32 + copyTexel form.
 *
 * 2-D within-tile byte offset is anchored to psgl_morton2
 * (sdk/libPSGL/src/psgl_context.c:251, used at :2112):
 *     byte_off  = swz_morton2(texel_x, texel_y) * texel_bytes
 *     x -> even bits (0, 2, 4, ...), y -> odd bits (1, 3, 5, ...)
 *
 * OPEN (recorded in the commit message and not hidden here):
 *   - dstLog2cdepth: assumed log2 of destination texel BYTE width
 *     (0 -> 1 B, 1 -> 2 B, 2 -> 4 B, 3 -> 8 B).  This is the SDK's
 *     existing "color depth" spelling but is NOT independently
 *     confirmed as the canonical meaning of this parameter.
 *   - border: ignored.  Semantics not pinned.
 *   - 3-D Morton order (bits interleaved x, y, z in that order) is
 *     a generalisation of the measured 2-D rule and has NOT been
 *     verified on RSX hardware.
 */
#ifndef PS3TC_CELL_GCM_SWIZZLE_FUNC_H
#define PS3TC_CELL_GCM_SWIZZLE_FUNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cellGcmConvertSwizzleFormat(void *swizzledTexture, const void *texture,
                                 uint32_t dstx0, uint32_t dsty0, uint32_t dstz0,
                                 uint32_t dstWidth, uint32_t dstHeight,
                                 uint32_t dstDepth,
                                 uint32_t srcx0, uint32_t srcy0, uint32_t srcz0,
                                 uint32_t srcx1, uint32_t srcy1, uint32_t srcz1,
                                 uint32_t srcWidth, uint32_t srcHeight,
                                 uint32_t srcDepth,
                                 uint32_t dstLog2cdepth,
                                 uint32_t srcColordepth,
                                 uint32_t border,
                                 uint32_t dim,
                                 void (*copyTexel)(void *dst, const void *src));

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_GCM_SWIZZLE_FUNC_H */
