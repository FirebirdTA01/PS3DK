/*
 * PS3 Custom Toolchain — tools/dxt-encoder/dxt.c
 *
 * Standard S3TC block encoder (see dxt.h for the format contract).
 *
 * Endpoint selection: bounding-box (min/max per channel).  For DXT1 the two
 * endpoints are the block's colour min and max.  For DXT3/DXT5 the alpha
 * endpoints are the block's alpha min and max.
 *
 * Palette (the part the independent test decoder must reverse exactly):
 *   4-colour mode (c0 > c1):  c0, c1, (2*c0+c1)/3, (c0+2*c1)/3
 *   3-colour mode (c0 <= c1): c0, c1, (c0+c1)/2, transparent (0,0,0,0)
 * Indices 0..3 map 1:1 to those four entries.
 *
 * All output is little-endian (matches the RSX / PS3 byte order for these
 * methods: 5:6:5 packed C0=0x0000, C1=0x0002, low=0x0004, high=0x0006).
 */
#include "dxt.h"
#include <string.h>
#include <limits.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* ---------- 5:6:5 pack/unpack ---------------------------------------- */
static uint16_t pack565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((((uint32_t)(((int)r >> 3) & 0x1f)) << 11)
                 | (((uint32_t)(((int)g >> 2) & 0x3f)) << 5)
                 |  (((uint32_t)(((int)b >> 3) & 0x1f))));
}

static void unpack565(uint16_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t rr = (v >> 11) & 0x1f;
    uint32_t gg = (v >> 5)  & 0x3f;
    uint32_t bb = (v       ) & 0x1f;
    *r = (uint8_t)((rr * 255u) >> 5);   /* expand 5-bit -> 8-bit */
    *g = (uint8_t)((gg * 255u) >> 6);   /* expand 6-bit -> 8-bit */
    *b = (uint8_t)((bb * 255u) >> 5);
}

/* ---------- byte helpers --------------------------------------------- */
static void put16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)(v >> 8); }

/* ---------- DXT1 block encoder (core; reused by DXT3/DXT5) ----------- */
static void encode_dxt1_color(const uint8_t *src, uint8_t *dst)
{
    int rmin = 255, gmin = 255, bmin = 255, rmax = 0, gmax = 0, bmax = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t *p = src + i * 4;
        rmin = (int)MIN(rmin, p[0]); gmin = (int)MIN(gmin, p[1]); bmin = (int)MIN(bmin, p[2]);
        rmax = (int)MAX(rmax, p[0]); gmax = (int)MAX(gmax, p[1]); bmax = (int)MAX(bmax, p[2]);
    }
    uint16_t c0 = pack565((uint8_t)rmax, (uint8_t)gmax, (uint8_t)bmax);
    uint16_t c1 = pack565((uint8_t)rmin, (uint8_t)gmin, (uint8_t)bmin);

    /* Build the 4 reference 8-bit (r8,g8,b8) palette entries the way a
     * decoder will: entry k from the unpacked 5:6:5 endpoints. */
    uint8_t e0r, e0g, e0b, e1r, e1g, e1b;
    unpack565(c0, &e0r, &e0g, &e0b);
    unpack565(c1, &e1r, &e1g, &e1b);

    /* Palette entries in 8-bit RGB:
     *  4-colour (c0>c1): 0=c0 1=c1 2=(2*c0+c1)/3 3=(c0+2*c1)/3
     *  3-colour (c0<=c1): 0=c0 1=c1 2=(c0+c1)/2  3=transparent
     * The interpolation happens in 8-bit on the unpacked endpoints. */
    const uint32_t a0r=e0r,a0g=e0g,a0b=e0b, a1r=e1r,a1g=e1g,a1b=e1b;
    uint8_t palr[4], palg[4], palb[4];
    palr[0]=a0r; palg[0]=a0g; palb[0]=a0b;
    palr[1]=a1r; palg[1]=a1g; palb[1]=a1b;
    int c0gtc1 = (c0 > c1);
    palr[2] = (uint8_t)((2*a0r + a1r) / 3u);
    palg[2] = (uint8_t)((2*a0g + a1g) / 3u);
    palb[2] = (uint8_t)((2*a0b + a1b) / 3u);
    if (c0gtc1) {
        palr[3] = (uint8_t)((a0r + 2*a1r) / 3u);
        palg[3] = (uint8_t)((a0g + 2*a1g) / 3u);
        palb[3] = (uint8_t)((a0b + 2*a1b) / 3u);
    } else {
        palr[3] = 0; palg[3] = 0; palb[3] = 0;
    }

    put16le(dst + 0, c0);
    put16le(dst + 2, c1);

    /* 16 two-bit colour indices, packed little-endian into dst[4..7]
     * (index i in bits 2i..2i+1).  The whole "colour" block is 8 bytes. */
    uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t *p = src + i * 4;
        int best = 0; uint32_t bestd = UINT32_MAX;
        for (int k = 0; k < 4; ++k) {
            int dr = (int)p[0]-palr[k], dg=(int)p[1]-palg[k], db=(int)p[2]-palb[k];
            uint32_t d = (uint32_t)(dr*dr + dg*dg + db*db);
            if (d < bestd) { bestd = d; best = k; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    for (int b = 0; b < 4; ++b) dst[4 + b] = (uint8_t)((idx >> (8 * b)) & 0xffu);
}

int dxt1_encode_block(const uint8_t *src, uint8_t *dst)
{
    if (!src || !dst) return 0;
    encode_dxt1_color(src, dst);
    return 1;
}

/* ---------- DXT3 block ------------------------------------------------ */
int dxt3_encode_block(const uint8_t *src, uint8_t *dst)
{
    if (!src || !dst) return 0;
    /* 4-bit explicit alpha: 16 nibbles, texel-major, MSB-first within each
     * byte.  byte i/2, nibble (i&1?0:4).  dst[0..7]=alpha, dst[8..15]=colour. */
    uint8_t alpha[8] = {0,0,0,0,0,0,0,0};
    for (int i = 0; i < 16; ++i) {
        uint8_t a = (uint8_t)(src[i * 4 + 3] >> 4);   /* 8-bit A -> 4-bit A */
        alpha[i >> 1] |= (uint8_t)(a << ((i & 1) ? 0 : 4));
    }
    memcpy(dst, alpha, 8);
    encode_dxt1_color(src, dst + 8);
    return 1;
}

/* ---------- DXT5 block ------------------------------------------------ */
int dxt5_encode_block(const uint8_t *src, uint8_t *dst)
{
    if (!src || !dst) return 0;
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; ++i) {
        int a = src[i * 4 + 3];
        amin = MIN(amin, a); amax = MAX(amax, a);
    }
    uint8_t a0 = (uint8_t)amax, a1 = (uint8_t)amin;
    dst[0] = a0; dst[1] = a1;

    uint8_t palette[6];
    if ((int)a0 - (int)a1 >= 2) {
        palette[2] = (uint8_t)((6*a0 + 1*a1) / 7u);
        palette[3] = (uint8_t)((5*a0 + 2*a1) / 7u);
        palette[4] = (uint8_t)((4*a0 + 3*a1) / 7u);
        palette[5] = (uint8_t)((3*a0 + 4*a1) / 7u);
    } else { /* a0 - a1 < 2: 6-entry with two zeros */
        palette[2] = (uint8_t)((3*a0 + 1*a1) / 4u);
        palette[3] = (uint8_t)((2*a0 + 2*a1) / 4u);
        palette[4] = (uint8_t)((1*a0 + 3*a1) / 4u);
        palette[5] = (uint8_t)0;
    }
    palette[0] = a0; palette[1] = a1;

    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int a = src[i * 4 + 3];
        int best = 0; int bestd = 0x7fffffff;
        for (int k = 0; k < 6; ++k) {
            int d = a - palette[k]; if (d < 0) d = -d;
            if (d < bestd) { bestd = d; best = k; }
        }
        bits |= (uint64_t)best << (i * 3);
    }
    /* 48 bits, little-endian across dst[2..7] (first 6 bytes of the
     * alpha-index region). */
    for (int b = 0; b < 6; ++b) dst[2 + b] = (uint8_t)((bits >> (8 * b)) & 0xffu);

    encode_dxt1_color(src, dst + 8);
    return 1;
}

/* ---------- whole-image wrappers ------------------------------------- */
static size_t encode_image(const uint8_t *rgba, uint32_t width, uint32_t height,
                           uint8_t *dst, int (*blk)(const uint8_t*, uint8_t*),
                           uint32_t bpb)
{
    if (!rgba || !dst || width == 0u || height == 0u) return 0;
    uint32_t bw = (width + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    for (uint32_t ty = 0; ty < bh; ++ty) {
        for (uint32_t tx = 0; tx < bw; ++tx) {
            /* Gather this 4x4 block into a padded 16-texel scratch (rows
             * beyond the image edge reuse the last valid texel). */
            uint8_t tmp[16 * 4];
            for (int y = 0; y < 4; ++y) {
                uint32_t sy = ty * 4 + (uint32_t)y;
                if (sy >= height) sy = height - 1u;
                for (int x = 0; x < 4; ++x) {
                    uint32_t sx = tx * 4 + (uint32_t)x;
                    if (sx >= width) sx = width - 1u;
                    const uint8_t *p = rgba + ((size_t)sy * width + sx) * 4;
                    memcpy(tmp + ((y * 4 + x) * 4), p, 4);
                }
            }
            uint8_t *out = dst + ((size_t)ty * bw + tx) * bpb;
            blk(tmp, out);
        }
    }
    return (size_t)(bw * bh) * bpb;
}

size_t dxt1_encode_image(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *dst)
{ return encode_image(rgba, width, height, dst, dxt1_encode_block, DXT1_BYTES_PER_BLOCK); }

size_t dxt3_encode_image(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *dst)
{ return encode_image(rgba, width, height, dst, dxt3_encode_block, DXT3_BYTES_PER_BLOCK); }

size_t dxt5_encode_image(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *dst)
{ return encode_image(rgba, width, height, dst, dxt5_encode_block, DXT5_BYTES_PER_BLOCK); }
