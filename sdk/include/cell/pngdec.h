/*! \file cell/pngdec.h
 \brief cellPngDec API - PNG decoder, 29 exports.

  libPngDec decodes PNG streams into RGBA/RGB/grayscale output buffers.
  The common flow is:

    cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC)  // pull SPRX into process
    cellPngDecCreate(&main, &tin, &tout)            // create decoder
    cellPngDecOpen(main, &sub, &src, &openinfo)     // bind a stream
    cellPngDecReadHeader(main, sub, &info)          // fill imageWidth/Height
    cellPngDecSetParameter(main, sub, &ip, &op)     // pick output color space
    cellPngDecDecodeData(main, sub, rgba_out, &dcp, &dout)
    cellPngDecClose(main, sub)
    cellPngDecDestroy(main)
    cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC)

  Linked against libpngdec_stub.a (built from the libpngdec nidgen YAML).
  All 29 exports are declared below; ancillary-chunk getters
  (cellPngDecGetPLTE, cellPngDecGetgAMA, etc.) are listed at the bottom
  as extern declarations that consumers can call when they need the
  PNG metadata chunks.
*/

#ifndef __PS3DK_CELL_PNGDEC_H__
#define __PS3DK_CELL_PNGDEC_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Error codes  -  facility 0x061 (CODEC), status 0x1201..0x120a
 * ------------------------------------------------------------------ */

#define CELL_PNGDEC_ERROR_HEADER          0x80611201
#define CELL_PNGDEC_ERROR_STREAM_FORMAT   0x80611202
#define CELL_PNGDEC_ERROR_ARG             0x80611203
#define CELL_PNGDEC_ERROR_SEQ             0x80611204
#define CELL_PNGDEC_ERROR_BUSY            0x80611205
#define CELL_PNGDEC_ERROR_FATAL           0x80611206
#define CELL_PNGDEC_ERROR_OPEN_FILE       0x80611207
#define CELL_PNGDEC_ERROR_SPU_UNSUPPORT   0x80611208
#define CELL_PNGDEC_ERROR_SPU_ERROR       0x80611209
#define CELL_PNGDEC_ERROR_CB_PARAM        0x8061120a

/* ------------------------------------------------------------------ *
 * Opaque handles.
 *
 * Declared as 32-bit EAs rather than native pointers.  The SPRX writes
 * 4 bytes into *mainHandle / *subHandle at Create/Open, so a 64-bit
 * destination leaves the low 4 bytes undefined and the handle the
 * caller hands back to Open/ReadHeader/DecodeData ends up with the
 * real value shifted into the upper 32 bits.  Symptom when this is
 * wrong: cellPngDecOpen returns CELL_PNGDEC_ERROR_ARG (0x80611203).
 * ------------------------------------------------------------------ */

typedef uint32_t CellPngDecMainHandle;
typedef uint32_t CellPngDecSubHandle;

/* ------------------------------------------------------------------ *
 * Enums
 * ------------------------------------------------------------------ */

typedef enum {
    CELL_PNGDEC_FILE    = 0,
    CELL_PNGDEC_BUFFER  = 1
} CellPngDecStreamSrcSel;

typedef enum {
    CELL_PNGDEC_SPU_THREAD_DISABLE = 0,
    CELL_PNGDEC_SPU_THREAD_ENABLE  = 1
} CellPngDecSpuThreadEna;

typedef enum {
    CELL_PNGDEC_TOP_TO_BOTTOM = 0,
    CELL_PNGDEC_BOTTOM_TO_TOP = 1
} CellPngDecOutputMode;

typedef enum {
    CELL_PNGDEC_GRAYSCALE       = 1,
    CELL_PNGDEC_RGB             = 2,
    CELL_PNGDEC_PALETTE         = 4,
    CELL_PNGDEC_GRAYSCALE_ALPHA = 9,
    CELL_PNGDEC_RGBA            = 10,
    CELL_PNGDEC_ARGB            = 20
} CellPngDecColorSpace;

typedef enum {
    CELL_PNGDEC_NO_INTERLACE    = 0,
    CELL_PNGDEC_ADAM7_INTERLACE = 1
} CellPngDecInterlaceMode;

typedef enum {
    CELL_PNGDEC_1BYTE_PER_NPIXEL = 0,
    CELL_PNGDEC_1BYTE_PER_1PIXEL = 1
} CellPngDecPackFlag;

typedef enum {
    CELL_PNGDEC_STREAM_ALPHA = 0,
    CELL_PNGDEC_FIX_ALPHA    = 1
} CellPngDecAlphaSelect;

typedef enum {
    CELL_PNGDEC_CONTINUE = 0,
    CELL_PNGDEC_STOP     = 1
} CellPngDecCommand;

typedef enum {
    CELL_PNGDEC_DEC_STATUS_FINISH = 0,
    CELL_PNGDEC_DEC_STATUS_STOP   = 1
} CellPngDecDecodeStatus;

/* ------------------------------------------------------------------ *
 * Callback signatures  (memory allocator injected into the decoder)
 * ------------------------------------------------------------------ */

typedef void   *(*CellPngDecCbControlMalloc)(uint32_t size, void *arg);
typedef int32_t (*CellPngDecCbControlFree)(void *ptr, void *arg);

/* ------------------------------------------------------------------ *
 * Structs - core Create/Open/ReadHeader/SetParameter/DecodeData path
 * ------------------------------------------------------------------ */

/* Callback-carrying struct fields on the PS3 HLE ABI are 32-bit EAs,
 * not native 64-bit pointers.  Sony's PS3 GCC emitted function pointers
 * as mode(SI); our GCC defaults to mode(DI), so if we type these fields
 * as raw function pointers the struct grows from 28 to 48 bytes and
 * the SPRX reads garbage, returning CELL_PNGDEC_ERROR_ARG.  We store
 * EAs as uint32_t and expose a convenience macro to convert from a
 * native function pointer (which, for our toolchain's compact OPDs,
 * IS the 32-bit descriptor EA already).
 *
 * Use CELL_PNGDEC_CB_EA(fn) to build a field value from a native
 * callback pointer. */
#define CELL_PNGDEC_CB_EA(fn)  ((uint32_t)(uintptr_t)(fn))

typedef struct CellPngDecThreadInParam {
    CellPngDecSpuThreadEna   spuThreadEnable;
    uint32_t                 ppuThreadPriority;
    uint32_t                 spuThreadPriority;
    uint32_t                 cbCtrlMallocFunc;   /* EA of CellPngDecCbControlMalloc */
    uint32_t                 cbCtrlMallocArg;    /* EA of callback-private context */
    uint32_t                 cbCtrlFreeFunc;     /* EA of CellPngDecCbControlFree */
    uint32_t                 cbCtrlFreeArg;
} CellPngDecThreadInParam;

typedef struct CellPngDecThreadOutParam {
    uint32_t pngCodecVersion;
} CellPngDecThreadOutParam;

typedef struct CellPngDecSrc {
    CellPngDecStreamSrcSel   srcSelect;
    uint32_t                 fileName;          /* EA of const char* path */
    int64_t                  fileOffset;
    uint32_t                 fileSize;
    uint32_t                 streamPtr;         /* EA of stream buffer */
    uint32_t                 streamSize;
    CellPngDecSpuThreadEna   spuThreadEnable;
} CellPngDecSrc;

typedef struct CellPngDecOpnInfo {
    uint32_t initSpaceAllocated;
} CellPngDecOpnInfo;

typedef struct CellPngDecInfo {
    uint32_t                imageWidth;
    uint32_t                imageHeight;
    uint32_t                numComponents;
    CellPngDecColorSpace    colorSpace;
    uint32_t                bitDepth;
    CellPngDecInterlaceMode interlaceMethod;
    uint32_t                chunkInformation;
} CellPngDecInfo;

typedef struct CellPngDecInParam {
    uint32_t                  commandPtr;         /* EA of volatile CellPngDecCommand* */
    CellPngDecOutputMode      outputMode;
    CellPngDecColorSpace      outputColorSpace;
    uint32_t                  outputBitDepth;
    CellPngDecPackFlag        outputPackFlag;
    CellPngDecAlphaSelect     outputAlphaSelect;
    uint32_t                  outputColorAlpha;
} CellPngDecInParam;

typedef struct CellPngDecOutParam {
    uint64_t              outputWidthByte;
    uint32_t              outputWidth;
    uint32_t              outputHeight;
    uint32_t              outputComponents;
    uint32_t              outputBitDepth;
    CellPngDecOutputMode  outputMode;
    CellPngDecColorSpace  outputColorSpace;
    uint32_t              useMemorySpace;
} CellPngDecOutParam;

typedef struct CellPngDecDataCtrlParam {
    uint64_t outputBytesPerLine;
} CellPngDecDataCtrlParam;

typedef struct CellPngDecDataOutInfo {
    uint32_t               chunkInformation;
    uint32_t               numText;
    uint32_t               numUnknownChunk;
    CellPngDecDecodeStatus status;
} CellPngDecDataOutInfo;

/* ------------------------------------------------------------------ *
 * Ext variants - SPU-backed pipelines with streaming + display callbacks
 * ------------------------------------------------------------------ */

typedef enum {
    CELL_PNGDEC_LINE_MODE = 1
} CellPngDecBufferMode;

typedef enum {
    CELL_PNGDEC_RECEIVE_EVENT    = 0,
    CELL_PNGDEC_TRYRECEIVE_EVENT = 1
} CellPngDecSpuMode;

/* Forward from <cell/spurs.h>; declared as an opaque pointer here so
 * callers that don't need SPURS don't have to drag the spurs headers
 * in transitively. */
struct CellSpurs;

typedef struct CellPngDecExtThreadInParam {
    struct CellSpurs *spurs;
    uint8_t           priority[8];
    uint32_t          maxContention;
} CellPngDecExtThreadInParam;

typedef struct CellPngDecExtThreadOutParam {
    uint32_t reserved;
} CellPngDecExtThreadOutParam;

typedef struct CellPngDecStrmInfo {
    uint32_t decodedStrmSize;
} CellPngDecStrmInfo;

typedef struct CellPngDecStrmParam {
    void    *strmPtr;
    uint32_t strmSize;
} CellPngDecStrmParam;

typedef int32_t (*CellPngDecCbControlStream)(CellPngDecStrmInfo  *info,
                                             CellPngDecStrmParam *param,
                                             void                *arg);

typedef struct CellPngDecCbCtrlStrm {
    CellPngDecCbControlStream cbCtrlStrmFunc;
    void                     *cbCtrlStrmArg;
} CellPngDecCbCtrlStrm;

typedef struct CellPngDecOpnParam {
    uint32_t selectChunk;
} CellPngDecOpnParam;

typedef struct CellPngDecExtInfo {
    uint64_t reserved;
} CellPngDecExtInfo;

typedef struct CellPngDecExtInParam {
    CellPngDecBufferMode bufferMode;
    uint32_t             outputCounts;
    CellPngDecSpuMode    spuMode;
} CellPngDecExtInParam;

typedef struct CellPngDecExtOutParam {
    uint64_t outputWidthByte;
    uint32_t outputHeight;
} CellPngDecExtOutParam;

typedef struct CellPngDecDispInfo {
    uint64_t  outputFrameWidthByte;
    uint32_t  outputFrameHeight;
    uint64_t  outputStartXByte;
    uint32_t  outputStartY;
    uint64_t  outputWidthByte;
    uint32_t  outputHeight;
    uint32_t  outputBitDepth;
    uint32_t  outputComponents;
    uint32_t  nextOutputStartY;
    uint32_t  scanPassCount;
    void     *outputImage;
} CellPngDecDispInfo;

typedef struct CellPngDecDispParam {
    void *nextOutputImage;
} CellPngDecDispParam;

typedef int32_t (*CellPngDecCbControlDisp)(CellPngDecDispInfo  *info,
                                           CellPngDecDispParam *param,
                                           void                *arg);

typedef struct CellPngDecCbCtrlDisp {
    CellPngDecCbControlDisp cbCtrlDispFunc;
    void                   *cbCtrlDispArg;
} CellPngDecCbCtrlDisp;

/* ------------------------------------------------------------------ *
 * Core decode API
 * ------------------------------------------------------------------ */

int32_t cellPngDecCreate(CellPngDecMainHandle          *mainHandle,
                         const CellPngDecThreadInParam *threadInParam,
                         CellPngDecThreadOutParam      *threadOutParam);

int32_t cellPngDecDestroy(CellPngDecMainHandle mainHandle);

int32_t cellPngDecOpen(CellPngDecMainHandle  mainHandle,
                       CellPngDecSubHandle  *subHandle,
                       const CellPngDecSrc  *src,
                       CellPngDecOpnInfo    *openInfo);

int32_t cellPngDecClose(CellPngDecMainHandle mainHandle,
                        CellPngDecSubHandle  subHandle);

int32_t cellPngDecReadHeader(CellPngDecMainHandle mainHandle,
                             CellPngDecSubHandle  subHandle,
                             CellPngDecInfo       *info);

int32_t cellPngDecSetParameter(CellPngDecMainHandle     mainHandle,
                               CellPngDecSubHandle      subHandle,
                               const CellPngDecInParam *inParam,
                               CellPngDecOutParam      *outParam);

int32_t cellPngDecDecodeData(CellPngDecMainHandle           mainHandle,
                             CellPngDecSubHandle            subHandle,
                             uint8_t                       *data,
                             const CellPngDecDataCtrlParam *dataCtrlParam,
                             CellPngDecDataOutInfo         *dataOutInfo);

/* ------------------------------------------------------------------ *
 * Ext variants
 * ------------------------------------------------------------------ */

int32_t cellPngDecExtCreate(CellPngDecMainHandle             *mainHandle,
                            const CellPngDecThreadInParam    *threadInParam,
                            CellPngDecThreadOutParam         *threadOutParam,
                            const CellPngDecExtThreadInParam *extThreadInParam,
                            CellPngDecExtThreadOutParam      *extThreadOutParam);

int32_t cellPngDecExtOpen(CellPngDecMainHandle        mainHandle,
                          CellPngDecSubHandle        *subHandle,
                          const CellPngDecSrc        *src,
                          CellPngDecOpnInfo          *openInfo,
                          const CellPngDecCbCtrlStrm *cbCtrlStrm,
                          const CellPngDecOpnParam   *opnParam);

int32_t cellPngDecExtReadHeader(CellPngDecMainHandle  mainHandle,
                                CellPngDecSubHandle   subHandle,
                                CellPngDecInfo       *info,
                                CellPngDecExtInfo    *extInfo);

int32_t cellPngDecExtSetParameter(CellPngDecMainHandle        mainHandle,
                                  CellPngDecSubHandle         subHandle,
                                  const CellPngDecInParam    *inParam,
                                  CellPngDecOutParam         *outParam,
                                  const CellPngDecExtInParam *extInParam,
                                  CellPngDecExtOutParam      *extOutParam);

int32_t cellPngDecExtDecodeData(CellPngDecMainHandle           mainHandle,
                                CellPngDecSubHandle            subHandle,
                                uint8_t                       *data,
                                const CellPngDecDataCtrlParam *dataCtrlParam,
                                CellPngDecDataOutInfo         *dataOutInfo,
                                const CellPngDecCbCtrlDisp    *cbCtrlDisp,
                                CellPngDecDispParam           *dispParam);

/* ------------------------------------------------------------------ *
 * Ancillary PNG metadata chunks.
 *
 * Structures for these live in <cell/codec/pngcom.h> in the reference
 * SDK; we do not redeclare them here.  Callers that need a specific
 * chunk can extern-declare it and the libpngdec_stub.a resolves the
 * symbol at link time regardless of the struct declaration path.
 * ------------------------------------------------------------------ */

int32_t cellPngDecGetPLTE(CellPngDecMainHandle mainHandle,
                          CellPngDecSubHandle  subHandle,
                          void                *plte);
int32_t cellPngDecGetgAMA(CellPngDecMainHandle, CellPngDecSubHandle, void *gama);
int32_t cellPngDecGetsRGB(CellPngDecMainHandle, CellPngDecSubHandle, void *srgb);
int32_t cellPngDecGetiCCP(CellPngDecMainHandle, CellPngDecSubHandle, void *iccp);
int32_t cellPngDecGetsBIT(CellPngDecMainHandle, CellPngDecSubHandle, void *sbit);
int32_t cellPngDecGettRNS(CellPngDecMainHandle, CellPngDecSubHandle, void *trns);
int32_t cellPngDecGethIST(CellPngDecMainHandle, CellPngDecSubHandle, void *hist);
int32_t cellPngDecGettIME(CellPngDecMainHandle, CellPngDecSubHandle, void *time);
int32_t cellPngDecGetbKGD(CellPngDecMainHandle, CellPngDecSubHandle, void *bkgd);
int32_t cellPngDecGetsPLT(CellPngDecMainHandle, CellPngDecSubHandle, void *splt);
int32_t cellPngDecGetoFFs(CellPngDecMainHandle, CellPngDecSubHandle, void *offs);
int32_t cellPngDecGetpHYs(CellPngDecMainHandle, CellPngDecSubHandle, void *phys);
int32_t cellPngDecGetsCAL(CellPngDecMainHandle, CellPngDecSubHandle, void *scal);
int32_t cellPngDecGetcHRM(CellPngDecMainHandle, CellPngDecSubHandle, void *chrm);
int32_t cellPngDecGetpCAL(CellPngDecMainHandle, CellPngDecSubHandle, void *pcal);
int32_t cellPngDecGetTextChunk(CellPngDecMainHandle, CellPngDecSubHandle,
                               uint32_t *textInfoNum, void *textInfo[]);
int32_t cellPngDecGetUnknownChunks(CellPngDecMainHandle, CellPngDecSubHandle,
                                   void *unknownChunk[], uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_PNGDEC_H__ */
