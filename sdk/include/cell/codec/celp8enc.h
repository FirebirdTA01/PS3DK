/* cell/codec/celp8enc.h — cellCelp8Enc CELP-8 voice encoder.
 *
 * Clean-room header.  9 entry points.  Encodes 8 kHz PCM → CELP-8
 * (MPE excitation) compressed stream.  Plugs into libadec as a
 * decoder via CELL_ADEC_TYPE_CELP8 + CELL_SYSMODULE_ADEC_CELP8.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR;
 * size_t / handle fields are uint32_t (SPRX 32-bit-pointer ABI).
 */
#ifndef __PS3DK_CELL_CODEC_CELP8ENC_H__
#define __PS3DK_CELL_CODEC_CELP8ENC_H__

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

#define CELL_CELP8ENC_ERROR_FAILED       CELL_ERROR_CAST(0x806140a1)
#define CELL_CELP8ENC_ERROR_SEQ          CELL_ERROR_CAST(0x806140a2)
#define CELL_CELP8ENC_ERROR_ARG          CELL_ERROR_CAST(0x806140a3)
#define CELL_CELP8ENC_ERROR_CORE_FAILED  CELL_ERROR_CAST(0x806140b1)
#define CELL_CELP8ENC_ERROR_CORE_SEQ     CELL_ERROR_CAST(0x806140b2)
#define CELL_CELP8ENC_ERROR_CORE_ARG     CELL_ERROR_CAST(0x806140b3)

typedef enum { CELL_CELP8ENC_EXCITATION_MODE_MPE = 0 } CELL_CELP8ENC_EXCITATION_MODE;
typedef enum {
    CELL_CELP8ENC_MPE_CONFIG_0=0,  CELL_CELP8ENC_MPE_CONFIG_2=2,
    CELL_CELP8ENC_MPE_CONFIG_6=6,  CELL_CELP8ENC_MPE_CONFIG_9=9,
    CELL_CELP8ENC_MPE_CONFIG_12=12,CELL_CELP8ENC_MPE_CONFIG_15=15,
    CELL_CELP8ENC_MPE_CONFIG_18=18,CELL_CELP8ENC_MPE_CONFIG_21=21,
    CELL_CELP8ENC_MPE_CONFIG_24=24,CELL_CELP8ENC_MPE_CONFIG_26=26,
} CELL_CELP8ENC_MPE_CONFIG;
typedef enum { CELL_CELP8ENC_WORD_SZ_FLOAT } CELL_CELP8ENC_WORD_SZ;
typedef enum { CELL_CELP8ENC_FS_8kHz = 1 } CELL_CELP8ENC_SAMPEL_RATE;

typedef struct {
    uint32_t excitationMode, sampleRate, configuration, wordSize;
    uint8_t *outBuff ATTRIBUTE_PRXPTR;
    uint32_t outSize;
} CellCelp8EncParam;

typedef struct {
    void     *startAddr ATTRIBUTE_PRXPTR;
    uint32_t  size;
} CellCelp8EncAuInfo, CellCelp8EncPcmInfo;

typedef struct {
    uint32_t workMemSize, celpEncVerUpper, celpEncVerLower;
} CellCelp8EncAttr;

typedef struct {
    uint32_t totalMemSize;
    void    *startAddr           ATTRIBUTE_PRXPTR;
    uint32_t ppuThreadPriority, spuThreadPriority;
    uint32_t ppuThreadStackSize;
} CellCelp8EncResource;

typedef struct {
    uint32_t   totalMemSize;
    void      *startAddr        ATTRIBUTE_PRXPTR;
    CellSpurs *spurs            ATTRIBUTE_PRXPTR;
    uint8_t    priority[8];
    uint32_t   maxContention;
} CellCelp8EncResourceEx;

typedef uint32_t CellCelp8EncHandle;

int32_t cellCelp8EncQueryAttr(CellCelp8EncAttr*);
int32_t cellCelp8EncOpen(CellCelp8EncResource*, CellCelp8EncHandle*);
int32_t cellCelp8EncOpenEx(CellCelp8EncResourceEx*, CellCelp8EncHandle*);
int32_t cellCelp8EncClose(CellCelp8EncHandle);
int32_t cellCelp8EncStart(CellCelp8EncHandle, CellCelp8EncParam*);
int32_t cellCelp8EncEnd(CellCelp8EncHandle);
int32_t cellCelp8EncEncodeFrame(CellCelp8EncHandle, CellCelp8EncPcmInfo*);
int32_t cellCelp8EncGetAu(CellCelp8EncHandle, void*, CellCelp8EncAuInfo*);
int32_t cellCelp8EncWaitForOutput(CellCelp8EncHandle);

#ifdef __cplusplus
}
#endif
#endif
