#ifndef EYE_YUV_H
#define EYE_YUV_H
#include <stddef.h>
#include <stdint.h>

/* Packed Y0 U Y1 V to byte RGBA, BT.601 limited range. Integer arithmetic
 * and byte accesses only: either buffer may start at any byte address.
 * The buffers must not overlap. Invalid dimensions, pitches or extents return
 * -1 before writing anything. Success returns 0; padding remains unchanged. */
int eye_yuv422_rgba(const uint8_t *src, size_t src_size, size_t src_pitch,
                   uint8_t *dst, size_t dst_size, size_t dst_pitch,
                   unsigned width, unsigned height);

/* Byte RGBA to big-endian X8R8G8B8 scanout bytes, independent of host endian. */
int eye_rgba_xrgb(const uint8_t *src, size_t src_size, size_t src_pitch,
                  uint8_t *dst, size_t dst_size, size_t dst_pitch,
                  unsigned width, unsigned height);
#endif
