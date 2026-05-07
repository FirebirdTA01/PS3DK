/* cell/sail/feeder_audio.h — cellSail audio feeder adapter. Clean-room. */
#ifndef __PS3DK_CELL_SAIL_FEEDER_AUDIO_H__
#define __PS3DK_CELL_SAIL_FEEDER_AUDIO_H__
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <cell/sail/common.h>
#include <cell/sail/common_rec.h>
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
typedef int (*CellSailFeederAudioFuncMakeup)(void*);
typedef int (*CellSailFeederAudioFuncCleanup)(void*);
typedef void (*CellSailFeederAudioFuncOpen)(void*,const CellSailAudioFormat*,uint64_t);
typedef void (*CellSailFeederAudioFuncClose)(void*);
typedef void (*CellSailFeederAudioFuncStart)(void*,uint64_t);
typedef void (*CellSailFeederAudioFuncStop)(void*,uint64_t);
typedef void (*CellSailFeederAudioFuncCancel)(void*);
typedef int (*CellSailFeederAudioFuncGetFormat)(void*,uint64_t,CellSailAudioFormat*);
typedef int (*CellSailFeederAudioFuncCheckout)(void*,CellSailFeederFrameInfo**);
typedef int (*CellSailFeederAudioFuncCheckin)(void*,CellSailFeederFrameInfo*);
typedef int (*CellSailFeederAudioFuncClear)(void*);
typedef struct {
    CellSailFeederAudioFuncMakeup   pMakeup   ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncCleanup  pCleanup  ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncOpen     pOpen     ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncClose    pClose    ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncStart    pStart    ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncStop     pStop     ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncCancel   pCancel   ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncGetFormat pGetFormat ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncCheckout pCheckout ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncCheckin  pCheckin  ATTRIBUTE_PRXPTR;
    CellSailFeederAudioFuncClear    pClear    ATTRIBUTE_PRXPTR;
} CellSailFeederAudioFuncs;
typedef struct { uint32_t thisSize; uint32_t reserved; } CellSailFeederAudioAttribute;
typedef struct { uint64_t internalData[64]; } CellSailFeederAudio;
int cellSailFeederAudioInitialize(CellSailFeederAudio*,const CellSailFeederAudioFuncs*,void*,CellSailFeederAudioAttribute*);
int cellSailFeederAudioFinalize(CellSailFeederAudio*);
void cellSailFeederAudioNotifyCallCompleted(CellSailFeederAudio*,int);
void cellSailFeederAudioNotifyFrameOut(CellSailFeederAudio*,uint32_t);
void cellSailFeederAudioNotifySessionEnd(CellSailFeederAudio*);
void cellSailFeederAudioNotifySessionError(CellSailFeederAudio*,int,bool);
#ifdef __cplusplus
}
#endif
#endif
