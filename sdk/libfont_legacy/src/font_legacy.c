/* sdk/libfont_legacy/src/font_legacy.c
 *
 * The five PSL1GHT libfont names that are not firmware exports.
 *
 * Every other PSL1GHT font* name is a nidgen alias of the reference export
 * it called (libfont_stub.yaml). These were C in PSL1GHT:
 *
 *   fontGetStubRevisionFlags reports the stub revision PSL1GHT programs
 *   were written against (0x14), which is older than the one
 *   cellFontGetStubRevisionFlags reports. Their configuration structures
 *   follow that revision, so the legacy name keeps it.
 *
 *   The other four return a pointer through an out-parameter. The PRX
 *   stores a 32-bit effective address, so each wrapper receives it into a
 *   32-bit slot and widens it into the caller's pointer. That is correct
 *   in both ABIs; under LP64 the caller's slot is 8 bytes (t_dba7a45b).
 *   A NULL argument returns CELL_FONT_ERROR_INVALID_PARAMETER without
 *   calling the PRX, and a failed call stores NULL.
 */

#include <stddef.h>
#include <stdint.h>

#include <cell/font/libfont.h>
#include <cell/font/error.h>

/* The firmware exports under their PSL1GHT alias names, declared with the
 * 32-bit out-parameter the PRX actually writes. */
extern int fontGetLibraryEx(CellFont *font, uint32_t *lib, uint32_t *type);
extern int fontGetBindingRendererEx(CellFont *font, uint32_t *renderer);
extern int fontGenerateCharGlyphEx(CellFont *font, uint32_t code,
                                   uint32_t *glyph);
extern int fontGenerateCharGlyphVerticalEx(CellFont *font, uint32_t code,
                                           uint32_t *glyph);

void fontGetStubRevisionFlags(uint64_t *revisionFlags);
int fontGetLibrary(CellFont *font, const CellFontLibrary **lib,
                   uint32_t *type);
int fontGetBindingRenderer(CellFont *font, CellFontRenderer **renderer);
int fontGenerateCharGlyph(CellFont *font, uint32_t code,
                          CellFontGlyph **glyph);
int fontGenerateCharGlyphVertical(CellFont *font, uint32_t code,
                                  CellFontGlyph **glyph);

#define EA_TO_PTR(ea) ((void *)(uintptr_t)(ea))

void fontGetStubRevisionFlags(uint64_t *revisionFlags)
{
    if (revisionFlags == NULL)
        return;
    *revisionFlags = 0x14;
}

int fontGetLibrary(CellFont *font, const CellFontLibrary **lib,
                   uint32_t *type)
{
    uint32_t ea = 0;
    int ret;

    if (font == NULL || lib == NULL || type == NULL)
        return CELL_FONT_ERROR_INVALID_PARAMETER;
    ret = fontGetLibraryEx(font, &ea, type);
    *lib = ret == 0 ? EA_TO_PTR(ea) : NULL;
    return ret;
}

int fontGetBindingRenderer(CellFont *font, CellFontRenderer **renderer)
{
    uint32_t ea = 0;
    int ret;

    if (font == NULL || renderer == NULL)
        return CELL_FONT_ERROR_INVALID_PARAMETER;
    ret = fontGetBindingRendererEx(font, &ea);
    *renderer = ret == 0 ? EA_TO_PTR(ea) : NULL;
    return ret;
}

int fontGenerateCharGlyph(CellFont *font, uint32_t code,
                          CellFontGlyph **glyph)
{
    uint32_t ea = 0;
    int ret;

    if (font == NULL || glyph == NULL)
        return CELL_FONT_ERROR_INVALID_PARAMETER;
    ret = fontGenerateCharGlyphEx(font, code, &ea);
    *glyph = ret == 0 ? EA_TO_PTR(ea) : NULL;
    return ret;
}

int fontGenerateCharGlyphVertical(CellFont *font, uint32_t code,
                                  CellFontGlyph **glyph)
{
    uint32_t ea = 0;
    int ret;

    if (font == NULL || glyph == NULL)
        return CELL_FONT_ERROR_INVALID_PARAMETER;
    ret = fontGenerateCharGlyphVerticalEx(font, code, &ea);
    *glyph = ret == 0 ? EA_TO_PTR(ea) : NULL;
    return ret;
}
