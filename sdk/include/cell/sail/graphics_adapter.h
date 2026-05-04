/* cell/sail/graphics_adapter.h — cellSail graphics adapter. Clean-room. 8 entry points. */
#ifndef __PS3DK_CELL_SAIL_GRAPHICS_ADAPTER_H__
#define __PS3DK_CELL_SAIL_GRAPHICS_ADAPTER_H__
#include <stdint.h>
#include <stddef.h>
#include <cell/sail/common.h>
#ifdef __PPU__
#include <ppu-types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { CELL_SAIL_GRAPHICS_ADAPTER_FIELD_TOP=0, CELL_SAIL_GRAPHICS_ADAPTER_FIELD_BOTTOM=1, CELL_SAIL_GRAPHICS_ADAPTER_FIELD_DONT_CARE=2 };
typedef int (*CellSailGraphicsAdapterFuncMakeup)(void*);
typedef int (*CellSailGraphicsAdapterFuncCleanup)(void*);
typedef int (*CellSailGraphicsAdapterFuncFormatChanged)(void*,CellSailVideoFormat*,uint32_t);
typedef int (*CellSailGraphicsAdapterFuncAllocFrame)(void*,uint32_t,int,uint8_t**);
typedef int (*CellSailGraphicsAdapterFuncFreeFrame)(void*,int,uint8_t**);
typedef struct {
    CellSailGraphicsAdapterFuncMakeup        pMakeup        ATTRIBUTE_PRXPTR;
    CellSailGraphicsAdapterFuncCleanup       pCleanup       ATTRIBUTE_PRXPTR;
    CellSailGraphicsAdapterFuncFormatChanged pFormatChanged ATTRIBUTE_PRXPTR;
    CellSailGraphicsAdapterFuncAllocFrame    pAlloc         ATTRIBUTE_PRXPTR;
    CellSailGraphicsAdapterFuncFreeFrame     pFree          ATTRIBUTE_PRXPTR;
} CellSailGraphicsAdapterFuncs;
typedef struct { uint8_t *pBuffer ATTRIBUTE_PRXPTR; uint32_t sessionId,tag; int status; uint64_t pts; } CellSailGraphicsFrameInfo;
typedef struct { uint64_t internalData[32]; } CellSailGraphicsAdapter;
int cellSailGraphicsAdapterInitialize(CellSailGraphicsAdapter*,const CellSailGraphicsAdapterFuncs*,void*);
int cellSailGraphicsAdapterFinalize(CellSailGraphicsAdapter*);
int cellSailGraphicsAdapterSetPreferredFormat(CellSailGraphicsAdapter*,const CellSailVideoFormat*);
int cellSailGraphicsAdapterGetFrame(CellSailGraphicsAdapter*,CellSailGraphicsFrameInfo*);
int cellSailGraphicsAdapterGetFrame2(CellSailGraphicsAdapter*,CellSailGraphicsFrameInfo*,CellSailGraphicsFrameInfo*,uint64_t*,uint64_t);
int cellSailGraphicsAdapterGetFormat(CellSailGraphicsAdapter*,CellSailVideoFormat*);
void cellSailGraphicsAdapterUpdateAvSync(CellSailGraphicsAdapter*,uint32_t,uint64_t);
int cellSailGraphicsAdapterPtsToTimePosition(CellSailGraphicsAdapter*,uint64_t,uint64_t*);
#ifdef __cplusplus
}
#endif
#endif
