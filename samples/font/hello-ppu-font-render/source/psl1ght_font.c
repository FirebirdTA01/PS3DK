/*
 * The PSL1GHT half of hello-ppu-font-render.
 *
 * PSL1GHT programs call libfont through font* names and PSL1GHT's own
 * types. Those names now resolve to the SDK's stub archive, and five of
 * them are C wrappers that receive a 32-bit address from the PRX. This file
 * drives a font that main.c opened through the canonical API, using only
 * PSL1GHT names, so the sample shows both surfaces on one screen.
 *
 * PSL1GHT's font types and the SDK's describe the same memory but cannot
 * share a translation unit, so the objects cross over as void pointers.
 */

#include <stdint.h>
#include <stdio.h>

#include <font/font.h>

#include "psl1ght_font.h"

int psl1ght_check_font(void *font_obj, const void *expect_lib,
                       const void *expect_renderer)
{
    font *f = (font *)font_obj;
    const fontLibrary *lib = NULL;
    fontRenderer *renderer = NULL;
    fontGlyph *glyph = NULL;
    u32 type = 0;
    int ok = 1;
    s32 rc;

    /* Each of these returns a pointer the PRX wrote as a 32-bit address. */
    rc = fontGetLibrary(f, &lib, &type);
    printf("  PSL1GHT fontGetLibrary -> 0x%08x lib=%p type=%u (canonical lib=%p)\n",
           (unsigned)rc, (const void *)lib, (unsigned)type, expect_lib);
    if (rc != 0 || (const void *)lib != expect_lib)
        ok = 0;

    rc = fontGetBindingRenderer(f, &renderer);
    printf("  PSL1GHT fontGetBindingRenderer -> 0x%08x renderer=%p (canonical %p)\n",
           (unsigned)rc, (void *)renderer, expect_renderer);
    if (rc != 0 || (const void *)renderer != expect_renderer)
        ok = 0;

    rc = fontGenerateCharGlyph(f, 'A', &glyph);
    printf("  PSL1GHT fontGenerateCharGlyph('A') -> 0x%08x glyph=%p\n",
           (unsigned)rc, (void *)glyph);
    if (rc != 0 || glyph == NULL) {
        ok = 0;
    } else {
        printf("    glyph metrics: width=%.1f height=%.1f advance=%.1f\n",
               glyph->metrics.width, glyph->metrics.height,
               glyph->metrics.horizontal.advance);
        fontDeleteGlyph(f, glyph);
    }

    /* The NULL checks run in the wrapper, not the PRX. */
    rc = fontGetLibrary(NULL, &lib, &type);
    printf("  PSL1GHT fontGetLibrary(NULL) -> 0x%08x (expect 0x80540002)\n",
           (unsigned)rc);
    if ((uint32_t)rc != 0x80540002u)
        ok = 0;

    return ok;
}

int psl1ght_render_text(void *font_obj, uint8_t *surface_buf, int width,
                        int height, float x, float y, const char *text,
                        float *pen_end)
{
    font *f = (font *)font_obj;
    fontRenderSurface surf;
    fontGlyphMetrics metrics;
    fontImageTransInfo trans;
    int failed = 0;

    fontRenderSurfaceInit(&surf, surface_buf, width, 1, width, height);
    fontRenderSurfaceSetScissor(&surf, 0, 0, width, height);
    for (const char *p = text; *p; p++) {
        s32 rc = fontRenderCharGlyphImage(f, (u8)*p, &surf, x, y, &metrics,
                                          &trans);
        if (rc != 0) {
            printf("  PSL1GHT fontRenderCharGlyphImage('%c' U+%04X) -> 0x%08x\n",
                   *p, (unsigned)(u8)*p, (unsigned)rc);
            failed++;
            continue;
        }
        copy_glyph(trans.image, trans.imageWidthByte, trans.imageWidth,
                   trans.imageHeight, (uint8_t *)trans.surface,
                   trans.surfWidthByte);
        x += metrics.horizontal.advance;
    }
    *pen_end = x;
    return failed;
}
