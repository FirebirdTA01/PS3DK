/* cell/sail/renderer_audio.h — cellSail audio renderer adapter.
   Clean-room header. 5 entry points. PRXPTR on callback-table funcs. */
#ifndef __PS3DK_CELL_SAIL_RENDERER_AUDIO_H__
#define __PS3DK_CELL_SAIL_RENDERER_AUDIO_H__
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
typedef struct { void *pPcm ATTRIBUTE_PRXPTR; int status; uint64_t pts, reserved; } CellSailAudioFrameInfo;
typedef int (*CellSailRendererAudioFuncMakeup)(void*);
typedef int (*CellSailRendererAudioFuncCleanup)(void*);
typedef void (*CellSailRendererAudioFuncOpen)(void*,const CellSailAudioFormat*,uint32_t);
typedef void (*CellSailRendererAudioFuncClose)(void*);
typedef void (*CellSailRendererAudioFuncStart)(void*,bool);
typedef void (*CellSailRendererAudioFuncStop)(void*,bool);
typedef void (*CellSailRendererAudioFuncCancel)(void*);
typedef int (*CellSailRendererAudioFuncCheckout)(void*,CellSailAudioFrameInfo**);
typedef int (*CellSailRendererAudioFuncCheckin)(void*,CellSailAudioFrameInfo*);
typedef CellSailRendererAudioFuncCheckin CellSailRendererAudioFunkCheckin;
typedef struct {
    CellSailRendererAudioFuncMakeup   pMakeup   ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncCleanup  pCleanup  ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncOpen     pOpen     ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncClose    pClose    ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncStart    pStart    ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncStop     pStop     ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncCancel   pCancel   ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncCheckout pCheckout ATTRIBUTE_PRXPTR;
    CellSailRendererAudioFuncCheckin  pCheckin  ATTRIBUTE_PRXPTR;
} CellSailRendererAudioFuncs;
typedef struct { uint64_t internalData[32]; } CellSailRendererAudio;
typedef struct {
    uint32_t thisSize; /* SPRX ABI width */
    CellSailAudioFormat *pPreferredFormat ATTRIBUTE_PRXPTR;
} CellSailRendererAudioAttribute;
int cellSailRendererAudioInitialize(CellSailRendererAudio*,const CellSailRendererAudioFuncs*,void*,const CellSailRendererAudioAttribute*);
int cellSailRendererAudioFinalize(CellSailRendererAudio*);
void cellSailRendererAudioNotifyCallCompleted(CellSailRendererAudio*,int);
void cellSailRendererAudioNotifyFrameDone(CellSailRendererAudio*);
void cellSailRendererAudioNotifyOutputEos(CellSailRendererAudio*);
#ifdef __cplusplus
}
#endif
#endif
