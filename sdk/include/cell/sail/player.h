/* cell/sail/player.h — cellSail player. Clean-room. 25+ entry points. PRXPTR on PlayerFuncNotified. */
#ifndef __PS3DK_CELL_SAIL_PLAYER_H__
#define __PS3DK_CELL_SAIL_PLAYER_H__
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
#define CELL_SAIL_MAX_LENGTH_PROTOCOL_NAME_WITH_NULL_TERMINATE 32
#define CELL_SAIL_MAX_NUM_DESCRIPTORS 2
#define CELL_SAIL_PROTOCOL_CELL_FS "x-cell-fs"
typedef void (*CellSailPlayerFuncNotified)(void*,CellSailEvent,uint64_t,uint64_t);
typedef enum { CELL_SAIL_PLAYER_STATE_INITIALIZED=0, CELL_SAIL_PLAYER_STATE_BOOT_TRANSITION=1, CELL_SAIL_PLAYER_STATE_CLOSED=2, CELL_SAIL_PLAYER_STATE_OPEN_TRANSITION=3, CELL_SAIL_PLAYER_STATE_OPENED=4, CELL_SAIL_PLAYER_STATE_START_TRANSITION=5, CELL_SAIL_PLAYER_STATE_RUNNING=6, CELL_SAIL_PLAYER_STATE_STOP_TRANSITION=7, CELL_SAIL_PLAYER_STATE_CLOSE_TRANSITION=8, CELL_SAIL_PLAYER_STATE_LOST=9 } CellSailPlayerStateType;
typedef enum { CELL_SAIL_PLAYER_CALL_NONE=0, CELL_SAIL_PLAYER_CALL_BOOT=1, CELL_SAIL_PLAYER_CALL_OPEN_STREAM=2, CELL_SAIL_PLAYER_CALL_CLOSE_STREAM=3, CELL_SAIL_PLAYER_CALL_OPEN_ES_AUDIO=4, CELL_SAIL_PLAYER_CALL_OPEN_ES_VIDEO=5, CELL_SAIL_PLAYER_CALL_OPEN_ES_USER=6, CELL_SAIL_PLAYER_CALL_CLOSE_ES_AUDIO=7, CELL_SAIL_PLAYER_CALL_CLOSE_ES_VIDEO=8, CELL_SAIL_PLAYER_CALL_CLOSE_ES_USER=9, CELL_SAIL_PLAYER_CALL_START=10, CELL_SAIL_PLAYER_CALL_STOP=11, CELL_SAIL_PLAYER_CALL_NEXT=12, CELL_SAIL_PLAYER_CALL_REOPEN_ES_AUDIO=13, CELL_SAIL_PLAYER_CALL_REOPEN_ES_VIDEO=14, CELL_SAIL_PLAYER_CALL_REOPEN_ES_USER=15 } CellSailPlayerCallType;
typedef enum { CELL_SAIL_PLAYER_PRESET_AV_SYNC=0, CELL_SAIL_PLAYER_PRESET_AS_IS=1, CELL_SAIL_PLAYER_PRESET_AV_SYNC_59_94HZ=2, CELL_SAIL_PLAYER_PRESET_AV_SYNC_29_97HZ=3, CELL_SAIL_PLAYER_PRESET_AV_SYNC_50HZ=4, CELL_SAIL_PLAYER_PRESET_AV_SYNC_25HZ=5, CELL_SAIL_PLAYER_PRESET_AV_SYNC_AUTO_DETECT=6 } CellSailPlayerPresetType;

typedef struct { uint64_t internalData[256]; } CellSailPlayer;

int cellSailPlayerInitialize(CellSailPlayer*,CellSailPlayerFuncNotified,void*,CellSailPlayerPresetType);
int cellSailPlayerInitialize2(CellSailPlayer*,CellSailPlayerFuncNotified,void*,CellSailPlayerPresetType,unsigned,unsigned);
int cellSailPlayerFinalize(CellSailPlayer*);
int cellSailPlayerBoot(CellSailPlayer*,uint32_t*,uint32_t*,uint64_t,uint64_t);
int cellSailPlayerAddDescriptor(CellSailPlayer*,CellSailDescriptor*);
int cellSailPlayerAddSource(CellSailPlayer*,CellSailSource*);
int cellSailPlayerAddGraphicsAdapter(CellSailPlayer*,CellSailGraphicsAdapter*);
int cellSailPlayerAddSoundAdapter(CellSailPlayer*,CellSailSoundAdapter*);
int cellSailPlayerAddAuReceiver(CellSailPlayer*,CellSailAuReceiver*,uint64_t);
int cellSailPlayerAddRendererAudio(CellSailPlayer*,CellSailRendererAudio*);
int cellSailPlayerAddRendererVideo(CellSailPlayer*,CellSailRendererVideo*);
int cellSailPlayerAddFeederAudio(CellSailPlayer*,CellSailFeederAudio*);
int cellSailPlayerAddFeederVideo(CellSailPlayer*,CellSailFeederVideo*);
int cellSailPlayerOpenStream(CellSailPlayer*,const char*,int,void*,const char*,CellSailSourceStreamingProfile*);
int cellSailPlayerCloseStream(CellSailPlayer*);
int cellSailPlayerOpenEsAudio(CellSailPlayer*,int,const CellSailAudioFormat*);
int cellSailPlayerOpenEsVideo(CellSailPlayer*,int,const CellSailVideoFormat*);
int cellSailPlayerOpenEsUser(CellSailPlayer*,int);
int cellSailPlayerCloseEsAudio(CellSailPlayer*,int);
int cellSailPlayerCloseEsVideo(CellSailPlayer*,int);
int cellSailPlayerCloseEsUser(CellSailPlayer*,int);
int cellSailPlayerReopenEsAudio(CellSailPlayer*,int,const CellSailAudioFormat*);
int cellSailPlayerReopenEsVideo(CellSailPlayer*,int,const CellSailVideoFormat*);
int cellSailPlayerReopenEsUser(CellSailPlayer*,int);
int cellSailPlayerSetStartOffset(CellSailPlayer*,uint64_t);
int cellSailPlayerSetLoop(CellSailPlayer*,bool);
int cellSailPlayerSetPause(CellSailPlayer*,bool);
int cellSailPlayerSetNext(CellSailPlayer*);
int cellSailPlayerSetParameter(CellSailPlayer*,int,int64_t);
int cellSailPlayerGetParameter(CellSailPlayer*,int,int64_t*);
int cellSailPlayerGetState(CellSailPlayer*,CellSailPlayerStateType*);
int cellSailPlayerGetElapsedTime(CellSailPlayer*,uint64_t*);
int cellSailPlayerGetElapsedTimeInTimeProgress(CellSailPlayer*,uint64_t*);
int cellSailPlayerGetRemainingTime(CellSailPlayer*,uint64_t*);
int cellSailPlayerGetWholeTime(CellSailPlayer*,uint64_t*);
int cellSailPlayerGetLastError(CellSailPlayer*,uint64_t*,uint64_t*);
int cellSailPlayerControl(CellSailPlayer*,int,int64_t);
#ifdef __cplusplus
}
#endif
#endif
