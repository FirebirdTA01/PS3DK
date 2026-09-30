/* cell/ovis.h - libovis, SPU code overlays (PPU side).
 *
 * The PPU prepares the overlay table: a 128-byte-aligned buffer in main
 * memory holding every overlay section of an SPU ELF, placed at the
 * section's load address minus 0x40000.  The SPU side (<cell/ovis.h> in the
 * SPU headers) DMAs sections from it into local store on demand.
 *
 *     int size = cellOvisGetOverlayTableSize(elf);
 *     void *table = memalign(128, size);
 *     cellOvisInitializeOverlayTable(table, elf);
 *     // pass the table's address to the SPU program
 *
 * The functions are implemented in libovis.a (link -lovis); they only read
 * the ELF and write the buffer, so they need no system module. */
#ifndef PS3TC_CELL_OVIS_H
#define PS3TC_CELL_OVIS_H

#include <cell/ovis/types.h>
#include <cell/ovis/util.h>
#include <cell/ovis/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of overlay table the SPU ELF image at elf needs (0 when it has no
 * overlay sections), or CELL_OVIS_ERROR_INVAL if elf is not an SPU ELF. */
int cellOvisGetOverlayTableSize(const char *elf);

/* Copy the overlay sections of the SPU ELF image at elf into the table at
 * ea_ovly_table (cellOvisGetOverlayTableSize bytes, 128-byte aligned). */
int cellOvisInitializeOverlayTable(void *ea_ovly_table, const char *elf);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_OVIS_H */
