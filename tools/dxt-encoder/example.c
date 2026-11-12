/* Small host asset-pipeline example: four solid RGB blocks with alpha ramps.
 * Byte-oriented RGBA input and little-endian DXT output are host-independent. */
#include "dxt.h"
#include <stdio.h>
#include <string.h>

static int write_bytes(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    int ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "Could not finish %s\n", path);
    return ok;
}

int main(int argc, char **argv)
{
    static const uint8_t colors[4][3] = {
        {255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}
    };
    static const uint8_t endpoints[4][2] = {
        {0x00,0xf8}, {0xe0,0x07}, {0x1f,0x00}, {0xff,0xff}
    };
    static const uint8_t alpha_bytes[8] = {0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe};
    uint8_t rgba[8*8*4], d1[32], d3[64], d5[64];
    if (argc != 1 && argc != 5) {
        fprintf(stderr, "Usage: %s [input.rgba output.dxt1 output.dxt3 output.dxt5]\n", argv[0]);
        return 2;
    }
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            unsigned block = (y/4)*2 + x/4;
            uint8_t *p = rgba + (y*8+x)*4;
            memcpy(p, colors[block], 3);
            p[3] = (uint8_t)(((y%4)*4+x%4)*17);
        }
    if (dxt1_encode_image(rgba,8,8,d1) != sizeof d1 ||
        dxt3_encode_image(rgba,8,8,d3) != sizeof d3 ||
        dxt5_encode_image(rgba,8,8,d5) != sizeof d5) {
        fputs("FAIL output size\n", stderr); return 1;
    }
    for (unsigned b = 0; b < 4; ++b) {
        /* Literal endpoint bytes and zero color indices for solid primaries.
         * This also checks image block order without sharing encoder logic. */
        uint8_t color[8] = {0};
        memcpy(color, endpoints[b], 2);
        memcpy(color+2, endpoints[b], 2);
        if (memcmp(d1+b*8, color, 8) || memcmp(d3+b*16+8, color, 8) ||
            memcmp(d5+b*16+8, color, 8) || memcmp(d3+b*16, alpha_bytes, 8)) {
            fprintf(stderr, "FAIL block %u byte layout\n", b); return 1;
        }
        /* Decode all sixteen alpha selectors using the encoder's truncating
         * integer palette. This host convention does not establish GPU rounding. */
        static const unsigned palette[8] = {255,0,218,182,145,109,72,36};
        const uint8_t *a = d5+b*16;
        uint64_t selectors = 0;
        if (a[0] != 255 || a[1] != 0) return 1;
        for (unsigned i = 0; i < 6; ++i) selectors |= (uint64_t)a[2+i] << (i*8);
        for (unsigned i = 0; i < 16; ++i) {
            int error = (int)palette[(selectors >> (i*3)) & 7] - (int)(i*17);
            if (error < -18 || error > 18) {
                fprintf(stderr, "FAIL block %u alpha texel %u\n", b, i); return 1;
            }
        }
    }
    if (argc == 5 && (!write_bytes(argv[1], rgba, sizeof rgba) ||
        !write_bytes(argv[2], d1, sizeof d1) || !write_bytes(argv[3], d3, sizeof d3) ||
        !write_bytes(argv[4], d5, sizeof d5))) return 1;
    puts("dxt-example: PASS (8x8 RGBA, four blocks; DXT1=32 DXT3=64 DXT5=64 bytes)");
    puts("DXT1 discards source alpha. DXT3 keeps this ramp exactly; host-decoded DXT5 error <=18/255.");
    return 0;
}
