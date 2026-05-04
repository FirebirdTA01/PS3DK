/* cell/codec/pngenc.h — cellPngEnc PNG encoder. Clean-room. 9 entries.
   size_t → uint32_t in caller structs, PRXPTR on all cross-SPRX pointers. */
#ifndef __PS3DK_CELL_CODEC_PNGENC_H__
#define __PS3DK_CELL_CODEC_PNGENC_H__
#include <stdint.h>
#include <stdbool.h>
#include <cell/error.h>
#include <cell/codec/pngcom.h>
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
#define CELL_PNGENC_ERROR_ARG             CELL_ERROR_CAST(0x80611291)
#define CELL_PNGENC_ERROR_SEQ             CELL_ERROR_CAST(0x80611292)
#define CELL_PNGENC_ERROR_BUSY            CELL_ERROR_CAST(0x80611293)
#define CELL_PNGENC_ERROR_EMPTY           CELL_ERROR_CAST(0x80611294)
#define CELL_PNGENC_ERROR_RESET           CELL_ERROR_CAST(0x80611295)
#define CELL_PNGENC_ERROR_FATAL           CELL_ERROR_CAST(0x80611296)
#define CELL_PNGENC_ERROR_STREAM_ABORT    CELL_ERROR_CAST(0x806112A1)
#define CELL_PNGENC_ERROR_STREAM_SKIP     CELL_ERROR_CAST(0x806112A2)
#define CELL_PNGENC_ERROR_STREAM_OVERFLOW CELL_ERROR_CAST(0x806112A3)
#define CELL_PNGENC_ERROR_STREAM_FILE_OPEN CELL_ERROR_CAST(0x806112A4)
typedef enum { CELL_PNGENC_COLOR_SPACE_GRAYSCALE=1, CELL_PNGENC_COLOR_SPACE_RGB=2, CELL_PNGENC_COLOR_SPACE_PALETTE=4, CELL_PNGENC_COLOR_SPACE_GRAYSCALE_ALPHA=9, CELL_PNGENC_COLOR_SPACE_RGBA=10, CELL_PNGENC_COLOR_SPACE_ARGB=20 } CellPngEncColorSpace;
typedef enum { CELL_PNGENC_COMPR_LEVEL_0, CELL_PNGENC_COMPR_LEVEL_1, CELL_PNGENC_COMPR_LEVEL_2, CELL_PNGENC_COMPR_LEVEL_3, CELL_PNGENC_COMPR_LEVEL_4, CELL_PNGENC_COMPR_LEVEL_5, CELL_PNGENC_COMPR_LEVEL_6, CELL_PNGENC_COMPR_LEVEL_7, CELL_PNGENC_COMPR_LEVEL_8, CELL_PNGENC_COMPR_LEVEL_9 } CellPngEncCompressionLevel;
typedef enum { CELL_PNGENC_FILTER_TYPE_NONE=0x08, CELL_PNGENC_FILTER_TYPE_SUB=0x10, CELL_PNGENC_FILTER_TYPE_UP=0x20, CELL_PNGENC_FILTER_TYPE_AVG=0x40, CELL_PNGENC_FILTER_TYPE_PAETH=0x80, CELL_PNGENC_FILTER_TYPE_ALL=0xF8 } CellPngEncFilterType;
typedef enum { CELL_PNGENC_CHUNK_TYPE_PLTE, CELL_PNGENC_CHUNK_TYPE_TRNS, CELL_PNGENC_CHUNK_TYPE_CHRM, CELL_PNGENC_CHUNK_TYPE_GAMA, CELL_PNGENC_CHUNK_TYPE_ICCP, CELL_PNGENC_CHUNK_TYPE_SBIT, CELL_PNGENC_CHUNK_TYPE_SRGB, CELL_PNGENC_CHUNK_TYPE_TEXT, CELL_PNGENC_CHUNK_TYPE_BKGD, CELL_PNGENC_CHUNK_TYPE_HIST, CELL_PNGENC_CHUNK_TYPE_PHYS, CELL_PNGENC_CHUNK_TYPE_SPLT, CELL_PNGENC_CHUNK_TYPE_TIME, CELL_PNGENC_CHUNK_TYPE_OFFS, CELL_PNGENC_CHUNK_TYPE_PCAL, CELL_PNGENC_CHUNK_TYPE_SCAL, CELL_PNGENC_CHUNK_TYPE_UNKNOWN } CellPngEncChunkType;
typedef enum { CELL_PNGENC_LOCATION_FILE, CELL_PNGENC_LOCATION_BUFFER } CellPngEncLocation;
typedef uint32_t CellPngEncHandle;
typedef struct { int32_t index; intptr_t value; } CellPngEncExParam;
typedef struct { uint32_t maxWidth, maxHeight, maxBitDepth; bool enableSpu; uint32_t addMemSize; CellPngEncExParam *exParamList ATTRIBUTE_PRXPTR; uint32_t exParamNum; } CellPngEncConfig;
typedef struct { uint32_t memSize; uint8_t cmdQueueDepth; uint32_t versionUpper, versionLower; } CellPngEncAttr;
typedef struct { void *memAddr ATTRIBUTE_PRXPTR; uint32_t memSize; int32_t ppuThreadPriority, spuThreadPriority; } CellPngEncResource;
typedef struct { void *memAddr ATTRIBUTE_PRXPTR; uint32_t memSize; int32_t ppuThreadPriority; CellSpurs *spurs ATTRIBUTE_PRXPTR; uint8_t priority[8]; } CellPngEncResourceEx;
typedef struct { uint32_t width, height, pitchWidth; CellPngEncColorSpace colorSpace; uint32_t bitDepth; bool packedPixel; void *pictureAddr ATTRIBUTE_PRXPTR; uint64_t userData; } CellPngEncPicture;
typedef struct { CellPngEncChunkType chunkType; void *chunkData ATTRIBUTE_PRXPTR; } CellPngEncAncillaryChunk;
typedef struct { bool enableSpu; CellPngEncColorSpace encodeColorSpace; CellPngEncCompressionLevel compressionLevel; uint32_t filterType; CellPngEncAncillaryChunk *ancillaryChunkList ATTRIBUTE_PRXPTR; uint32_t ancillaryChunkNum; } CellPngEncEncodeParam;
typedef struct { CellPngEncLocation location; const char *streamFileName; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize; } CellPngEncOutputParam;
typedef struct { int32_t state; CellPngEncLocation location; const char *streamFileName; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize, streamSize; uint32_t processedLine; uint64_t userData; } CellPngEncStreamInfo;
int32_t cellPngEncQueryAttr(const CellPngEncConfig*,CellPngEncAttr*);
int32_t cellPngEncOpen(const CellPngEncConfig*,const CellPngEncResource*,CellPngEncHandle*);
int32_t cellPngEncOpenEx(const CellPngEncConfig*,const CellPngEncResourceEx*,CellPngEncHandle*);
int32_t cellPngEncWaitForInput(CellPngEncHandle,bool);
int32_t cellPngEncEncodePicture(CellPngEncHandle,const CellPngEncPicture*,const CellPngEncEncodeParam*,const CellPngEncOutputParam*);
int32_t cellPngEncWaitForOutput(CellPngEncHandle,uint32_t*,bool);
int32_t cellPngEncGetStreamInfo(CellPngEncHandle,CellPngEncStreamInfo*);
int32_t cellPngEncReset(CellPngEncHandle);
int32_t cellPngEncClose(CellPngEncHandle);
#ifdef __cplusplus
}
#endif
#endif
