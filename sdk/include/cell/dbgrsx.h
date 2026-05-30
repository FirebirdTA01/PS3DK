/*
 * PS3 Custom Toolchain -- <cell/dbgrsx.h>
 *
 * Umbrella header for the cellDbgRsx library (libdbgrsx.a).
 * Provides tile/region inspection, Zcull introspection, RSX
 * interrupt/error-state queries, FIFO cache inspection, cursor
 * control, and graphics pipeline debugging.
 */

#ifndef __PS3DK_CELL_DBGRSX_H__
#define __PS3DK_CELL_DBGRSX_H__

#include <stdint.h>
#include <cell/error.h>
#include <cell/dbgrsx/dbgrsx_enum.h>
#include <cell/dbgrsx/dbgrsx_struct.h>
#include <cell/dbgrsx/dbgrsx_error.h>
#include <cell/dbgrsx/dbgrsx_bundle_state_struct.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Init / Exit --- */

int32_t cellDbgRsxInit(void);
int32_t cellDbgRsxExit(void);

/* --- Tile / Zcull region --- */

int32_t cellDbgRsxGetTileRegion(uint8_t index, CellDbgRsxTileRegion *region);
int32_t cellDbgRsxGetZcullRegion(uint8_t index, CellDbgRsxZcullRegion *zcRegion);
int32_t cellDbgRsxGetZcullStatus(uint8_t index, CellDbgRsxZcullStatus *zcStatus);
int32_t cellDbgRsxGetZcullMatch(CellDbgRsxZcullMatch *zcMatch);

/* --- Zcull RAM sizing --- */

uint32_t cellDbgRsxGetZcullNearRamSize(uint32_t pixel_width, uint32_t pixel_height);
uint32_t cellDbgRsxGetZcullFarRamSize(uint32_t pixel_width, uint32_t pixel_height);
uint32_t cellDbgRsxGetZcullStencilRamSize(uint32_t pixel_width, uint32_t pixel_height, uint32_t antialias);

/* --- Zcull RAM texture sizes --- */

int32_t cellDbgRsxGetZcullNearRamTextureSize(uint32_t pixel_width, uint32_t pixel_height,
	uint32_t *tex_width, uint32_t *tex_height, uint32_t *tex_format);
int32_t cellDbgRsxGetZcullFarRamTextureSize(uint32_t pixel_width, uint32_t pixel_height,
	uint32_t *tex_width, uint32_t *tex_height, uint32_t *tex_format);
int32_t cellDbgRsxGetZcullStencilRamTextureSize(uint32_t pixel_width, uint32_t pixel_height,
	uint32_t antialias, uint32_t *tex_width, uint32_t *tex_height, uint32_t *tex_format);

/* --- Zcull RAM readback --- */

int32_t cellDbgRsxGetZcullNearRam(uint16_t *data, uint32_t pixel_width, uint32_t pixel_height,
	uint32_t zcstart, uint32_t zdir, uint32_t zformat);
int32_t cellDbgRsxGetZcullFarRam(uint16_t *data, uint32_t pixel_width, uint32_t pixel_height,
	uint32_t zcstart, uint32_t zdir, uint32_t zformat);
int32_t cellDbgRsxGetZcullStencilRam(uint8_t *data, uint32_t pixel_width, uint32_t pixel_height,
	uint32_t zcstart, uint32_t antialias);

/* --- Cursor --- */

int32_t cellDbgRsxEnableCursor(const CellDbgRsxCursor *cursor, const uint32_t x, const uint32_t y);
int32_t cellDbgRsxDisableCursor(void);
int32_t cellDbgRsxSetCursorPosition(const uint32_t x, const uint32_t y);
int32_t cellDbgRsxGetCursorStatus(int32_t *status);
int32_t cellDbgRsxSetCursorDefaultValues(CellDbgRsxCursor *cursor);
int32_t cellDbgRsxSetCursorBitmap(CellDbgRsxCursor *cursor, const uint8_t *source_bitmap);
int32_t cellDbgRsxSetCursorPalette(CellDbgRsxCursor *cursor, const uint8_t *source_palette);
int32_t cellDbgRsxSetCursorMagnificationFactor(CellDbgRsxCursor *cursor, const uint32_t magnification_factor);

/* --- Interrupt error status --- */

int32_t cellDbgRsxGetInterruptErrorStatus(uint32_t *status);
int32_t cellDbgRsxGetInterruptFifoErrorStatus(uint32_t *status);
int32_t cellDbgRsxGetInterruptIoifErrorStatus(uint32_t *status);
int32_t cellDbgRsxGetInterruptGraphicsErrorStatus(uint64_t *status);
int32_t cellDbgRsxGetInterruptMiscErrorStatus(uint32_t *status);

/* --- FIFO cache / graphics FIFO --- */

int32_t cellDbgRsxGetFifoCacheIndex(CellDbgRsxFifoCache *cache, int32_t index);
int32_t cellDbgRsxGetFifoCache(CellDbgRsxFifoCache *cache);
int32_t cellDbgRsxGetGraphicsFifo(CellDbgRsxGraphicsFifo *fifo);

/* --- Error state queries --- */

int32_t cellDbgRsxGetFifoErrorState(int32_t id, CellDbgRsxErrorState *state);
int32_t cellDbgRsxGetIoifErrorState(int32_t id, CellDbgRsxErrorState *state);
int32_t cellDbgRsxGetGraphicsErrorState(int32_t id, CellDbgRsxErrorState *state);
int32_t cellDbgRsxGetGraphicsLaunchCheckState(uint32_t id, CellDbgRsxErrorState *state);
int32_t cellDbgRsxGetGraphicsMethodCheckState(uint32_t addr, uint32_t data, CellDbgRsxMethodCheckState *state);

/* --- Bundle state --- */

int32_t cellDbgRsxGetGraphicsBundleState(int32_t id, uint32_t *data);

/* --- GCM method extraction --- */

int32_t cellDbgRsxExtractGcmMethod(const uint32_t *gcm_method, uint32_t index,
	CellDbgRsxFifoCache *fifo_cache);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_DBGRSX_H__ */
