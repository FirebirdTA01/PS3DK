/* cell/sail/sound_adapter.h — cellSail sound adapter. Clean-room. 7 entry points. */
#ifndef __PS3DK_CELL_SAIL_SOUND_ADAPTER_H__
#define __PS3DK_CELL_SAIL_SOUND_ADAPTER_H__
#include <stdint.h>
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
typedef int (*CellSailSoundAdapterFuncMakeup)(void*);
typedef int (*CellSailSoundAdapterFuncCleanup)(void*);
typedef int (*CellSailSoundAdapterFuncFormatChanged)(void*,CellSailAudioFormat*,uint32_t);
typedef struct {
    CellSailSoundAdapterFuncMakeup        pMakeup        ATTRIBUTE_PRXPTR;
    CellSailSoundAdapterFuncCleanup       pCleanup       ATTRIBUTE_PRXPTR;
    CellSailSoundAdapterFuncFormatChanged pFormatChanged ATTRIBUTE_PRXPTR;
} CellSailSoundAdapterFuncs;
typedef struct { float *pBuffer ATTRIBUTE_PRXPTR; uint32_t sessionId,tag; int status; uint64_t pts; } CellSailSoundFrameInfo;
typedef struct { uint64_t internalData[32]; } CellSailSoundAdapter;
int cellSailSoundAdapterInitialize(CellSailSoundAdapter*,const CellSailSoundAdapterFuncs*,void*);
int cellSailSoundAdapterFinalize(CellSailSoundAdapter*);
int cellSailSoundAdapterSetPreferredFormat(CellSailSoundAdapter*,const CellSailAudioFormat*);
int cellSailSoundAdapterGetFrame(CellSailSoundAdapter*,uint32_t,CellSailSoundFrameInfo*);
int cellSailSoundAdapterGetFormat(CellSailSoundAdapter*,CellSailAudioFormat*);
void cellSailSoundAdapterUpdateAvSync(CellSailSoundAdapter*,uint32_t,uint64_t);
int cellSailSoundAdapterPtsToTimePosition(CellSailSoundAdapter*,uint64_t,uint64_t*);
#ifdef __cplusplus
}
#endif
#endif
