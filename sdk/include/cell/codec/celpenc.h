/* cell/codec/celpenc.h — cellCelpEnc CELP voice encoder.
 *
 * Clean-room header.  9 entry points.  Encodes 16 kHz PCM → CELP
 * (RPE excitation) compressed stream.  Plugs into libadec as a
 * decoder via CELL_ADEC_TYPE_CELP + CELL_SYSMODULE_ADEC_CELP.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR;
 * size_t / handle fields are uint32_t (SPRX 32-bit-pointer ABI).
 */
#ifndef __PS3DK_CELL_CODEC_CELPENC_H__
#define __PS3DK_CELL_CODEC_CELPENC_H__

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

#define CELL_CELPENC_ERROR_FAILED       CELL_ERROR_CAST(0x80614001)
#define CELL_CELPENC_ERROR_SEQ          CELL_ERROR_CAST(0x80614002)
#define CELL_CELPENC_ERROR_ARG          CELL_ERROR_CAST(0x80614003)
#define CELL_CELPENC_ERROR_CORE_FAILED  CELL_ERROR_CAST(0x80614081)
#define CELL_CELPENC_ERROR_CORE_SEQ     CELL_ERROR_CAST(0x80614082)
#define CELL_CELPENC_ERROR_CORE_ARG     CELL_ERROR_CAST(0x80614083)

typedef enum { CELL_CELPENC_EXCITATION_MODE_RPE = 1 } CELL_CELPENC_EXCITATION_MODE;
typedef enum {
    CELL_CELPENC_RPE_CONFIG_0, CELL_CELPENC_RPE_CONFIG_1,
    CELL_CELPENC_RPE_CONFIG_2, CELL_CELPENC_RPE_CONFIG_3,
} CELL_CELPENC_RPE_CONFIG;
typedef enum { CELL_CELPENC_WORD_SZ_INT16_LE, CELL_CELPENC_WORD_SZ_FLOAT } CELL_CELPENC_WORD_SZ;
typedef enum { CELL_CELPENC_FS_16kHz = 2 } CELL_CELPENC_SAMPEL_RATE;

typedef struct {
    uint32_t excitationMode, sampleRate, configuration, wordSize;
    uint8_t *outBuff ATTRIBUTE_PRXPTR;
    uint32_t outSize;
} CellCelpEncParam;

typedef struct {
    void     *startAddr ATTRIBUTE_PRXPTR;
    uint32_t  size;
} CellCelpEncAuInfo, CellCelpEncPcmInfo;

typedef struct {
    uint32_t workMemSize, celpEncVerUpper, celpEncVerLower;
} CellCelpEncAttr;

typedef struct {
    uint32_t totalMemSize;
    void    *startAddr           ATTRIBUTE_PRXPTR;
    uint32_t ppuThreadPriority, spuThreadPriority;
    uint32_t ppuThreadStackSize;
} CellCelpEncResource;

typedef struct {
    uint32_t   totalMemSize;
    void      *startAddr        ATTRIBUTE_PRXPTR;
    CellSpurs *spurs            ATTRIBUTE_PRXPTR;
    uint8_t    priority[8];
    uint32_t   maxContention;
} CellCelpEncResourceEx;

typedef uint32_t CellCelpEncHandle;

int32_t cellCelpEncQueryAttr(CellCelpEncAttr*);
int32_t cellCelpEncOpen(CellCelpEncResource*, CellCelpEncHandle*);
int32_t cellCelpEncOpenEx(CellCelpEncResourceEx*, CellCelpEncHandle*);
int32_t cellCelpEncClose(CellCelpEncHandle);
int32_t cellCelpEncStart(CellCelpEncHandle, CellCelpEncParam*);
int32_t cellCelpEncEnd(CellCelpEncHandle);
int32_t cellCelpEncEncodeFrame(CellCelpEncHandle, CellCelpEncPcmInfo*);
int32_t cellCelpEncGetAu(CellCelpEncHandle, void*, CellCelpEncAuInfo*);
int32_t cellCelpEncWaitForOutput(CellCelpEncHandle);

#ifdef __cplusplus
}
#endif
#endif
