/*
 * PS3 Custom Toolchain -- <cell/dbgrsx/dbgrsx_struct.h>
 *
 * Struct and union types for the cellDbgRsx library (libdbgrsx.a).
 */

#ifndef __PS3DK_CELL_DBGRSX_STRUCT_H__
#define __PS3DK_CELL_DBGRSX_STRUCT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Tile / Zcull --- */

typedef struct CellDbgRsxTileRegion {
	uint32_t enable;
	uint32_t bank;
	uint32_t start;
	uint32_t limit;
	uint32_t pitch;
	uint32_t base_tag;
	uint32_t limit_tag;
	uint32_t compmode;
} __attribute__((aligned(4))) CellDbgRsxTileRegion;

typedef struct CellDbgRsxZcullRegion {
	uint32_t enable;
	uint32_t format;
	uint32_t antialias;
	uint32_t width;
	uint32_t height;
	uint32_t start;
	uint32_t offset;
} __attribute__((aligned(4))) CellDbgRsxZcullRegion;

typedef struct CellDbgRsxZcullStatus {
	uint32_t valid;
	uint32_t zdir;
	uint32_t zformat;
	uint32_t sfunc;
	uint32_t sref;
	uint32_t smask;
	uint16_t pushbacklimit;
	uint16_t moveforwardlimit;
} __attribute__((aligned(4))) CellDbgRsxZcullStatus;

typedef struct CellDbgRsxZcullMatch {
	uint32_t valid;
	uint32_t region;
} __attribute__((aligned(4))) CellDbgRsxZcullMatch;

/* --- Cursor --- */

typedef struct CellDbgRsxCursor {
	uint32_t bitmap[128];
	uint16_t palette[16][4];
	uint32_t magnification_factor;
} __attribute__((aligned(4))) CellDbgRsxCursor;

/* --- Fifo cache / graphics fifo --- */

typedef struct CellDbgRsxFifoCache {
	uint16_t type;
	uint16_t addr;
	uint32_t data;
} __attribute__((aligned(4))) CellDbgRsxFifoCache;

typedef struct CellDbgRsxGraphicsFifo {
	uint16_t type;
	uint16_t addr;
	uint32_t data1;
	uint32_t data2;
} __attribute__((aligned(4))) CellDbgRsxGraphicsFifo;

/* --- Error state --- */

typedef union CellDbgRsxErrorState {
	struct { uint32_t value[4];  } ui32;
	struct { uint16_t value[8];  } ui16;
	struct { uint8_t  value[16]; } ui8;
} __attribute__((aligned(4))) CellDbgRsxErrorState;

/* --- Method check --- */

typedef struct CellDbgRsxMethodCheckState {
	uint8_t  state;
	uint32_t func_id;
	uint32_t arg_id;
	uint32_t value;
	uint8_t  error_info;
} __attribute__((aligned(4))) CellDbgRsxMethodCheckState;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_DBGRSX_STRUCT_H__ */
