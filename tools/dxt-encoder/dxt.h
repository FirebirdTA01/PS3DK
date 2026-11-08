/* Portable C S3TC block encoder for build-time assets.
 * Input: row-major RGBA8 bytes. Output: little-endian DXT blocks.
 * DXT1: 8 bytes (two RGB565 endpoints and sixteen 2-bit selectors).
 * DXT3: 16 bytes (sixteen 4-bit alphas followed by color data).
 * DXT5: 16 bytes (two alpha endpoints, sixteen 3-bit alpha selectors, color).
 * Color endpoints use a simple bounding box. DXT1 ignores source alpha;
 * this API does not encode 1-bit transparency. Use DXT3/5 to retain alpha.
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
 * does not accept this enum and does not encode source alpha. */
enum dxt1_alpha_mode {
    DXT1_OPAQUE = 0,   /* no alpha; index-3 in 3-colour mode decodes black   */
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
