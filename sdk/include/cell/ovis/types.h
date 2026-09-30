/* cell/ovis/types.h - the overlay table entry libovis shares between the
 * PPU and SPU sides.
 *
 * One entry per overlay section, 16 bytes: the section's LS address (vma),
 * its size rounded up to the section alignment, its load address in the
 * SPU ELF (lma, at or above 0x40000, past the end of local store) and a
 * word the SPU mapper sets while the section is resident.  The linker
 * script cellOvisMkLdscript / cellOvisConfigAuto writes builds the table
 * (_ovly_table, one __ovly_info_<section> symbol per entry). */
#ifndef PS3TC_CELL_OVIS_TYPES_H
#define PS3TC_CELL_OVIS_TYPES_H

#ifdef __SPU__
#include <spu_intrinsics.h>
/* words: vma, size, lma, mapped */
typedef vec_uint4 _ovly_table_t;
#else
#include <stdint.h>
typedef struct _ovly_table_t {
	uint32_t vma;
	uint32_t size;
	uint32_t lma;
	uint32_t mapped;
} __attribute__((aligned(16))) _ovly_table_t;
#endif

#endif /* PS3TC_CELL_OVIS_TYPES_H */
