#ifndef __PS3DK_GCM_TOOL_H__
#define __PS3DK_GCM_TOOL_H__

#include <stddef.h>
#include <stdint.h>

#ifndef CELL_GCM_DISPLAY_MAXID
#define CELL_GCM_DISPLAY_MAXID 8
#endif

#ifndef CELL_GCM_MAX_TILE_INDEX
#define CELL_GCM_MAX_TILE_INDEX 15
#endif

#ifndef CELL_GCM_MAX_ZCULL_INDEX
#define CELL_GCM_MAX_ZCULL_INDEX 7
#endif

typedef struct CellGcmConfig {
    void *localAddress;
    void *ioAddress;
    uint32_t localSize;
    uint32_t ioSize;
    uint32_t memoryFrequency;
    uint32_t coreFrequency;
} CellGcmConfig;

typedef struct CellGcmOffsetTable {
    uint16_t *io;
    uint16_t *ea;
} CellGcmOffsetTable;

typedef struct CellGcmReportData {
    uint64_t timer;
    uint32_t value;
    uint32_t zero;
} CellGcmReportData;

typedef struct CellGcmContextData {
    uint32_t *begin;
    uint32_t *end;
    uint32_t *current;
    int32_t (*callback)(struct CellGcmContextData *context, uint32_t count);
} CellGcmContextData;

typedef struct CellGcmSurface {
    uint8_t type;
    uint8_t antiAlias;
    uint8_t colorFormat;
    uint8_t colorTarget;
    uint8_t colorLocation[4];
    uint32_t colorOffset[4];
    uint32_t colorPitch[4];
    uint8_t depthFormat;
    uint8_t depthLocation;
    uint8_t reserved[2];
    uint32_t depthOffset;
    uint32_t depthPitch;
    uint16_t width;
    uint16_t height;
    uint16_t x;
    uint16_t y;
} CellGcmSurface;

#endif /* __PS3DK_GCM_TOOL_H__ */
