/* CPU-side swizzle conversion; no GCM context or GPU submission needed. */
#include <cell/gcm/gcm_swizzle_func.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH = 8, HEIGHT = 4, TEXEL_BYTES = 4, DEST_BYTES = 16384 };

/* Explicit offsets for this small 8x4 rectangle, in texels. This table is
 * intentionally separate from the converter's bit-interleaving code. */
static const uint8_t positions[HEIGHT][WIDTH] = {
    { 0, 1, 4, 5, 16, 17, 20, 21 },
    { 2, 3, 6, 7, 18, 19, 22, 23 },
    { 8, 9,12,13, 24, 25, 28, 29 },
    {10,11,14,15, 26, 27, 30, 31 }
};

static unsigned copies;
static void copy_rgba(void *dst, const void *src)
{
    memcpy(dst, src, TEXEL_BYTES);
    ++copies;
}

int main(void)
{
    uint8_t source[HEIGHT][WIDTH][TEXEL_BYTES];
    static uint8_t expected[DEST_BYTES];
    static struct { uint8_t before[16], bytes[DEST_BYTES], after[16]; } dest;
    int failures = 0;
    memset(expected, 0xcc, sizeof expected);
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH; ++x) {
            source[y][x][0] = (uint8_t)(x * 31u);
            source[y][x][1] = (uint8_t)(y * 63u);
            source[y][x][2] = 0x5a;
            source[y][x][3] = 0xff;
            memcpy(expected + TEXEL_BYTES * positions[y][x], source[y][x], TEXEL_BYTES);
        }

    for (unsigned callback = 0; callback < 2; ++callback) {
        memset(&dest, 0xcc, sizeof dest);
        copies = 0;
        /* Current SDK: destination extent must cover a 32x32 tile for
         * 4-byte texels. Reserve a whole 16 KiB tile, including padding.
         * Source is an 8x4 subrectangle, not an 8x4 destination texture.
         * dstLog2cdepth=2 follows the SDK's byte-width interpretation. */
        cellGcmConvertSwizzleFormat(dest.bytes, source,
            0, 0, 0, 32, 32, 1,
            0, 0, 0, WIDTH, HEIGHT, 1,
            WIDTH, HEIGHT, 1, 2, TEXEL_BYTES, 0, 2,
            callback ? copy_rgba : NULL);
        int ok = memcmp(dest.bytes, expected, sizeof expected) == 0;
        for (unsigned i = 0; i < sizeof dest.before; ++i)
            ok = ok && dest.before[i] == 0xcc && dest.after[i] == 0xcc;
        ok = ok && copies == (callback ? WIDTH * HEIGHT : 0);
        printf("swizzle %s: %s (32 texels, padding and guards checked)\n",
               callback ? "copy callback" : "default copy", ok ? "PASS" : "FAIL");
        failures += !ok;
    }
    printf("hello-ppu-gcm-swizzle: %s; CPU conversion only\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
