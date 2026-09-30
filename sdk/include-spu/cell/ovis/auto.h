/* cell/ovis/auto.h - automatic overlay mapping.
 *
 * With cellOvisConfigAuto, every call into an overlay object goes through
 * a generated wrapper that maps the object's section first
 * (_cellOvisUpdateAndMapSection), using the table address and DMA tag set
 * here.  Call this once before the first overlaid call. */
#ifndef PS3TC_CELL_OVIS_AUTO_H
#define PS3TC_CELL_OVIS_AUTO_H

#include <stdint.h>
#include <cell/ovis/error.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int gCellOvisTag;
extern uint64_t gCellOvisTable;

#ifdef __cplusplus
}
#endif

static inline int cellOvisInitializeAutoMapping(uint64_t eaOvlyTable, uint32_t tag)
{
	if (tag >= 32)
		return CELL_OVIS_ERROR_INVAL;
	if (eaOvlyTable % 128 != 0)
		return CELL_OVIS_ERROR_ALIGN;
	gCellOvisTag = tag;
	gCellOvisTable = eaOvlyTable;
	return CELL_OK;
}

#endif /* PS3TC_CELL_OVIS_AUTO_H */
