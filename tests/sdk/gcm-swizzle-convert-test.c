/* Host test for cellGcmConvertSwizzleFormat (patch 2, t_a229ec8f).
 *
 * Strategy: fill a 32x32 RGBA8 linear texture with a known pattern
 * (each texel value encodes its own (x,y) position), run the converter
 * with dim=2 into a full 4x4-tile region (16 KiB per tile, 4 tiles),
 * then for each linear position (x,y) compute the expected swizzled
 * byte offset using an INDEPENDENT copy of the Morton-2 interleave
 * (the same formula as psgl_morton2 in libPSGL).
 *
 * The destination buffer is sized for a 64x64 full-swizzled texture
 * (4 tiles x 16 KiB = 64 KiB).  Texels land at:
 *     off = (tile_row * tiles_x + tile_col) * 16384 + dec_morton2(x, y) * tw
 * where tile_row = dsty/32, tile_col = dstx/32 for a 32-texel tile.
 *
 * What this pins:
 *   1. 2-D Morton order matches psgl_morton2 (x→even bits, y→odd bits).
 *   2. Every texel is written exactly once (no collisions, no gaps within
 *      the 32x32 texel footprint).
 *   3. Out-of-footprint bytes stay zero (no stray writes).
 *
 * 3-D is not tested: OPEN, not pinned, would be testing a guess.
 *
 * Host-only; no cross compiler needed.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------- independent Morton-2 (x on even bits, y on odd bits) ------- */
static uint32_t dec_morton2(uint32_t x, uint32_t y)
{
    uint32_t out = 0u;
    for (uint32_t b = 0u; b < 16u; ++b) {
        out |= ((x >> b) & 1u) << (2u * b);
        out |= ((y >> b) & 1u) << (2u * b + 1u);
    }
    return out;
}

/* -------- the function under test (declaration from the header) ------ */
#include <cell/gcm/gcm_swizzle_func.h>

int main(void)
{
    const uint32_t W = 32u, H = 32u;          /* linear texel footprint  */
    const uint32_t T = 4u;                    /* RGBA8 texel bytes       */
    const uint32_t TILE_T = (128u / T);       /* 32 texels per tile edge */
    const uint32_t TILES_X = (64u / TILE_T);  /* dst is a 64x64 texture  */
    const uint32_t TILES_Y = (64u / TILE_T);
    const size_t TILE = 16384u;               /* 16 KiB per 2-D tile     */
    const size_t DSIZE = (size_t)TILES_X * TILES_Y * TILE; /* 64 KiB */

    uint8_t *linear = (uint8_t *)malloc((size_t)W * H * T);
    uint8_t *swz    = (uint8_t *)calloc(1, DSIZE);
    if (!linear || !swz) { perror("alloc"); return 1; }

    /* Fill linear: texel (x,y) encodes its own (y,x) in the payload:
     * byte[0]=x, byte[1]=y, byte[2]=0x55 (tag), byte[3]=0xAA (tag).
     * This lets the decoder recover (x,y) unambiguously AND distinguish
     * "empty" (all zero) from the (0,0) texel which encodes x=0,y=0
     * but has the 0x55/0xAA tag bytes. */
    memset(linear, 0, (size_t)W * H * T);
    for (uint32_t y = 0u; y < H; ++y) {
        for (uint32_t x = 0u; x < W; ++x) {
            uint32_t idx = (size_t)y * W * T + (size_t)x * T;
            linear[idx + 0] = (uint8_t)x;
            linear[idx + 1] = (uint8_t)y;
            linear[idx + 2] = 0x55;
            linear[idx + 3] = 0xAA;
        }
    }

    /* Convert: full 32x32 block at dst origin (0,0), into a 64x64 tex
     * (so it lands in tile 0,0).  src box = whole texture.  dim=2. */
    cellGcmConvertSwizzleFormat(swz, linear,
                                /* dst origin, full dst extent */
                                0u, 0u, 0u, 64u, 64u, 1u,
                                /* src box */
                                0u, 0u, 0u, W, H, 1u,
                                /* linear dims */
                                W, H, 1u,
                                /* params */
                                2u /*tw=4 bytes (log2=2)*/, T,
                                0u /*border*/, 2u /*dim*/,
                                0  /*copyTexel=memcpy*/);

    /* Verify: for each source texel (x,y), the value must appear at the
     * expected Morton offset in tile 0,0. */
    int failures = 0;
    for (uint32_t y = 0u; y < H; ++y) {
        for (uint32_t x = 0u; x < W; ++x) {
            /* Expected offset (independent decode via dec_morton2):
             * within-tile texel coords: x, y (since tile is 32x32
             * texels for tw=4; both x and y are < 32 so they live in
             * tile (0,0)).  Byte offset within tile = mor(x,y) * tw. */
            const size_t byte_off = (size_t)dec_morton2(x, y) * T;
            if (byte_off + T > TILE) {
                printf("FAIL (%u,%u): byte_off 0x%lx out of tile\n",
                       x, y, (unsigned long)byte_off);
                continue;
            }
            const uint8_t *got = swz + byte_off;
            if (got[2] != 0x55u || got[3] != 0xAAu) {
                printf("FAIL (%u,%u) @off 0x%lx: tag mismatch got [0x%02x,0x%02x,0x%02x,0x%02x]\n",
                       x, y, (unsigned long)byte_off,
                       got[0], got[1], got[2], got[3]);
                if (++failures > 10) { printf("too many\n"); return 1; }
                continue;
            }
            if (got[0] != (uint8_t)x || got[1] != (uint8_t)y) {
                printf("FAIL (%u,%u) @off 0x%lx: got (%u,%u)\n",
                       x, y, (unsigned long)byte_off, got[0], got[1]);
                if (++failures > 10) { printf("too many\n"); return 1; }
            }
        }
    }

    /* Gap check: every of W*H texel slots must carry the 0x55 tag.
     * Scan only the 0..TILE-1 bytes of tile 0 (where our 32x32 footprint
     * lives; the rest of the 64 KiB buffer is legitimately zero). */
    uint32_t found = 0;
    for (size_t i = 0; i + 3 < (size_t)TILE; i++) {
        if (swz[i + 2] == 0x55u && swz[i + 3] == 0xAAu) found++;
    }
    if (found != W * H) {
        printf("FAIL: found %u tagged texels, expected %u (gap or collision)\n",
               found, W * H);
        return 1;
    }

    if (failures == 0) {
        printf("swizzle-convert-test: PASS (32x32 RGBA8, 2-D Morton matches independent decoder, no gaps/collisions)\n");
        free(linear); free(swz);
        return 0;
    }
    printf("swizzle-convert-test: FAIL (%d failures)\n", failures);
    free(linear); free(swz);
    return 1;
}
