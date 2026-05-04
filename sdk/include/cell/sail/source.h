/* cell/sail/source.h — cellSail source adapter. Clean-room. 3 entry points + diag handler. PRXPTR on callback table. */
#ifndef __PS3DK_CELL_SAIL_SOURCE_H__
#define __PS3DK_CELL_SAIL_SOURCE_H__
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
enum { CELL_SAIL_SOURCE_SEEK_ABSOLUTE_BYTE_POSITION=1<<0 };
enum { CELL_SAIL_SOURCE_CAPABILITY_NONE=0, CELL_SAIL_SOURCE_CAPABILITY_SEEK_ABSOLUTE_BYTE_POSITION=1<<0, CELL_SAIL_SOURCE_CAPABILITY_PAUSE=1<<4, CELL_SAIL_SOURCE_CAPABILITY_GAPLESS=1<<5, CELL_SAIL_SOURCE_CAPABILITY_EOS=1<<6, CELL_SAIL_SOURCE_CAPABILITY_SEEK_ABSOLUTE_TIME_POSITION=1<<7 };
typedef struct CellSailSourceStartCommand { uint64_t startFlags; int64_t startArg, lengthArg; uint64_t optionalArg0, optionalArg1; } CellSailSourceStartCommand;
typedef struct CellSailSourceStreamingProfile { uint32_t reserved0, numItems, maxBitrate, reserved1; uint64_t duration, streamSize; } CellSailSourceStreamingProfile;
typedef int (*CellSailSourceFuncMakeup)(void*,const char*);
typedef int (*CellSailSourceFuncCleanup)(void*);
typedef void (*CellSailSourceFuncOpen)(void*,int,void*,const char*,const CellSailSourceStreamingProfile*);
typedef void (*CellSailSourceFuncClose)(void*);
typedef void (*CellSailSourceFuncStart)(void*,const CellSailSourceStartCommand*,uint32_t);
typedef void (*CellSailSourceFuncStop)(void*);
typedef void (*CellSailSourceFuncCancel)(void*);
typedef int (*CellSailSourceFuncCheckout)(void*,const CellSailSourceBufferItem**);
typedef int (*CellSailSourceFuncCheckin)(void*,const CellSailSourceBufferItem*);
typedef int (*CellSailSourceFuncClear)(void*);
typedef int (*CellSailSourceFuncRead)(void*,int,void*,const char*,uint64_t,uint8_t*,uint32_t,uint64_t*);
typedef int (*CellSailSourceFuncReadSync)(void*,int,void*,const char*,uint64_t,uint8_t*,uint32_t,uint64_t*);
typedef int (*CellSailSourceFuncGetCapabilities)(void*,int,void*,const char*,uint64_t*);
typedef int (*CellSailSourceFuncInquireCapability)(void*,int,void*,const char*,const CellSailSourceStartCommand*);
typedef struct {
    CellSailSourceFuncMakeup pMakeup ATTRIBUTE_PRXPTR; CellSailSourceFuncCleanup pCleanup ATTRIBUTE_PRXPTR;
    CellSailSourceFuncOpen pOpen ATTRIBUTE_PRXPTR; CellSailSourceFuncClose pClose ATTRIBUTE_PRXPTR;
    CellSailSourceFuncStart pStart ATTRIBUTE_PRXPTR; CellSailSourceFuncStop pStop ATTRIBUTE_PRXPTR;
    CellSailSourceFuncCancel pCancel ATTRIBUTE_PRXPTR; CellSailSourceFuncCheckout pCheckout ATTRIBUTE_PRXPTR;
    CellSailSourceFuncCheckin pCheckin ATTRIBUTE_PRXPTR; CellSailSourceFuncClear pClear ATTRIBUTE_PRXPTR;
    CellSailSourceFuncRead pRead ATTRIBUTE_PRXPTR; CellSailSourceFuncReadSync pReadSync ATTRIBUTE_PRXPTR;
    CellSailSourceFuncGetCapabilities pGetCapabilities ATTRIBUTE_PRXPTR;
    CellSailSourceFuncInquireCapability pInquireCapability ATTRIBUTE_PRXPTR;
} CellSailSourceFuncs;
typedef struct { uint64_t internalData[20]; } CellSailSource;
int cellSailSourceInitialize(CellSailSource*,const CellSailSourceFuncs*,void*);
int cellSailSourceFinalize(CellSailSource*);
void cellSailSourceNotifyCallCompleted(CellSailSource*,int);
void cellSailSourceNotifyInputEos(CellSailSource*);
void cellSailSourceNotifyStreamOut(CellSailSource*,uint32_t);
void cellSailSourceNotifySessionError(CellSailSource*,int,bool);
void cellSailSourceNotifyMediaStateChanged(CellSailSource*,int);
enum { CELL_SAIL_SOURCE_DIAG_NOTIFY_CALL_COMPLETED=0, CELL_SAIL_SOURCE_DIAG_NOTIFY_INPUT_EOS=1, CELL_SAIL_SOURCE_DIAG_NOTIFY_STREAM_OUT=2, CELL_SAIL_SOURCE_DIAG_NOTIFY_SESSION_ERROR=3, CELL_SAIL_SOURCE_DIAG_NOTIFY_MEDIA_STATE_CHANGED=4 };
typedef void (*CellSailSourceFuncDiagNotify)(void*,int,int,int);
CellSailSourceFuncDiagNotify cellSailSourceSetDiagHandler(CellSailSource*,CellSailSourceFuncDiagNotify,void*);
#ifdef __cplusplus
}
#endif
#endif
