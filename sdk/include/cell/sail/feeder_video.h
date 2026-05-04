/* cell/sail/feeder_video.h — cellSail video feeder adapter. Clean-room. */
#ifndef __PS3DK_CELL_SAIL_FEEDER_VIDEO_H__
#define __PS3DK_CELL_SAIL_FEEDER_VIDEO_H__
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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
typedef int (*CellSailFeederVideoFuncMakeup)(void*);
typedef int (*CellSailFeederVideoFuncCleanup)(void*);
typedef void (*CellSailFeederVideoFuncOpen)(void*,const CellSailVideoFormat*,uint64_t);
typedef void (*CellSailFeederVideoFuncClose)(void*);
typedef void (*CellSailFeederVideoFuncStart)(void*,uint64_t);
typedef void (*CellSailFeederVideoFuncStop)(void*,uint64_t);
typedef void (*CellSailFeederVideoFuncCancel)(void*);
typedef int (*CellSailFeederVideoFuncGetFormat)(void*,uint64_t,CellSailVideoFormat*);
typedef int (*CellSailFeederVideoFuncCheckout)(void*,CellSailFeederFrameInfo**);
typedef int (*CellSailFeederVideoFuncCheckin)(void*,CellSailFeederFrameInfo*);
typedef int (*CellSailFeederVideoFuncClear)(void*);
typedef struct {
    CellSailFeederVideoFuncMakeup   pMakeup   ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncCleanup  pCleanup  ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncOpen     pOpen     ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncClose    pClose    ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncStart    pStart    ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncStop     pStop     ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncCancel   pCancel   ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncGetFormat pGetFormat ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncCheckout pCheckout ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncCheckin  pCheckin  ATTRIBUTE_PRXPTR;
    CellSailFeederVideoFuncClear    pClear    ATTRIBUTE_PRXPTR;
} CellSailFeederVideoFuncs;
typedef struct { uint32_t thisSize; uint32_t reserved; } CellSailFeederVideoAttribute;
typedef struct { uint64_t internalData[64]; } CellSailFeederVideo;
int cellSailFeederVideoInitialize(CellSailFeederVideo*,const CellSailFeederVideoFuncs*,void*,CellSailFeederVideoAttribute*);
int cellSailFeederVideoFinalize(CellSailFeederVideo*);
void cellSailFeederVideoNotifyCallCompleted(CellSailFeederVideo*,int);
void cellSailFeederVideoNotifyFrameOut(CellSailFeederVideo*,uint32_t);
void cellSailFeederVideoNotifySessionEnd(CellSailFeederVideo*);
void cellSailFeederVideoNotifySessionError(CellSailFeederVideo*,int,bool);
#ifdef __cplusplus
}
#endif
#endif
