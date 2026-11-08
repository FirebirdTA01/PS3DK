/* Host format checks: literal alpha bytes, separately decoded solids,
 * eight-entry alpha ramps, bounded gradients, and image sizes.
 * The color-gradient tolerance reflects the simple bounding-box encoder;
 * it is not a quality guarantee for arbitrary images. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "dxt.h"

/* ============ INDEPENDENT DECODER (written here, not from dxt.c) ===== */

/* expand 5:6:5 (16-bit) to 8-bit channels */
static void dec565(uint16_t v, int *r, int *g, int *b)
{
    int rr = (v >> 11) & 0x1f;
    int gg = (v >> 5)  & 0x3f;
    int bb = (v       ) & 0x1f;
    *r = rr * 255 / 31;
    *g = gg * 255 / 63;
    *b = bb * 255 / 31;
}

/* Decode the 8-byte DXT1 colour sub-block: 4-colour or 3-colour palette,
 * 16 two-bit indices in c[4..7] (little-endian). */
static void dec_dxt1_color(const uint8_t *c, uint8_t rgb[16][3], int force_four)
{
    uint16_t c0 = (uint16_t)(c[0] | (c[1] << 8));
    uint16_t c1 = (uint16_t)(c[2] | (c[3] << 8));
    int e0r, e0g, e0b, e1r, e1g, e1b;
    dec565(c0, &e0r, &e0g, &e0b);
    dec565(c1, &e1r, &e1g, &e1b);
    int pr[4], pg[4], pb[4];
    pr[0]=e0r; pg[0]=e0g; pb[0]=e0b;
    pr[1]=e1r; pg[1]=e1g; pb[1]=e1b;
    pr[2]=(2*e0r+e1r)/3; pg[2]=(2*e0g+e1g)/3; pb[2]=(2*e0b+e1b)/3;
    if (force_four || c0 > c1) { pr[3]=(e0r+2*e1r)/3; pg[3]=(e0g+2*e1g)/3; pb[3]=(e0b+2*e1b)/3; }
    else         { pr[2]=(e0r+e1r)/2; pg[2]=(e0g+e1g)/2; pb[2]=(e0b+e1b)/2; pr[3]=0; pg[3]=0; pb[3]=0; }   /* 3-colour: index 3 transparent */

    uint32_t idx = 0;
    for (int b = 0; b < 4; ++b) idx |= (uint32_t)c[4 + b] << (8 * b);
    for (int i = 0; i < 16; ++i) {
        int idxx = (int)((idx >> (i * 2)) & 0x3u);
        rgb[i][0] = (uint8_t)pr[idxx];
        rgb[i][1] = (uint8_t)pg[idxx];
        rgb[i][2] = (uint8_t)pb[idxx];
    }
}

static void dec_dxt1(const uint8_t *blk, uint8_t rgb[16][3])
{
    dec_dxt1_color(blk, rgb, 0);
}

static void dec_dxt3(const uint8_t *blk, uint8_t rgb[16][3], uint8_t alpha[16])
{
    for (int i = 0; i < 16; ++i) {
        int byte = i >> 1, shift = (i & 1) * 4;
        alpha[i] = (uint8_t)(((blk[byte] >> shift) & 0x0fu) * 17u);
    }
    dec_dxt1_color(blk + 8, rgb, 1);
}

static void dec_dxt5(const uint8_t *blk, uint8_t rgb[16][3], uint8_t alpha[16])
{
    int a0 = blk[0], a1 = blk[1];
    int pal[8];
    pal[0]=a0; pal[1]=a1;
    if (a0 > a1) {
        pal[2]=(6*a0+1*a1)/7; pal[3]=(5*a0+2*a1)/7;
        pal[4]=(4*a0+3*a1)/7; pal[5]=(3*a0+4*a1)/7;
        pal[6]=(2*a0+5*a1)/7; pal[7]=(a0+6*a1)/7;
    } else {
        pal[2]=(4*a0+a1)/5; pal[3]=(3*a0+2*a1)/5;
        pal[4]=(2*a0+3*a1)/5; pal[5]=(a0+4*a1)/5;
        pal[6]=0; pal[7]=255;
    }
    uint64_t bits = 0;
    for (int b = 0; b < 6; ++b) bits |= (uint64_t)blk[2 + b] << (8 * b);
    for (int i = 0; i < 16; ++i)
        alpha[i] = (uint8_t)pal[(int)((bits >> (i * 3)) & 0x7u)];
    dec_dxt1_color(blk + 8, rgb, 1);
}

/* ============ helpers ================================================ */
static int checks = 0, failures = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int maxerr_rgb(uint8_t rgb[16][3], int er, int eg, int eb)
{
    int m = 0;
    for (int i = 0; i < 16; ++i) {
        int d = rgb[i][0]-er; if (d<0) d=-d; if (d>m) m=d;
        d = rgb[i][1]-eg;     if (d<0) d=-d; if (d>m) m=d;
        d = rgb[i][2]-eb;     if (d<0) d=-d; if (d>m) m=d;
    }
    return m;
}
static int maxerr_alpha(uint8_t a[16], int e)
{
    int m = 0;
    for (int i = 0; i < 16; ++i) { int d = a[i]-e; if (d<0) d=-d; if (d>m) m=d; }
    return m;
}
static void solid_block(uint8_t *buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{ for (int i = 0; i < 16; ++i) { buf[i*4+0]=r; buf[i*4+1]=g; buf[i*4+2]=b; buf[i*4+3]=a; } }

int main(void)
{
    uint8_t src[64] = {0}, blk[16] = {0}, rgb[16][3] = {{0}}, a1[16] = {0};

    /* Decoder controls are literal blocks, not encoder output. With C0=0,
     * C1=white and selector 3, DXT1 is black; DXT3/5 always use four colors. */
    static const uint8_t color_control[8] = {0,0,255,255,255,255,255,255};
    memcpy(blk, color_control, 8);
    dec_dxt1(blk, rgb);
    CHECK(maxerr_rgb(rgb, 0, 0, 0) == 0, "DXT1 decoder three-color control");
    memset(blk, 255, 8);
    memcpy(blk+8, color_control, 8);
    dec_dxt3(blk, rgb, a1);
    CHECK(maxerr_rgb(rgb, 170, 170, 170) == 0, "DXT3 decoder four-color control");
    CHECK(maxerr_alpha(a1, 255) == 0, "DXT3 decoder alpha normalization");
    dec_dxt5(blk, rgb, a1);
    CHECK(maxerr_rgb(rgb, 170, 170, 170) == 0, "DXT5 decoder four-color control");
    CHECK(maxerr_alpha(a1, 255) == 0, "DXT5 decoder implicit alpha 255");

    /* Literal byte fixtures, independent of the encoder's palette builder.
     * S3TC explicit alpha: texel zero occupies the LOW nibble. Its normalized
     * 4-bit value expands to 8 bits by replication, not by a left shift.
     * https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt */
    static const uint8_t ramp_bytes[8] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe
    };
    for (int i = 0; i < 16; ++i) src[4*i+3] = (uint8_t)(17*i);
    CHECK(dxt3_encode_block(src, blk) == 1, "DXT3 ramp encode failed");
    CHECK(memcmp(blk, ramp_bytes, 8) == 0, "DXT3 alpha nibble order");
    dec_dxt3(blk, rgb, a1);
    for (int i = 0; i < 16; ++i)
        CHECK(a1[i] == 17*i, "DXT3 alpha ramp texel %d: %u", i, a1[i]);

    /* All eight entries under this host decoder's truncating integer
     * convention. This is not a measurement of GPU interpolation rounding.
     * Pin the packed selectors independently of the matching decoder. */
    static const uint8_t alpha_ramp[8] = {255, 0, 218, 182, 145, 109, 72, 36};
    static const uint8_t alpha_selectors[8] = {
        255, 0, 0x88, 0xc6, 0xfa, 0x88, 0xc6, 0xfa
    }; /* endpoints 255/0, selectors 0..7 twice, least-significant first */
    for (int i = 0; i < 16; ++i) src[4*i+3] = alpha_ramp[i % 8];
    CHECK(dxt5_encode_block(src, blk) == 1, "DXT5 ramp encode failed");
    CHECK(memcmp(blk, alpha_selectors, 8) == 0, "DXT5 eight-selector byte fixture");
    dec_dxt5(blk, rgb, a1);
    for (int i = 0; i < 16; ++i)
        CHECK(a1[i] == alpha_ramp[i % 8], "DXT5 alpha ramp texel %d: %u", i, a1[i]);

    /* ---- 1. SOLID ROUND-TRIP: 8 solids, all EXACT under DXT1/3/5 ----
     * Colour channels in {0,255}: the only 5:6:5-exact 8-bit values
     * other than 0 (255>>3 = 31, 31*255/31 = 255; 128>>3 = 16, 16*255/31
     * = 131 which is off by 3 - not exact).
     * Alpha in {0,255}: 4-bit-exact (15*17 = 255) under
     * DXT3; exact under DXT5 because a0==a1 in a solid. */
    struct solid { const char *n; uint8_t r, g, b, a; } solids[] = {
        {"black",   0,  0,  0,  0},
        {"black+a", 0,  0,  0,255},
        {"white", 255,255,255,  0},
        {"white+a",255,255,255,255},
        {"red",   255,  0,  0,  0},
        {"red+a", 255,  0,  0,255},
        {"green",   0,255,  0,  0},
        {"blue",    0,  0,255,255},
    };

    for (int s = 0; s < (int)(sizeof solids / sizeof solids[0]); ++s) {
        solid_block(src, solids[s].r, solids[s].g, solids[s].b, solids[s].a);

        int ec = 0, ea = 0;
        CHECK(dxt1_encode_block(src, blk) == 1, "%s: dxt1 encode", solids[s].n);
        dec_dxt1(blk, rgb);
        ec = maxerr_rgb(rgb, solids[s].r, solids[s].g, solids[s].b);
        CHECK(ec == 0, "%s: DXT1 solid not exact (maxerr=%d)", solids[s].n, ec);

        CHECK(dxt3_encode_block(src, blk) == 1, "%s: dxt3 encode", solids[s].n);
        dec_dxt3(blk, rgb, a1);
        ec = maxerr_rgb(rgb, solids[s].r, solids[s].g, solids[s].b);
        ea = maxerr_alpha(a1, solids[s].a);
        CHECK(ec == 0, "%s: DXT3 solid colour not exact (maxerr=%d)", solids[s].n, ec);
        CHECK(ea == 0, "%s: DXT3 solid alpha not exact (maxerr=%d)", solids[s].n, ea);

        CHECK(dxt5_encode_block(src, blk) == 1, "%s: dxt5 encode", solids[s].n);
        dec_dxt5(blk, rgb, a1);
        ec = maxerr_rgb(rgb, solids[s].r, solids[s].g, solids[s].b);
        ea = maxerr_alpha(a1, solids[s].a);
        CHECK(ec == 0, "%s: DXT5 solid colour not exact (maxerr=%d)", solids[s].n, ec);
        CHECK(ea == 0, "%s: DXT5 solid alpha not exact (maxerr=%d)", solids[s].n, ea);
    }

    /* Bounding-box endpoints poorly fit a two-dimensional color gradient.
     * Keep a bounded regression without claiming a small quantization error. */
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        int idx = y * 4 + x;
        src[idx*4+0] = (uint8_t)(255 * x         / 3);   /* red   = x    */
        src[idx*4+1] = (uint8_t)(255 * y         / 3);   /* green = y    */
        src[idx*4+2] = (uint8_t)(255 * (x + y)   / 6);   /* blue  = diag */
        src[idx*4+3] = 255;                              /* alpha   = 255 */
    }

    dxt1_encode_block(src, blk);
    dec_dxt1(blk, rgb);
    {
        int e = 0;
        for (int i = 0; i < 16; ++i)
            for (int c = 0; c < 3; ++c) {
                int d = (int)rgb[i][c] - (int)src[i*4+c];
                if (d < 0) d = -d;
                if (d > e) e = d;
            }
        /* bounding-box endpoints: red=max, green=max, blue=max all hit 255
           on this diagonal, so c0/c1 are (255,255,255) and (0,0,0); the palette
           is only two greys plus midpoints, so a per-channel error of up to ~130
           is the expected DXT1 limit for this input.  Assert a loose bound that
           still catches "encoder produced garbage" (error would be ~255). */
        CHECK(e <= 180, "DXT1 gradient maxerr %d exceeds expected bound 180", e);
        printf("  DXT1 gradient maxerr=%d (bound 180, bbox worst-case on this input)\n", e);
    }

    dxt5_encode_block(src, blk);
    dec_dxt5(blk, rgb, a1);
    {
        int e = 0, ea = 0;
        for (int i = 0; i < 16; ++i)
            for (int c = 0; c < 3; ++c) {
                int d = (int)rgb[i][c] - (int)src[i*4+c];
                if (d < 0) d = -d;
                if (d > e) e = d;
            }
        for (int i = 0; i < 16; ++i) {
            int d = a1[i]-255; if (d < 0) d = -d; if (d > ea) ea = d;
        }
        CHECK(e <= 180, "DXT5 gradient colour maxerr %d exceeds expected bound 180", e);
        CHECK(ea <= 16, "DXT5 gradient alpha maxerr %d exceeds bound 16", ea);
        printf("  DXT5 gradient maxerr=%d alpha_err=%d (bounds 180/16)\n", e, ea);
    }

    dxt3_encode_block(src, blk);
    dec_dxt3(blk, rgb, a1);
    {
        int e = 0, ea = 0;
        for (int i = 0; i < 16; ++i)
            for (int c = 0; c < 3; ++c) {
                int d = (int)rgb[i][c] - (int)src[i*4+c];
                if (d < 0) d = -d;
                if (d > e) e = d;
            }
        for (int i = 0; i < 16; ++i) {
            int d = a1[i]-255; if (d < 0) d = -d; if (d > ea) ea = d;
        }
        CHECK(e <= 180, "DXT3 gradient colour maxerr %d exceeds expected bound 180", e);
        CHECK(ea <= 15, "DXT3 gradient alpha maxerr %d exceeds 4-bit bound 15", ea);
        printf("  DXT3 gradient maxerr=%d alpha_err=%d (bounds 180/15)\n", e, ea);
    }

    /* ---- 3. OUTPUT SIZES --------------------------------------------- */
    uint8_t *out = (uint8_t *)malloc(256);
    CHECK(dxt1_encode_image(src, 4, 4, out) == 8,  "DXT1 4x4 image size != 8");
    CHECK(dxt3_encode_image(src, 4, 4, out) == 16, "DXT3 4x4 image size != 16");
    CHECK(dxt5_encode_image(src, 4, 4, out) == 16, "DXT5 4x4 image size != 16");
    uint8_t big[8 * 8 * 4];
    for (int i = 0; i < 64; ++i) { big[i*4]=128; big[i*4+1]=64; big[i*4+2]=32; big[i*4+3]=0; }
    CHECK(dxt1_encode_image(big, 8, 8, out) == 8 * 2 * 2, "DXT1 8x8 image size != 32");
    free(out);

    if (failures == 0)
        printf("dxt-test: PASS (%d checks, lossless solids + bounded gradients + sizes)\n", checks);
    else
        printf("dxt-test: FAIL (%d of %d checks failed)\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
