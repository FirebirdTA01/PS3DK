/* cell/sail/avi.h — cellSail AVI stream support. Independent. */
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
typedef struct { uint32_t microSecPerFrame, maxBytesPerSec, paddingGranularity, flags, totalFrames, initialFrames, streams, suggestedBufferSize, width, height, reserved[4]; } CellSailAviMainHeader;
typedef struct CellSailAviStreamHeader {
    uint32_t fccType;
    uint32_t fccHandler;
    uint32_t flags;
    uint16_t priority;
    uint16_t language;
    uint32_t initialFrames;
    uint32_t scale;
    uint32_t rate;
    uint32_t start;
    uint32_t length;
    uint32_t suggestedBufferSize;
    uint32_t quality;
    uint32_t sampleSize;
    union {
        struct {
            int16_t left;
            int16_t top;
            int16_t right;
            int16_t bottom;
        } frame;
        struct {
            int16_t left;
            int16_t top;
            int16_t right;
            int16_t bottom;
        };
    };
} CellSailAviStreamHeader;
int cellSailAviMovieGetMovieInfo(CellSailAviMovie*,CellSailAviMovieInfo*);
int cellSailAviMovieGetStreamByIndex(CellSailAviMovie*,int,CellSailAviStream*);
int cellSailAviStreamGetHeader(CellSailAviStream*,CellSailAviStreamHeader*);
#ifdef __cplusplus
}
#endif
#endif
