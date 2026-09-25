#ifndef HELLO_PPU_FONT_RENDER_PSL1GHT_FONT_H
#define HELLO_PPU_FONT_RENDER_PSL1GHT_FONT_H

#include <stdint.h>

/* Checks the PSL1GHT pointer-returning wrappers against the objects the
 * canonical API produced. Returns 1 when every check passes. */
int psl1ght_check_font(void *font_obj, const void *expect_lib,
                       const void *expect_renderer);

/* Renders text through PSL1GHT names into an 8-bit surface; returns the
 * pen position after the last glyph. */
float psl1ght_render_text(void *font_obj, uint8_t *surface_buf, int width,
                          int height, float x, float y, const char *text);

/* Copies a rendered glyph image into the surface position the font
 * library reported (main.c). */
void copy_glyph(const uint8_t *image, uint32_t image_stride, uint32_t w,
                uint32_t h, uint8_t *dst, uint32_t dst_stride);

#endif
