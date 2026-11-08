/*
 * PS3 Custom Toolchain — sdk/libgcm_cmd/src/convert_swizzle.c
 *
 * cellGcmConvertSwizzleFormat: CPU-side linear -> Morton-swizzled
 * texture block copy.
 *
 * 2-D bit order anchors to psgl_morton2
 * (sdk/libPSGL/src/psgl_context.c:251):
 *     x -> even bit positions (0, 2, 4, ...)
 *     y -> odd  bit positions (1, 3, 5, ...)
 *
 * 3-D bit order is NOT pinned to a measured reference.  The
 * implementation interleaves x/y/z one bit at a time with x first:
 *     x -> bits 0, 3, 6, 9, ...
 *     y -> bits 1, 4, 7, 10, ...
 *     z -> bits 2, 5, 8, 11, ...
 * OPEN: not confirmed against the RSX hardware spec.
 *
 * dstWidth / dstHeight / dstDepth are the FULL swizzled texture
 * dimensions in texel units, used to compute tile coordinates.
 *
 * dstLog2cdepth: log2 of destination texel byte width.
 *     0 -> 1 byte, 1 -> 2 bytes, 2 -> 4 bytes, 3 -> 8 bytes.
 * OPEN: the parameter's byte-versus-bit unit still needs confirmation.
 *
 * border: meaning not pinned.  Ignored (treated as 0).
 * OPEN: listed in the commit message.
 */

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Internal Morton helpers — NOT part of the public API.  They mirror
 * psgl_morton2 (libPSGL) for the 2-D case so the 2-D result is
 * bit-identical to PSGL's tiling.  The 3-D interleaving is a
 * conventional generalisation and is flagged OPEN.
 * ------------------------------------------------------------------ */

static uint32_t swz_morton2(uint32_t x, uint32_t y)
{
    uint32_t out = 0u;
    for (uint32_t bit = 0u; bit < 16u; ++bit) {
        out |= ((x >> bit) & 1u) << (bit * 2u);
        out |= ((y >> bit) & 1u) << (bit * 2u + 1u);
    }
    return out;
}

static uint32_t swz_morton3(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t out = 0u;
    for (uint32_t bit = 0u; bit < 11u; ++bit) {
        out |= ((x >> bit) & 1u) << (bit * 3u);
        out |= ((y >> bit) & 1u) << (bit * 3u + 1u);
        out |= ((z >> bit) & 1u) << (bit * 3u + 2u);
    }
    return out;
}

/* ------------------------------------------------------------------ *
 * Public entry point
 * ------------------------------------------------------------------ */

void cellGcmConvertSwizzleFormat(void *swizzledTexture, const void *texture,
                                 uint32_t dstx0, uint32_t dsty0, uint32_t dstz0,
                                 uint32_t dstWidth, uint32_t dstHeight,
                                 uint32_t dstDepth,
                                 uint32_t srcx0, uint32_t srcy0, uint32_t srcz0,
                                 uint32_t srcx1, uint32_t srcy1, uint32_t srcz1,
                                 uint32_t srcWidth, uint32_t srcHeight,
                                 uint32_t srcDepth,
                                 uint32_t dstLog2cdepth, uint32_t srcColordepth,
                                 uint32_t border, uint32_t dim,
                                 void (*copyTexel)(void *dst, const void *src))
{
    (void)border;       /* OPEN: semantics not pinned; ignored */
    (void)srcDepth;     /* full linear depth; addressing uses srcWidth/srcHeight */
    (void)dstDepth;     /* only used implicitly via srcz1/srcz0 in 3-D */
    if (!swizzledTexture || !texture) return;

    const uint32_t tw = (dstLog2cdepth < 32u) ? (1u << dstLog2cdepth) : 1u;
    const uint8_t *src = (const uint8_t *)texture;
    uint8_t       *dst = (uint8_t *)swizzledTexture;

    switch (dim) {

    /* --------------------------------------------------------- 2-D  */
    case 2: {
        /* Within-tile byte offset = swz_morton2(texel_x, texel_y) * texel_bytes.
         * This mirrors psgl_context.c:2112 which writes
         *   ((uint32_t*)addr)[psgl_morton2(dst_x, dst_y)] = value;
         * i.e. byte offset = psgl_morton2(texel_x, texel_y) * texel_size.
         * psgl_morton2 interleaves x on even bits, y on odd bits.
         *
         * Tile = one 16-KiB block (RSX 2-D tile).  A tile holds
         * (128/tw) x (128/tw) texels of tw-byte width, which is
         * 128*128 = 16384 bytes in any format.                    */
        const uint32_t tile_t = (tw > 0u) ? (128u / tw) : 0u;
        if (tile_t == 0u) return;
        const uint32_t tiles_w = (dstWidth > 0u) ? (dstWidth / tile_t) : 0u;
        if (tiles_w == 0u) return;

        const uint32_t bcount = (srcy1 > srcy0) ? (srcy1 - srcy0) : 0u;
        const uint32_t bwidth = (srcx1 > srcx0) ? (srcx1 - srcx0) : 0u;
        for (uint32_t dy = 0u; dy < bcount; ++dy) {
            for (uint32_t dx = 0u; dx < bwidth; ++dx) {
                const uint32_t sx = srcx0 + dx;
                const uint32_t sy = srcy0 + dy;
                const uint32_t X  = dstx0 + dx;
                const uint32_t Y  = dsty0 + dy;
                const uint32_t tx = X / tile_t;
                const uint32_t ty = Y / tile_t;
                /* Within tile: use texel coords directly (not byte coords).
                 * The tile size in bytes is fixed at 16 KiB = 16384 regardless
                 * of tw, because 128 bytes * 128 bytes = 16 KiB and each tile
                 * spans 128/tw x 128/tw texels.  The within-tile byte offset
                 * is psgl_morton2(texel_x, texel_y) * tw. */
                const size_t tile_off = (size_t)swz_morton2(X % tile_t, Y % tile_t)
                                       * tw;
                const size_t off = (size_t)(ty * tiles_w + tx) * 16384u
                                 + tile_off;
                uint8_t       *dp  = dst + off;
                const uint8_t *sp  = src + (size_t)sy * srcWidth * srcColordepth
                                       + (size_t)sx * srcColordepth;
                if (copyTexel) copyTexel(dp, sp);
                else           memcpy(dp, sp, srcColordepth);
            }
        }
        break;
    }

    /* --------------------------------------------------------- 3-D  */
    case 3: {
        /* OPEN: 3-D Morton order and tiling are NOT confirmed against a
         * reference.  The model below is self-consistent (no two texels
         * collide, every written byte stays inside a 2-MiB 3-D tile) and
         * generalises the measured 2-D rule, but nothing in-tree has
         * validated it.  Flagged in the commit message.
         *
         * Tile = 128 x 128 x 128 bytes = 2 MiB (1 << 21).
         * Within-tile byte offset = swz_morton3(sbx, sby, sbz) with
         * each byte coordinate in [0,128) — the 3-D generalisation of
         * psgl_morton2.                                             */
        const uint64_t TILE3 = 1u << 21;          /* 2 MiB */
        const uint32_t tile_t = (tw > 0u) ? (128u / tw) : 0u;
        if (tile_t == 0u) return;
        const uint32_t tiles_w = (dstWidth  > 0u) ? (dstWidth  / tile_t) : 0u;
        const uint32_t tiles_h = (dstHeight > 0u) ? (dstHeight / tile_t) : 0u;
        const uint32_t tiles_d = (dstDepth  > 0u) ? (dstDepth  / tile_t) : 0u;
        if (tiles_w == 0u || tiles_h == 0u || tiles_d == 0u) return;

        const uint32_t bz = srcz1 - srcz0;
        const uint32_t by = srcy1 - srcy0;
        const uint32_t bx = srcx1 - srcx0;
        for (uint32_t dz = 0u; dz < bz; ++dz) {
            for (uint32_t dy = 0u; dy < by; ++dy) {
                for (uint32_t dx = 0u; dx < bx; ++dx) {
                    const uint32_t sx = srcx0 + dx;
                    const uint32_t sy = srcy0 + dy;
                    const uint32_t sz = srcz0 + dz;
                    const uint32_t X  = dstx0 + dx;
                    const uint32_t Y  = dsty0 + dy;
                    const uint32_t Z  = dstz0 + dz;
                    const uint32_t tx = X / tile_t;
                    const uint32_t ty = Y / tile_t;
                    const uint32_t tz = Z / tile_t;
                    const uint32_t sbx = (X % tile_t) * tw;
                    const uint32_t sby = (Y % tile_t) * tw;
                    const uint32_t sbz = (Z % tile_t) * tw;
                    const uint64_t tile_base =
                        ((uint64_t)tz * tiles_h + ty) * tiles_w + tx;
                    const size_t off = (size_t)(tile_base * TILE3)
                                     + swz_morton3(sbx, sby, sbz);
                    uint8_t       *dp  = dst + off;
                    const uint8_t *sp  = src +
                        (size_t)sz * srcWidth * srcHeight * srcColordepth
                      + (size_t)sy * srcWidth * srcColordepth
                      + (size_t)sx * srcColordepth;
                    if (copyTexel) copyTexel(dp, sp);
                    else           memcpy(dp, sp, srcColordepth);
                }
            }
        }
        break;
    }

    default:
        /* unsupported dim value; do nothing */
        break;
    }
}
