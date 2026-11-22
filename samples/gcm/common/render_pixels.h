#ifndef SAMPLE_RENDER_PIXELS_H
#define SAMPLE_RENDER_PIXELS_H
#include <stdint.h>
/* XRGB render targets do not promise the high byte. Compare RGB only. */
static inline int sample_rgb_matches(uint32_t actual, uint32_t expected,
                                     unsigned tolerance)
{
    for (unsigned shift=0; shift<24; shift+=8) {
        int delta=(int)((actual>>shift)&255)-(int)((expected>>shift)&255);
        if (delta < -(int)tolerance || delta > (int)tolerance) return 0;
    }
    return 1;
}

static inline uint32_t sample_blend_rgb(uint32_t color,uint32_t background)
{
    unsigned a=color>>24;
    uint32_t out=0;
    for (unsigned s=0;s<24;s+=8)
        out|=(((((color>>s)&255)*a+((background>>s)&255)*(255-a)+127)/255)<<s);
    return out;
}

/* Returns -1 for invalid geometry, otherwise the count of mismatched texels.
 * Pitch is in pixels. Source expectations are row-major; the sample reads
 * the center of each nearest-filtered, integer-scaled source texel. */
static inline int sample_check_panel(const volatile uint32_t *pixels,
    unsigned image_width,unsigned image_height,unsigned pitch,
    unsigned left,unsigned top,unsigned scale,unsigned width,unsigned height,
    const uint32_t *expected,unsigned tolerance)
{
    if (!pixels || !expected || !width || !height || !scale ||
        pitch<image_width || left>=image_width || top>=image_height ||
        width>(image_width-left)/scale || height>(image_height-top)/scale ||
        width>4096 || height>4096 || image_height>4096 || pitch>16384)
        return -1;
    int errors=0;
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        unsigned px=left+x*scale+scale/2, py=top+y*scale+scale/2;
        if (!sample_rgb_matches(pixels[py*pitch+px],expected[y*width+x],tolerance))
            ++errors;
    }
    return errors;
}
#endif
