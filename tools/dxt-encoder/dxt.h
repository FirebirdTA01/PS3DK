/*
 * PS3 Custom Toolchain — tools/dxt-encoder/dxt.h
 *
 * Standard S3TC (DirectX Texture Co-compression) block encoder, plain C,
 * for the asset pipeline (t_a229ec8f patch 3).  Runs at asset BUILD time
 * on the host; speed is not critical.  Each 4x4 block of an RGBA8 source
 * is compressed to the standard DXT1 / DXT3 / DXT5 block layout that the
 * SDK's NV40 DXT1/DXT3/DXT5 texture formats expect:
 *
 *   DXT1 : 8  bytes/block — C0, C1 (5:6:5) + 16 four-bit colour indices
 *   DXT3 : 16 bytes/block — 16 four-bit alpha values + DXT1 colour
 *   DXT5 : 16 bytes/block — A0, A1 (8-bit) + 16 three-bit alpha indices
 *                           + DXT1 colour
 *
 * Endpoint selection is the bounding-box method (min/max RGB, min/max A);
 * it is "good enough" per the task spec and is what most reference
 * encoders use for a first pass.
 *
 * "DXT1 with 1-bit alpha" and "DXT1 without" (opaque) share this encoder:
 * the 8-byte block is byte-for-byte identical in both; the two modes differ
 * only in how a CONSUMER decodes palette index 3 in the 3-colour mode
 * (transparent vs opaque).  The encoder emits the standard block and the
 * caller chooses the mode at consumption time, so only one encoder is needed.
 *
 * No SDK / PPU dependencies — compiles with any C11 compiler.
 */
#ifndef PS3TC_DXT_ENCODER_H
#define PS3TC_DXT_ENCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes per 4x4 block, per format. */
#define DXT1_BYTES_PER_BLOCK 8u
#define DXT3_BYTES_PER_BLOCK 16u
#define DXT5_BYTES_PER_BLOCK 16u

/* Alpha handling for the DXT1 (8-byte) mode at consume time.  The encoder
 * output is identical for both; this value is informational for documents. */
enum dxt1_alpha_mode {
    DXT1_OPAQUE = 0,   /* no alpha; index-3 in 3-colour mode maps to C1   */
    DXT1_ONE_BIT      /* 1-bit alpha; index-3 in 3-colour mode is clear   */
};

/* Encode one 4x4 block.
 *   src : 16 RGBA8 texels (4 bytes each, 64 bytes total), row-major,
 *         order (x0,y0)..(x3,y0), (x0,y1)..: i.e. src + (y*4 + x)*4.
 *   dst : DXT1_BYTES_PER_BLOCK (8) for dxt1.
 * Returns 1 on success, 0 if src is NULL. */
int dxt1_encode_block(const uint8_t *src, uint8_t *dst);

/* Encode one 4x4 block to DXT3 (16 bytes: explicit 4-bit alpha + DXT1 col). */
int dxt3_encode_block(const uint8_t *src, uint8_t *dst);

/* Encode one 4x4 block to DXT5 (16 bytes: interpolated alpha + DXT1 col). */
int dxt5_encode_block(const uint8_t *src, uint8_t *dst);

/* Whole-image encoders.
 *   rgba : RGBA8, width*height*4 bytes, row-major, row stride = width*4.
 *   dst  : receives ceil(width/4)*ceil(height/4) blocks, packed tightly
 *          (no mip chain).
 *   width/height in texels.  Output size (bytes) is returned.
 * Returns 0 bytes if width/height/rgba are invalid. */
size_t dxt1_encode_image(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *dst);
size_t dxt3_encode_image(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *dst);
size_t dxt5_encode_image(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_DXT_ENCODER_H */
