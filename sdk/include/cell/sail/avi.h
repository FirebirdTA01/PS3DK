/* cell/sail/avi.h — cellSail AVI stream support. Clean-room. */
#ifndef __PS3DK_CELL_SAIL_AVI_H__
#define __PS3DK_CELL_SAIL_AVI_H__
#include <stdint.h>
#include <cell/sail/common.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CELL_SAIL_AVI_FCC_auds CELL_SAIL_4CC('a','u','d','s')
#define CELL_SAIL_AVI_FCC_vids CELL_SAIL_4CC('v','i','d','s')
#define CELL_SAIL_AVI_FCC_NULL 0
typedef struct { uint64_t internalData[16]; } CellSailAviMovie;
typedef struct { uint64_t internalData[2]; } CellSailAviStream;
typedef struct { uint32_t maxBytesPerSec, flags, reserved0, streams, suggestedBufferSize, width, height, scale, rate, length, reserved1, reserved2; } CellSailAviMovieInfo;
typedef struct { uint32_t microSecPerFrame, maxBytesPerSec, padding, flags, totalFrames, initialFrames, streams, suggestedBufferSize, width, height, reserved[4]; } CellSailAviMainHeader;
typedef struct { uint32_t fccType, fccHandler, flags, priority, language, initialFrames, scale, rate, start, length, suggestedBufferSize, quality, sampleSize; int16_t left, top, right, bottom; } CellSailAviStreamHeader;
int cellSailAviMovieGetMovieInfo(CellSailAviMovie*,CellSailAviMovieInfo*);
int cellSailAviMovieGetStreamByIndex(CellSailAviMovie*,int,CellSailAviStream*);
int cellSailAviStreamGetHeader(CellSailAviStream*,CellSailAviStreamHeader*);
#ifdef __cplusplus
}
#endif
#endif
