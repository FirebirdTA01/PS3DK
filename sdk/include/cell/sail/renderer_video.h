/* cell/sail/renderer_video.h — cellSail video renderer adapter.
   Clean-room header. 5 entry points. */
#ifndef __PS3DK_CELL_SAIL_RENDERER_VIDEO_H__
#define __PS3DK_CELL_SAIL_RENDERER_VIDEO_H__
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
typedef struct { void *pPic ATTRIBUTE_PRXPTR; int status; uint64_t pts, reserved; uint16_t interval; uint8_t structure; int8_t repeatNum; uint8_t reserved2[4]; } CellSailVideoFrameInfo;
typedef int (*CellSailRendererVideoFuncMakeup)(void*);
typedef int (*CellSailRendererVideoFuncCleanup)(void*);
typedef void (*CellSailRendererVideoFuncOpen)(void*,const CellSailVideoFormat*,uint32_t,uint32_t);
typedef void (*CellSailRendererVideoFuncClose)(void*);
typedef void (*CellSailRendererVideoFuncStart)(void*,bool);
typedef void (*CellSailRendererVideoFuncStop)(void*,bool,bool);
typedef void (*CellSailRendererVideoFuncCancel)(void*);
typedef int (*CellSailRendererVideoFuncCheckout)(void*,CellSailVideoFrameInfo**);
typedef int (*CellSailRendererVideoFuncCheckin)(void*,CellSailVideoFrameInfo*);
typedef CellSailRendererVideoFuncCheckin CellSailRendererVideoFunkCheckin;
typedef struct {
    CellSailRendererVideoFuncMakeup   pMakeup   ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncCleanup  pCleanup  ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncOpen     pOpen     ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncClose    pClose    ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncStart    pStart    ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncStop     pStop     ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncCancel   pCancel   ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncCheckout pCheckout ATTRIBUTE_PRXPTR;
    CellSailRendererVideoFuncCheckin  pCheckin  ATTRIBUTE_PRXPTR;
} CellSailRendererVideoFuncs;
typedef struct { uint64_t internalData[32]; } CellSailRendererVideo;
typedef struct { uint32_t thisSize; CellSailVideoFormat *pPreferredFormat ATTRIBUTE_PRXPTR; } CellSailRendererVideoAttribute;
int cellSailRendererVideoInitialize(CellSailRendererVideo*,const CellSailRendererVideoFuncs*,void*,const CellSailRendererVideoAttribute*);
int cellSailRendererVideoFinalize(CellSailRendererVideo*);
void cellSailRendererVideoNotifyCallCompleted(CellSailRendererVideo*,int);
void cellSailRendererVideoNotifyFrameDone(CellSailRendererVideo*);
void cellSailRendererVideoNotifyOutputEos(CellSailRendererVideo*);
#ifdef __cplusplus
}
#endif
#endif
