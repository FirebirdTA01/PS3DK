/* cell/codec/gifdec.h — cellGifDec GIF decoder. Clean-room. 12 entries. PRXPTR on callbacks and cross-SPRX pointers. Handles as uint32_t. */
#ifndef __PS3DK_CELL_CODEC_GIFDEC_H__
#define __PS3DK_CELL_CODEC_GIFDEC_H__
#include <stdint.h>
#include <cell/error.h>
#ifdef __PPU__
#include <ppu-types.h>
typedef struct CellSpurs CellSpurs;
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define CELL_GIFDEC_ERROR_OPEN_FILE     CELL_ERROR_CAST(0x80611300)
#define CELL_GIFDEC_ERROR_STREAM_FORMAT CELL_ERROR_CAST(0x80611301)
#define CELL_GIFDEC_ERROR_SEQ           CELL_ERROR_CAST(0x80611302)
#define CELL_GIFDEC_ERROR_ARG           CELL_ERROR_CAST(0x80611303)
#define CELL_GIFDEC_ERROR_FATAL         CELL_ERROR_CAST(0x80611304)
#define CELL_GIFDEC_ERROR_SPU_UNSUPPORT CELL_ERROR_CAST(0x80611305)
#define CELL_GIFDEC_ERROR_SPU_ERROR     CELL_ERROR_CAST(0x80611306)
#define CELL_GIFDEC_ERROR_CB_PARAM      CELL_ERROR_CAST(0x80611307)
typedef enum { CELL_GIFDEC_FILE=0, CELL_GIFDEC_BUFFER=1 } CellGifDecStreamSrcSel;
typedef enum { CELL_GIFDEC_SPU_THREAD_DISABLE=0, CELL_GIFDEC_SPU_THREAD_ENABLE=1 } CellGifDecSpuThreadEna;
typedef void* (*CellGifDecCbControlMalloc)(uint32_t,void*);
typedef int32_t (*CellGifDecCbControlFree)(void*,void*);
typedef struct { CellGifDecSpuThreadEna spuThreadEnable; uint32_t ppuThreadPriority, spuThreadPriority; CellGifDecCbControlMalloc cbCtrlMallocFunc ATTRIBUTE_PRXPTR; void *cbCtrlMallocArg; CellGifDecCbControlFree cbCtrlFreeFunc ATTRIBUTE_PRXPTR; void *cbCtrlFreeArg; } CellGifDecThreadInParam;
typedef struct { uint32_t gifCodecVersion; } CellGifDecThreadOutParam;
typedef struct { CellSpurs *spurs ATTRIBUTE_PRXPTR; uint8_t priority[8]; uint32_t maxContention; } CellGifDecExtThreadInParam;
typedef struct { uint32_t reserved; } CellGifDecExtThreadOutParam;
typedef struct { CellGifDecStreamSrcSel srcSelect; const char *fileName; int64_t fileOffset; uint32_t fileSize; void *streamPtr ATTRIBUTE_PRXPTR; uint32_t streamSize; CellGifDecSpuThreadEna spuThreadEnable; } CellGifDecSrc;
typedef struct { uint32_t initSpaceAllocated; } CellGifDecOpnInfo;
typedef struct { uint32_t SWidth,SHeight,SGlobalColorTableFlag,SColorResolution,SSortFlag,SSizeOfGlobalColorTable,SBackGroundColor,SPixelAspectRatio; } CellGifDecInfo;
typedef enum { CELL_GIFDEC_CONTINUE=0, CELL_GIFDEC_STOP=1 } CellGifDecCommand;
typedef enum { CELL_GIFDEC_RGBA=10, CELL_GIFDEC_ARGB=20 } CellGifDecColorSpace;
typedef struct { volatile CellGifDecCommand *commandPtr; CellGifDecColorSpace colorSpace; uint8_t outputColorAlpha1,outputColorAlpha2,reserved[2]; } CellGifDecInParam;
typedef struct { uint64_t outputWidthByte; uint32_t outputWidth,outputHeight,outputComponents,outputBitDepth; CellGifDecColorSpace outputColorSpace; uint32_t useMemorySpace; } CellGifDecOutParam;
typedef enum { CELL_GIFDEC_RECORD_TYPE_IMAGE_DESC=1, CELL_GIFDEC_RECORD_TYPE_EXTENSION=2, CELL_GIFDEC_RECORD_TYPE_TERMINATE=3 } CellGifDecRecordType;
typedef struct { uint8_t label, *data ATTRIBUTE_PRXPTR; } CellGifDecExtension;
typedef enum { CELL_GIFDEC_DEC_STATUS_FINISH=0, CELL_GIFDEC_DEC_STATUS_STOP=1 } CellGifDecDecodeStatus;
typedef struct { CellGifDecRecordType recordType; CellGifDecExtension outExtension; CellGifDecDecodeStatus status; } CellGifDecDataOutInfo;
typedef struct { uint64_t outputBytesPerLine; } CellGifDecDataCtrlParam;
typedef uint32_t CellGifDecMainHandle, CellGifDecSubHandle;
int32_t cellGifDecCreate(CellGifDecMainHandle*,const CellGifDecThreadInParam*,CellGifDecThreadOutParam*);
int32_t cellGifDecExtCreate(CellGifDecMainHandle*,const CellGifDecThreadInParam*,CellGifDecThreadOutParam*,const CellGifDecExtThreadInParam*,CellGifDecExtThreadOutParam*);
int32_t cellGifDecOpen(CellGifDecMainHandle,CellGifDecSubHandle*,const CellGifDecSrc*,CellGifDecOpnInfo*);
int32_t cellGifDecReadHeader(CellGifDecMainHandle,CellGifDecSubHandle,CellGifDecInfo*);
int32_t cellGifDecSetParameter(CellGifDecMainHandle,CellGifDecSubHandle,const CellGifDecInParam*,CellGifDecOutParam*);
int32_t cellGifDecDecodeData(CellGifDecMainHandle,CellGifDecSubHandle,uint8_t*,const CellGifDecDataCtrlParam*,CellGifDecDataOutInfo*);
int32_t cellGifDecClose(CellGifDecMainHandle,CellGifDecSubHandle);
int32_t cellGifDecDestroy(CellGifDecMainHandle);
/* Ext variants */
typedef struct { void *strmPtr ATTRIBUTE_PRXPTR; uint32_t strmSize; } CellGifDecStrmParam;
typedef struct { uint32_t decodedStrmSize; } CellGifDecStrmInfo;
typedef int32_t (*CellGifDecCbControlStream)(CellGifDecStrmInfo*,CellGifDecStrmParam*,void*);
typedef struct { CellGifDecCbControlStream cbCtrlStrmFunc ATTRIBUTE_PRXPTR; void *cbCtrlStrmArg; } CellGifDecCbCtrlStrm;
typedef enum { CELL_GIFDEC_NO_INTERLACE=0, CELL_GIFDEC_INTERLACE=1 } CellGifDecInterlaceMode;
typedef struct { uint64_t outputFrameWidthByte; uint32_t outputFrameHeight; uint64_t outputStartXByte; uint32_t outputStartY; uint64_t outputWidthByte; uint32_t outputHeight,outputBitDepth,outputComponents,scanPassCount; void *outputImage ATTRIBUTE_PRXPTR; CellGifDecInterlaceMode interlaceFlag; } CellGifDecDispInfo;
typedef struct { void *nextOutputImage; } CellGifDecDispParam;
typedef int32_t (*CellGifDecCbControlDisp)(CellGifDecDispInfo*,CellGifDecDispParam*,void*);
typedef struct { CellGifDecCbControlDisp cbCtrlDispFunc ATTRIBUTE_PRXPTR; void *cbCtrlDispArg; } CellGifDecCbCtrlDisp;
typedef struct { uint64_t reserved; } CellGifDecExtInfo;
typedef enum { CELL_GIFDEC_LINE_MODE=1 } CellGifDecBufferMode;
typedef enum { CELL_GIFDEC_RECEIVE_EVENT=0, CELL_GIFDEC_TRYRECEIVE_EVENT=1 } CellGifDecSpuMode;
typedef struct { CellGifDecBufferMode bufferMode; uint32_t outputCounts; CellGifDecSpuMode spuMode; } CellGifDecExtInParam;
typedef struct { uint64_t outputWidthByte; uint32_t outputHeight; } CellGifDecExtOutParam;
int32_t cellGifDecExtOpen(CellGifDecMainHandle,CellGifDecSubHandle*,const CellGifDecSrc*,CellGifDecOpnInfo*,const CellGifDecCbCtrlStrm*);
int32_t cellGifDecExtReadHeader(CellGifDecMainHandle,CellGifDecSubHandle,CellGifDecInfo*,CellGifDecExtInfo*);
int32_t cellGifDecExtSetParameter(CellGifDecMainHandle,CellGifDecSubHandle,const CellGifDecInParam*,CellGifDecOutParam*,const CellGifDecExtInParam*,CellGifDecExtOutParam*);
int32_t cellGifDecExtDecodeData(CellGifDecMainHandle,CellGifDecSubHandle,uint8_t*,const CellGifDecDataCtrlParam*,CellGifDecDataOutInfo*,const CellGifDecCbCtrlDisp*,CellGifDecDispParam*);
#ifdef __cplusplus
}
#endif
#endif
