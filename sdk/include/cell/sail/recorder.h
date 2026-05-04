/* cell/sail/recorder.h — cellSailRecorder. Clean-room. */
#ifndef __PS3DK_CELL_SAIL_RECORDER_H__
#define __PS3DK_CELL_SAIL_RECORDER_H__
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <cell/sail/common.h>
#include <cell/sail/common_rec.h>
#include <cell/sail/muxer.h>
#include <cell/sail/source.h>
#include <cell/sail/descriptor.h>
#include <cell/sail/feeder_audio.h>
#include <cell/sail/feeder_video.h>
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
typedef void (*CellSailRecorderFuncNotified)(void*,CellSailEvent,uint64_t,uint64_t);
typedef enum { CELL_SAIL_RECORDER_STATE_INITIALIZED=0, CELL_SAIL_RECORDER_STATE_BOOT_TRANSITION=1, CELL_SAIL_RECORDER_STATE_CLOSED=2, CELL_SAIL_RECORDER_STATE_OPEN_TRANSITION=3, CELL_SAIL_RECORDER_STATE_OPENED=4, CELL_SAIL_RECORDER_STATE_START_TRANSITION=5, CELL_SAIL_RECORDER_STATE_RUNNING=6, CELL_SAIL_RECORDER_STATE_STOP_TRANSITION=7, CELL_SAIL_RECORDER_STATE_CLOSE_TRANSITION=8, CELL_SAIL_RECORDER_STATE_LOST=9 } CellSailRecorderStateType;
typedef enum { CELL_SAIL_RECORDER_PRESET_AV_SYNC_AUTO_DETECT=0, CELL_SAIL_RECORDER_PRESET_AS_IS=1 } CellSailRecorderPresetType;
typedef struct { uint64_t internalData[128]; } CellSailRecorder;
int cellSailRecorderInitialize(CellSailRecorder*,CellSailRecorderFuncNotified,void*,CellSailRecorderPresetType);
int cellSailRecorderFinalize(CellSailRecorder*);
int cellSailRecorderBoot(CellSailRecorder*,uint32_t,uint32_t,uint64_t,uint64_t);
int cellSailRecorderOpenStream(CellSailRecorder*,const char*,int,void*,const char*,CellSailSourceStreamingProfile*);
int cellSailRecorderCloseStream(CellSailRecorder*);
int cellSailRecorderOpenEsAudio(CellSailRecorder*,int,const CellSailAudioFormat*);
int cellSailRecorderOpenEsVideo(CellSailRecorder*,int,const CellSailVideoFormat*);
int cellSailRecorderOpenEsUser(CellSailRecorder*,int);
int cellSailRecorderCloseEsAudio(CellSailRecorder*,int);
int cellSailRecorderCloseEsVideo(CellSailRecorder*,int);
int cellSailRecorderCloseEsUser(CellSailRecorder*,int);
int cellSailRecorderSetStartOffset(CellSailRecorder*,uint64_t);
int cellSailRecorderSetPause(CellSailRecorder*,bool);
int cellSailRecorderSetParameter(CellSailRecorder*,int,int64_t);
int cellSailRecorderGetParameter(CellSailRecorder*,int,int64_t*);
int cellSailRecorderGetState(CellSailRecorder*,CellSailRecorderStateType*);
int cellSailRecorderGetElapsedTime(CellSailRecorder*,uint64_t*);
int cellSailRecorderGetRemainingTime(CellSailRecorder*,uint64_t*);
int cellSailRecorderGetLastError(CellSailRecorder*,uint64_t*,uint64_t*);
int cellSailRecorderAddDescriptor(CellSailRecorder*,CellSailDescriptor*);
int cellSailRecorderAddSource(CellSailRecorder*,CellSailSource*);
int cellSailRecorderAddFeederAudio(CellSailRecorder*,CellSailFeederAudio*);
int cellSailRecorderAddFeederVideo(CellSailRecorder*,CellSailFeederVideo*);
int cellSailRecorderGetCaptureFrameInfo(CellSailRecorder*,CellSailCaptureFrameInfo*,CellSailCaptureFrameInfo*,int);
#ifdef __cplusplus
}
#endif
#endif
