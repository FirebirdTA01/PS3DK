/* cell/ovis/mapper.h - mapping overlay sections by hand.
 *
 *     CELL_OVIS_DEFINE_MAPPER(sec_quicksort)      // at file scope
 *     ...
 *     if (cellOvisStartMapping_sec_quicksort(ea_table, tag) == CELL_OK)
 *         cellOvisWaitMapping_sec_quicksort(tag);
 *     quicksort(...);
 *
 * StartMapping DMAs the section from the overlay table in main memory (see
 * cellOvisInitializeOverlayTable on the PPU) into its LS address with the
 * given tag, and marks every section sharing that LS range as no longer
 * resident.  It returns CELL_OVIS_ERROR_ABORT, transferring nothing, when
 * the section is already resident.  WaitMapping waits for the tag, then
 * marks the section resident. */
#ifndef PS3TC_CELL_OVIS_MAPPER_H
#define PS3TC_CELL_OVIS_MAPPER_H

#include <stdint.h>
#include <cell/ovis/debug.h>
#include <cell/ovis/error.h>

/* Load addresses of overlay sections start here, past the end of LS. */
#define sOvisLsSize 0x040000

#ifdef __cplusplus
extern "C" {
#endif

/* offset: how far the program was loaded from its link address (0 for a
 * program that runs where it was linked); the section lands at vma+offset. */
int _cellOvisStartMapping(_ovly_table_t __ovly_info, uint64_t ea_ovly_table,
                          uint32_t tag, uint32_t offset);
void _cellOvisWaitMapping(_ovly_table_t *__ovly_info, uint32_t tag);

#ifdef __cplusplus
}
#endif

/* The load offset of the calling code: run-time address of a label minus
 * its link-time address. */
#define __CELL_OVIS_LOAD_OFFSET(out)                          \
	do {                                                      \
		uint32_t __ovis_t;                                    \
		__asm__ volatile("brsl %0, 1f\n"                      \
		                 "1:\n\t"                             \
		                 "ila %1, 1b\n\t"                     \
		                 "sf %0, %1, %0"                      \
		                 : "=&r"(out), "=&r"(__ovis_t));      \
	} while (0)

#define CELL_OVIS_DEFINE_MAPPER(secname)                                          \
extern _ovly_table_t __ovly_info_##secname;                                       \
static inline __attribute__((always_inline))                                      \
int cellOvisStartMapping_##secname(uint64_t ea_ovly_table, uint32_t tag)          \
{                                                                                 \
	uint32_t __ovis_offset;                                                       \
	if (ea_ovly_table % 128 != 0)                                                 \
		return CELL_OVIS_ERROR_ALIGN;                                             \
	if (tag >= 32)                                                                \
		return CELL_OVIS_ERROR_INVAL;                                             \
	__CELL_OVIS_LOAD_OFFSET(__ovis_offset);                                       \
	return _cellOvisStartMapping(__ovly_info_##secname, ea_ovly_table, tag,       \
	                             __ovis_offset);                                  \
}                                                                                 \
static inline int cellOvisWaitMapping_##secname(uint32_t tag)                     \
{                                                                                 \
	if (tag >= 32)                                                                \
		return CELL_OVIS_ERROR_INVAL;                                             \
	_cellOvisWaitMapping(&__ovly_info_##secname, tag);                            \
	return CELL_OK;                                                               \
}

#endif /* PS3TC_CELL_OVIS_MAPPER_H */
