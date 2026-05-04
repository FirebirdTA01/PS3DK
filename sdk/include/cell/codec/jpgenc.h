/* cell/codec/jpgenc.h — cellJpgEnc JPEG encoder. Clean-room. 10 entries. size_t→uint32_t, PRXPTR on cross-SPRX pointers. */
#ifndef __PS3DK_CELL_CODEC_JPGENC_H__
#define __PS3DK_CELL_CODEC_JPGENC_H__
#include <stdint.h>
#include <stdbool.h>
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
#define CELL_JPGENC_ERROR_ARG              CELL_ERROR_CAST(0x80611191)
#define CELL_JPGENC_ERROR_SEQ              CELL_ERROR_CAST(0x80611192)
#define CELL_JPGENC_ERROR_BUSY             CELL_ERROR_CAST(0x80611193)
#define CELL_JPGENC_ERROR_EMPTY            CELL_ERROR_CAST(0x80611194)
#define CELL_JPGENC_ERROR_RESET            CELL_ERROR_CAST(0x80611195)
#define CELL_JPGENC_ERROR_FATAL            CELL_ERROR_CAST(0x80611196)
#define CELL_JPGENC_ERROR_STREAM_ABORT     CELL_ERROR_CAST(0x806111A1)
#define CELL_JPGENC_ERROR_STREAM_SKIP      CELL_ERROR_CAST(0x806111A2)
#define CELL_JPGENC_ERROR_STREAM_OVERFLOW  CELL_ERROR_CAST(0x806111A3)
#define CELL_JPGENC_ERROR_STREAM_FILE_OPEN CELL_ERROR_CAST(0x806111A4)
typedef enum { CELL_JPGENC_COLOR_SPACE_GRAYSCALE=1, CELL_JPGENC_COLOR_SPACE_RGB=2, CELL_JPGENC_COLOR_SPACE_YCbCr=3, CELL_JPGENC_COLOR_SPACE_RGBA=10, CELL_JPGENC_COLOR_SPACE_ARGB=20 } CellJpgEncColorSpace;
typedef enum { CELL_JPGENC_SAMPLING_FMT_YCbCr444, CELL_JPGENC_SAMPLING_FMT_YCbCr422, CELL_JPGENC_SAMPLING_FMT_YCbCr420, CELL_JPGENC_SAMPLING_FMT_YCbCr411, CELL_JPGENC_SAMPLING_FMT_FULL } CellJpgEncSamplingFormat;
typedef enum { CELL_JPGENC_DCT_METHOD_QUALITY=0, CELL_JPGENC_DCT_METHOD_FAST=5 } CellJpgEncDctMethod;
typedef enum { CELL_JPGENC_COMPR_MODE_CONSTANT_QUALITY, CELL_JPGENC_COMPR_MODE_STREAM_SIZE_LIMIT } CellJpgEncCompressionMode;
typedef enum { CELL_JPGENC_LOCATION_FILE, CELL_JPGENC_LOCATION_BUFFER } CellJpgEncLocation;
typedef uint32_t CellJpgEncHandle;
typedef struct { int32_t index; intptr_t value; } CellJpgEncExParam;
typedef struct { uint32_t maxWidth, maxHeight; CellJpgEncColorSpace encodeColorSpace; CellJpgEncSamplingFormat encodeSamplingFormat; bool enableSpu; CellJpgEncExParam *exParamList ATTRIBUTE_PRXPTR; uint32_t exParamNum; } CellJpgEncConfig;
typedef struct { uint32_t memSize; uint8_t cmdQueueDepth; uint32_t versionUpper, versionLower; } CellJpgEncAttr;
typedef struct { void *memAddr ATTRIBUTE_PRXPTR; uint32_t memSize; int32_t ppuThreadPriority, spuThreadPriority; } CellJpgEncResource;
typedef struct { void *memAddr ATTRIBUTE_PRXPTR; uint32_t memSize; int32_t ppuThreadPriority; CellSpurs *spurs ATTRIBUTE_PRXPTR; uint8_t priority[8]; } CellJpgEncResourceEx;
typedef struct { uint32_t width, height; CellJpgEncColorSpace colorSpace; void *pictureAddr ATTRIBUTE_PRXPTR; uint64_t userData; } CellJpgEncPicture;
typedef struct { uint32_t width, height, pitchWidth; CellJpgEncColorSpace colorSpace; void *pictureAddr ATTRIBUTE_PRXPTR; uint64_t userData; } CellJpgEncPicture2;
typedef struct { uint32_t markerCode; uint8_t *markerSegmentData ATTRIBUTE_PRXPTR; uint32_t markerSegmentDataLength; } CellJpgEncMarkerSegment;
typedef struct { bool enableSpu; uint32_t restartInterval; CellJpgEncDctMethod dctMethod; CellJpgEncCompressionMode compressionMode; uint32_t quality; CellJpgEncMarkerSegment *markerSegmentList ATTRIBUTE_PRXPTR; uint32_t markerSegmentNum; } CellJpgEncEncodeParam;
typedef struct { CellJpgEncLocation location; const char *streamFileName; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize; } CellJpgEncOutputParam;
typedef struct { int32_t state; CellJpgEncLocation location; const char *streamFileName; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize, streamSize; uint32_t processedLine, quality; uint64_t userData; } CellJpgEncStreamInfo;
int32_t cellJpgEncQueryAttr(const CellJpgEncConfig*,CellJpgEncAttr*);
int32_t cellJpgEncOpen(const CellJpgEncConfig*,const CellJpgEncResource*,CellJpgEncHandle*);
int32_t cellJpgEncOpenEx(const CellJpgEncConfig*,const CellJpgEncResourceEx*,CellJpgEncHandle*);
int32_t cellJpgEncWaitForInput(CellJpgEncHandle,bool);
int32_t cellJpgEncEncodePicture(CellJpgEncHandle,const CellJpgEncPicture*,const CellJpgEncEncodeParam*,const CellJpgEncOutputParam*);
int32_t cellJpgEncEncodePicture2(CellJpgEncHandle,const CellJpgEncPicture2*,const CellJpgEncEncodeParam*,const CellJpgEncOutputParam*);
int32_t cellJpgEncWaitForOutput(CellJpgEncHandle,uint32_t*,bool);
int32_t cellJpgEncGetStreamInfo(CellJpgEncHandle,CellJpgEncStreamInfo*);
int32_t cellJpgEncReset(CellJpgEncHandle);
int32_t cellJpgEncClose(CellJpgEncHandle);
#ifdef __cplusplus
}
#endif
#endif
