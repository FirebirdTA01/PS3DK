/* sdk/libfontFT_legacy/src/fontft_legacy.c
 *
 * The two PSL1GHT libfontFT names that are not firmware exports.
 *
 * fontInitLibraryFreeTypeWithRevision is a nidgen alias of the reference
 * export (libfontFT_stub.yaml). PSL1GHT wrapped it in C:
 *
 *   fontFTGetStubRevisionFlags reports PSL1GHT's stub revision (0x14),
 *   older than the one cellFontFTGetStubRevisionFlags reports; PSL1GHT
 *   configuration structures follow it.
 *
 *   fontInitLibraryFreeType passes that revision and receives the library
 *   pointer the PRX writes as a 32-bit effective address, widening it into
 *   the caller's pointer (see font_legacy.c in libfont_legacy).
 */

#include <stddef.h>
#include <stdint.h>

#include <cell/font/libfontFT.h>
#include <cell/font/error.h>

extern int fontInitLibraryFreeTypeWithRevision(uint64_t revisionFlags,
                                               CellFontLibraryConfigFT *config,
                                               uint32_t *lib);

void fontFTGetStubRevisionFlags(uint64_t *revisionFlags);
int fontInitLibraryFreeType(CellFontLibraryConfigFT *config,
                            const CellFontLibrary **lib);

void fontFTGetStubRevisionFlags(uint64_t *revisionFlags)
{
    if (revisionFlags == NULL)
        return;
    *revisionFlags = 0x14;
}

int fontInitLibraryFreeType(CellFontLibraryConfigFT *config,
                            const CellFontLibrary **lib)
{
    uint64_t revisionFlags = 0;
    uint32_t ea = 0;
    int ret;

    if (config == NULL || lib == NULL)
        return CELL_FONT_ERROR_INVALID_PARAMETER;
    fontFTGetStubRevisionFlags(&revisionFlags);
    ret = fontInitLibraryFreeTypeWithRevision(revisionFlags, config, &ea);
    *lib = ret == 0 ? (const CellFontLibrary *)(uintptr_t)ea : NULL;
    return ret;
}
