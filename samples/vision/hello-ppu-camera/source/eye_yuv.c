#include "eye_yuv.h"

static int span(size_t size, size_t pitch, size_t row, unsigned height, size_t *used)
{
    if (!height || !row || pitch < row ||
        (size_t)(height - 1) > (SIZE_MAX - row) / pitch)
        return -1;
    *used = (size_t)(height - 1) * pitch + row;
    return *used <= size ? 0 : -1;
}

static int buffers(const uint8_t *src, size_t src_size, size_t src_pitch,
                   uint8_t *dst, size_t dst_size, size_t dst_pitch,
                   unsigned width, unsigned height, unsigned input_bytes)
{
    size_t in_used, out_used, output_row = (size_t)width * 4;
    if (!src || !dst || !width || output_row / 4 != width ||
        span(src_size, src_pitch, (size_t)width * input_bytes, height, &in_used) ||
        span(dst_size, dst_pitch, output_row, height, &out_used))
        return -1;
    uintptr_t in = (uintptr_t)src, out = (uintptr_t)dst;
    if (in > UINTPTR_MAX - in_used || out > UINTPTR_MAX - out_used ||
        (in < out + out_used && out < in + in_used))
        return -1;
    return 0;
}

static uint8_t channel(int scaled)
{
    /* Clamp before division: never depend on right-shift of a negative int. */
    if (scaled <= 0) return 0;
    if (scaled >= 255 * 256) return 255;
    return (uint8_t)(scaled / 256);
}

int eye_yuv422_rgba(const uint8_t *src, size_t src_size, size_t src_pitch,
                   uint8_t *dst, size_t dst_size, size_t dst_pitch,
                   unsigned width, unsigned height)
{
    if ((width & 1) || buffers(src, src_size, src_pitch, dst, dst_size, dst_pitch,
                               width, height, 2))
        return -1;
    for (unsigned y = 0; y < height; ++y) {
        const uint8_t *s = src + (size_t)y * src_pitch;
        uint8_t *d = dst + (size_t)y * dst_pitch;
        for (unsigned x = 0; x < width; x += 2) {
            int u = (int)s[1] - 128, v = (int)s[3] - 128;
            for (unsigned p = 0; p < 2; ++p) {
                int luma = 298 * ((int)s[p * 2] - 16);
                d[0] = channel(luma + 409 * v + 128);
                d[1] = channel(luma - 100 * u - 208 * v + 128);
                d[2] = channel(luma + 516 * u + 128);
                d[3] = 255;
                d += 4;
            }
            s += 4;
        }
    }
    return 0;
}

int eye_rgba_xrgb(const uint8_t *src, size_t src_size, size_t src_pitch,
                  uint8_t *dst, size_t dst_size, size_t dst_pitch,
                  unsigned width, unsigned height)
{
    if (buffers(src, src_size, src_pitch, dst, dst_size, dst_pitch, width, height, 4))
        return -1;
    for (unsigned y = 0; y < height; ++y) {
        const uint8_t *s = src + (size_t)y * src_pitch;
        uint8_t *d = dst + (size_t)y * dst_pitch;
        for (unsigned x = 0; x < width; ++x) {
            d[0] = 255; d[1] = s[0]; d[2] = s[1]; d[3] = s[2];
            s += 4; d += 4;
        }
    }
    return 0;
}
