/*! \file cell/codec/jpgdec.h
 \brief cellJpgDec API - JPEG decoder, 12 exports.

  libJpgDec decodes JPEG streams into RGBA / RGB / ARGB / grayscale
  output buffers.  Common flow:

    cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC)
    cellJpgDecCreate(&main, &tin, &tout)
    cellJpgDecOpen(main, &sub, &src, &openinfo)
    cellJpgDecReadHeader(main, sub, &info)     -> imageWidth / Height / color space
    cellJpgDecSetParameter(main, sub, &ip, &op) -> pick output format
    cellJpgDecDecodeData(main, sub, rgba_out, &dcp, &dout)
    cellJpgDecClose(main, sub)
    cellJpgDecDestroy(main)
    cellSysmoduleUnloadModule(CELL_SYSMODULE_JPGDEC)

  Linked against libjpgdec_stub.a (built from the libjpgdec nidgen
  YAML).  Ext entry points (cellJpgDecExt*) are declared at the bottom
  for SPU-assisted decoding via a Spurs taskset.
*/

#ifndef __PS3DK_CELL_CODEC_JPGDEC_H__
#define __PS3DK_CELL_CODEC_JPGDEC_H__

#include <stdint.h>
#include <ppu-types.h>   /* ATTRIBUTE_PRXPTR */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Error codes - facility 0x061 (CODEC), status 0x1101..0x1109
 * ------------------------------------------------------------------ */

#define CELL_JPGDEC_ERROR_HEADER          0x80611101
#define CELL_JPGDEC_ERROR_STREAM_FORMAT   0x80611102
#define CELL_JPGDEC_ERROR_ARG             0x80611103
#define CELL_JPGDEC_ERROR_SEQ             0x80611104
#define CELL_JPGDEC_ERROR_BUSY            0x80611105
#define CELL_JPGDEC_ERROR_FATAL           0x80611106
#define CELL_JPGDEC_ERROR_OPEN_FILE       0x80611107
#define CELL_JPGDEC_ERROR_SPU_UNSUPPORT   0x80611108
#define CELL_JPGDEC_ERROR_CB_PARAM        0x80611109

/* ------------------------------------------------------------------ *
 * Opaque handles.  Declared as 32-bit EAs rather than native pointers:
 * the SPRX writes 4 bytes into *mainHandle / *subHandle at Create/Open,
 * so an 8-byte destination leaves the low 4 bytes undefined and the
 * handle the caller hands back to Open/ReadHeader/DecodeData ends up
 * with the real value in the wrong half.  Same pattern as pngdec.
 * ------------------------------------------------------------------ */

typedef uint32_t CellJpgDecMainHandle;
typedef uint32_t CellJpgDecSubHandle;

/* ------------------------------------------------------------------ *
 * Enumerations
 * ------------------------------------------------------------------ */

typedef enum {
    CELL_JPGDEC_FILE   = 0,
    CELL_JPGDEC_BUFFER = 1
} CellJpgDecStreamSrcSel;

typedef enum {
    CELL_JPGDEC_SPU_THREAD_DISABLE = 0,
    CELL_JPGDEC_SPU_THREAD_ENABLE  = 1
} CellJpgDecSpuThreadEna;

typedef enum {
    CELL_JPGDEC_QUALITY = 0,
    CELL_JPGDEC_FAST    = 5
} CellJpgDecMethod;

typedef enum {
    CELL_JPGDEC_TOP_TO_BOTTOM = 0,
    CELL_JPGDEC_BOTTOM_TO_TOP = 1
} CellJpgDecOutputMode;

typedef enum {
    CELL_JPG_UNKNOWN                 = 0,
    CELL_JPG_GRAYSCALE               = 1,
    CELL_JPG_RGB                     = 2,
    CELL_JPG_YCbCr                   = 3,
    CELL_JPG_RGBA                    = 10,
    CELL_JPG_UPSAMPLE_ONLY           = 11,
    CELL_JPG_ARGB                    = 20,
    CELL_JPG_GRAYSCALE_TO_ALPHA_RGBA = 40,
    CELL_JPG_GRAYSCALE_TO_ALPHA_ARGB = 41
} CellJpgDecColorSpace;

typedef enum {
    CELL_JPGDEC_CONTINUE = 0,
    CELL_JPGDEC_STOP     = 1
} CellJpgDecCommand;

typedef enum {
    CELL_JPGDEC_DEC_STATUS_FINISH = 0,
    CELL_JPGDEC_DEC_STATUS_STOP   = 1
} CellJpgDecDecodeStatus;

/* ------------------------------------------------------------------ *
 * Callbacks
 * ------------------------------------------------------------------ */

typedef void *(*CellJpgDecCbControlMalloc)(uint32_t size, void *cbCtrlMallocArg);
typedef int32_t (*CellJpgDecCbControlFree)(void *ptr, void *cbCtrlFreeArg);

/* ------------------------------------------------------------------ *
 * Structs
 * ------------------------------------------------------------------ */

/* Pointer fields carry ATTRIBUTE_PRXPTR so GCC emits them as 32-bit
 * EAs in this struct's layout, matching the reference-SDK ABI that the
 * sys_io SPRX reads.  Without this the 8-byte natural pointers on
 * PPU64 shift downstream fields and the SPRX reads garbage. */
typedef struct CellJpgDecThreadInParam {
    CellJpgDecSpuThreadEna     spuThreadEnable;
    uint32_t                   ppuThreadPriority;
    uint32_t                   spuThreadPriority;
    CellJpgDecCbControlMalloc  cbCtrlMallocFunc ATTRIBUTE_PRXPTR;
    void                      *cbCtrlMallocArg  ATTRIBUTE_PRXPTR;
    CellJpgDecCbControlFree    cbCtrlFreeFunc   ATTRIBUTE_PRXPTR;
    void                      *cbCtrlFreeArg    ATTRIBUTE_PRXPTR;
} CellJpgDecThreadInParam;

typedef struct CellJpgDecThreadOutParam {
    uint32_t                   jpegCodecVersion;
} CellJpgDecThreadOutParam;

typedef struct CellJpgDecSrc {
    CellJpgDecStreamSrcSel     srcSelect;
    const char                *fileName ATTRIBUTE_PRXPTR;
    int64_t                    fileOffset;
    uint32_t                   fileSize;
    void                      *streamPtr ATTRIBUTE_PRXPTR;
    uint32_t                   streamSize;
    CellJpgDecSpuThreadEna     spuThreadEnable;
} CellJpgDecSrc;

typedef struct CellJpgDecOpnInfo {
    uint32_t                   initSpaceAllocated;
} CellJpgDecOpnInfo;

typedef struct CellJpgDecInfo {
    uint32_t                   imageWidth;
    uint32_t                   imageHeight;
    uint32_t                   numComponents;
    CellJpgDecColorSpace       jpegColorSpace;
} CellJpgDecInfo;

typedef struct CellJpgDecInParam {
    volatile CellJpgDecCommand *commandPtr ATTRIBUTE_PRXPTR;
    uint32_t                   downScale;
    CellJpgDecMethod           method;
    CellJpgDecOutputMode       outputMode;
    CellJpgDecColorSpace       outputColorSpace;
    uint8_t                    outputColorAlpha;
    uint8_t                    reserved[3];
} CellJpgDecInParam;

typedef struct CellJpgDecOutParam {
    uint64_t                   outputWidthByte;
    uint32_t                   outputWidth;
    uint32_t                   outputHeight;
    uint32_t                   outputComponents;
    CellJpgDecOutputMode       outputMode;
    CellJpgDecColorSpace       outputColorSpace;
    uint32_t                   downScale;
    uint32_t                   useMemorySpace;
} CellJpgDecOutParam;

typedef struct CellJpgDecDataOutInfo {
    float                      mean;
    uint32_t                   outputLines;
    CellJpgDecDecodeStatus     status;
} CellJpgDecDataOutInfo;

typedef struct CellJpgDecDataCtrlParam {
    uint64_t                   outputBytesPerLine;
} CellJpgDecDataCtrlParam;

/* ------------------------------------------------------------------ *
 * Base API (7 exports).
 * ------------------------------------------------------------------ */

int32_t cellJpgDecCreate(CellJpgDecMainHandle *mainHandle,
                         const CellJpgDecThreadInParam *threadInParam,
                         CellJpgDecThreadOutParam *threadOutParam);

int32_t cellJpgDecDestroy(CellJpgDecMainHandle mainHandle);

int32_t cellJpgDecOpen(CellJpgDecMainHandle mainHandle,
                       CellJpgDecSubHandle *subHandle,
                       const CellJpgDecSrc *src,
                       CellJpgDecOpnInfo *openInfo);

int32_t cellJpgDecClose(CellJpgDecMainHandle mainHandle,
                        CellJpgDecSubHandle subHandle);

int32_t cellJpgDecReadHeader(CellJpgDecMainHandle mainHandle,
                             CellJpgDecSubHandle subHandle,
                             CellJpgDecInfo *info);

int32_t cellJpgDecSetParameter(CellJpgDecMainHandle mainHandle,
                               CellJpgDecSubHandle subHandle,
                               const CellJpgDecInParam *inParam,
                               CellJpgDecOutParam *outParam);

int32_t cellJpgDecDecodeData(CellJpgDecMainHandle mainHandle,
                             CellJpgDecSubHandle subHandle,
                             uint8_t *data,
                             const CellJpgDecDataCtrlParam *dataCtrlParam,
                             CellJpgDecDataOutInfo *dataOutInfo);

/* ------------------------------------------------------------------ *
 * Ext API (5 exports) - SPU-assisted decoding via Spurs taskset.
 * Declared here so callers can extern these prototypes when they need
 * them; we don't pull in <cell/spurs.h> to avoid the dependency for
 * the common case.
 * ------------------------------------------------------------------ */

struct CellSpurs;

typedef enum {
    CELL_JPGDEC_MCU_MODE  = 0,
    CELL_JPGDEC_LINE_MODE = 1
} CellJpgDecBufferMode;

typedef enum {
    CELL_JPGDEC_RECEIVE_EVENT    = 0,
    CELL_JPGDEC_TRYRECEIVE_EVENT = 1
} CellJpgDecSpuMode;

typedef struct CellJpgDecExtThreadInParam {
    struct CellSpurs          *spurs;
    uint8_t                    priority[8];
    uint32_t                   maxContention;
} CellJpgDecExtThreadInParam;

typedef struct CellJpgDecExtThreadOutParam {
    uint32_t                   reserved;
} CellJpgDecExtThreadOutParam;

typedef struct CellJpgDecExtInfo {
    uint64_t                   coefBufferSize;
    uint32_t                   mcuWidth;
} CellJpgDecExtInfo;

typedef struct CellJpgDecExtInParam {
    void                      *coefBufferPtr;
    CellJpgDecBufferMode       bufferMode;
    uint32_t                   outputCounts;
    CellJpgDecSpuMode          spuMode;
} CellJpgDecExtInParam;

typedef struct CellJpgDecExtOutParam {
    uint64_t                   outputWidthByte;
    uint32_t                   outputHeight;
    uint32_t                   oneMcuWidth;
    uint32_t                   oneMcuHeight;
} CellJpgDecExtOutParam;

typedef struct CellJpgDecStrmInfo  { uint32_t decodedStrmSize; } CellJpgDecStrmInfo;
typedef struct CellJpgDecStrmParam { void *strmPtr; uint32_t strmSize; } CellJpgDecStrmParam;

typedef struct CellJpgDecDispInfo {
    uint64_t outputFrameWidthByte;
    uint32_t outputFrameHeight;
    uint64_t outputStartXByte;
    uint32_t outputStartY;
    uint64_t outputWidthByte;
    uint32_t outputHeight;
    uint32_t outputComponents;
    void    *outputImage;
} CellJpgDecDispInfo;

typedef struct CellJpgDecDispParam { void *nextOutputImage; } CellJpgDecDispParam;

typedef int32_t (*CellJpgDecCbControlStream)(CellJpgDecStrmInfo *strmInfo,
                                             CellJpgDecStrmParam *strmParam,
                                             void *cbCtrlStrmArg);
typedef int32_t (*CellJpgDecCbControlDisp)(CellJpgDecDispInfo *dispInfo,
                                           CellJpgDecDispParam *dispParam,
                                           void *cbCtrlDispArg);

typedef struct CellJpgDecCbCtrlStrm {
    CellJpgDecCbControlStream cbCtrlStrmFunc;
    void                     *cbCtrlStrmArg;
} CellJpgDecCbCtrlStrm;

typedef struct CellJpgDecCbCtrlDisp {
    CellJpgDecCbControlDisp cbCtrlDispFunc;
    void                   *cbCtrlDispArg;
} CellJpgDecCbCtrlDisp;

int32_t cellJpgDecExtCreate(CellJpgDecMainHandle *mainHandle,
                            const CellJpgDecThreadInParam *threadInParam,
                            CellJpgDecThreadOutParam *threadOutParam,
                            const CellJpgDecExtThreadInParam *extThreadInParam,
                            CellJpgDecExtThreadOutParam *extThreadOutParam);

int32_t cellJpgDecExtOpen(CellJpgDecMainHandle mainHandle,
                          CellJpgDecSubHandle *subHandle,
                          const CellJpgDecSrc *src,
                          CellJpgDecOpnInfo *openInfo,
                          const CellJpgDecCbCtrlStrm *cbCtrlStrm);

int32_t cellJpgDecExtReadHeader(CellJpgDecMainHandle mainHandle,
                                CellJpgDecSubHandle subHandle,
                                CellJpgDecInfo *info,
                                CellJpgDecExtInfo *extInfo);

int32_t cellJpgDecExtSetParameter(CellJpgDecMainHandle mainHandle,
                                  CellJpgDecSubHandle subHandle,
                                  const CellJpgDecInParam *inParam,
                                  CellJpgDecOutParam *outParam,
                                  const CellJpgDecExtInParam *extInParam,
                                  CellJpgDecExtOutParam *extOutParam);

int32_t cellJpgDecExtDecodeData(CellJpgDecMainHandle mainHandle,
                                CellJpgDecSubHandle subHandle,
                                uint8_t *data,
                                const CellJpgDecDataCtrlParam *dataCtrlParam,
                                CellJpgDecDataOutInfo *dataOutInfo,
                                const CellJpgDecCbCtrlDisp *cbCtrlDisp,
                                CellJpgDecDispParam *dispParam);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CODEC_JPGDEC_H__ */
