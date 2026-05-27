/*
 * PS3 Custom Toolchain - cell/font/libfontFT.h
 *
 * cellFont FreeType surface: 3 exports backed by libcellFontFT_stub.a.
 */
#ifndef _PS3DK_CELL_FONT_LIBFONTFT_H_
#define _PS3DK_CELL_FONT_LIBFONTFT_H_

#include <stdint.h>
#include <cell/font/libfont.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_FONT_LIBRARY_TYPE_FREETYPE (2)

typedef struct CellFontLibraryConfigFT {
	void *library;
	CellFontMemoryInterface MemoryIF;
} CellFontLibraryConfigFT;

typedef CellFontRendererConfig CellFontRendererConfigFT;

int cellFontInitLibraryFreeTypeWithRevision(
	uint64_t revisionFlags,
	CellFontLibraryConfigFT *config,
	const CellFontLibrary **lib);

int cellFontFTGetRevisionFlags(const CellFontLibrary *lib,
                               uint64_t *revisionFlags);

int cellFontFTGetInitializedRevisionFlags(
	const CellFontLibrary *lib,
	uint64_t *revisionFlags);

static inline int cellFontInitLibraryFreeType(
	CellFontLibraryConfigFT *config,
	const CellFontLibrary **lib)
{
	extern void cellFontFTGetStubRevisionFlags(
		uint64_t *revisionFlags);
	uint64_t revisionFlags;
	cellFontFTGetStubRevisionFlags(&revisionFlags);
	return cellFontInitLibraryFreeTypeWithRevision(
		revisionFlags, config, lib);
}

#ifdef __cplusplus
}
#endif

#endif
