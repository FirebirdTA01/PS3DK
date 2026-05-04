/* cell/codec/adec.h — cellAdec audio decoder framework.
 *
 * Clean-room header.  9 entry points: QueryAttr / Open / OpenEx /
 * Close / StartSeq / EndSeq / DecodeAu / GetPcm / GetPcmItem.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR;
 * size_t fields in caller-allocated structs are uint32_t (SPRX
 * uses a 32-bit-pointer ABI).  Handles are typed as uint32_t.
 */
#ifndef __PS3DK_CELL_CODEC_ADEC_H__
#define __PS3DK_CELL_CODEC_ADEC_H__

#include <stdint.h>
#include <stddef.h>

#include <cell/codec/adec_base.h>
#include <cell/codec/types.h>

#ifdef __PPU__
#include <ppu-types.h>
/* Forward-declare CellSpurs to avoid the <cell/spurs/types.h>
 * transitive-include chain (same pattern as cell/codec/dmux.h). */
typedef struct CellSpurs CellSpurs;
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- codec type enum -------------------------------------------- */

enum CELL_ADEC_CORE_TYPE {
    CELL_ADEC_TYPE_RESERVED1,
    CELL_ADEC_TYPE_LPCM_PAMF,
    CELL_ADEC_TYPE_AC3,
    CELL_ADEC_TYPE_ATRACX,
    CELL_ADEC_TYPE_MP3,
    CELL_ADEC_TYPE_ATRAC3,
    CELL_ADEC_TYPE_MPEG_L2,
    CELL_ADEC_TYPE_RESERVED5,
    CELL_ADEC_TYPE_RESERVED6,
    CELL_ADEC_TYPE_RESERVED7,
    CELL_ADEC_TYPE_RESERVED8,
    CELL_ADEC_TYPE_CELP,
    CELL_ADEC_TYPE_RESERVED10,
    CELL_ADEC_TYPE_ATRACX_2CH,
    CELL_ADEC_TYPE_ATRACX_6CH,
    CELL_ADEC_TYPE_ATRACX_8CH,
    CELL_ADEC_TYPE_M4AAC,
    CELL_ADEC_TYPE_RESERVED12,
    CELL_ADEC_TYPE_RESERVED13,
    CELL_ADEC_TYPE_RESERVED14,
    CELL_ADEC_TYPE_RESERVED15,
    CELL_ADEC_TYPE_RESERVED16,
    CELL_ADEC_TYPE_RESERVED17,
    CELL_ADEC_TYPE_RESERVED18,
    CELL_ADEC_TYPE_RESERVED19,
    CELL_ADEC_TYPE_CELP8,
    /* trailing reserved slots omitted for brevity */
};

/* ---- channel / sample-rate / bit-length enums ---- -------------- */

enum CELL_ADEC_CHANNEL {
    CELL_ADEC_CH_RESERVED1,
    CELL_ADEC_CH_MONO,
    CELL_ADEC_CH_RESERVED2,
    CELL_ADEC_CH_STEREO,
    CELL_ADEC_CH_3_0,
    CELL_ADEC_CH_2_1,
    CELL_ADEC_CH_3_1,
    CELL_ADEC_CH_2_2,
    CELL_ADEC_CH_3_2,
    CELL_ADEC_CH_3_2_LFE,
    CELL_ADEC_CH_3_4,
    CELL_ADEC_CH_3_4_LFE,
    CELL_ADEC_CH_RESERVED3,
};

enum CELL_ADEC_SAMPEL_RATE {
    CELL_ADEC_FS_RESERVED1 = 0,
    CELL_ADEC_FS_48kHz = 1,
    CELL_ADEC_FS_16kHz = 2,
    CELL_ADEC_FS_8kHz  = 5,
};

enum CELL_ADEC_BIT_LENGTH {
    CELL_ADEC_BIT_LENGTH_RESERVED1,
    CELL_ADEC_BIT_LENGTH_16,
    CELL_ADEC_BIT_LENGTH_20,
    CELL_ADEC_BIT_LENGTH_24,
};

/* ---- message type enum ------------------------------------------ */

typedef enum {
    CELL_ADEC_MSG_TYPE_AUDONE,
    CELL_ADEC_MSG_TYPE_PCMOUT,
    CELL_ADEC_MSG_TYPE_ERROR,
    CELL_ADEC_MSG_TYPE_SEQDONE,
} CellAdecMsgType;

/* ---- types ------------------------------------------------------ */

typedef struct {
    uint32_t audioCodecType;
} CellAdecType;

typedef struct {
    uint32_t workMemSize;
    uint32_t adecVerUpper;
    uint32_t adecVerLower;
} CellAdecAttr;

typedef struct {
    uint32_t totalMemSize;
    void    *startAddr           ATTRIBUTE_PRXPTR;
    uint32_t ppuThreadPriority;
    uint32_t spuThreadPriority;
    uint32_t ppuThreadStackSize; /* uint32_t — SPRX ABI width */
} CellAdecResource;

typedef struct {
    uint32_t   totalMemSize;
    void      *startAddr           ATTRIBUTE_PRXPTR;
    uint32_t   ppuThreadPriority;
    uint32_t   ppuThreadStackSize; /* uint32_t — SPRX ABI width */
    CellSpurs *spurs               ATTRIBUTE_PRXPTR;
    uint8_t    priority[8];
    uint32_t   maxContention;
} CellAdecResourceEx;

/* ---- opaque handle (SPRX writes 4-byte EA into *handle) ---------- */

typedef uint32_t CellAdecHandle;

/* ---- callback --------------------------------------------------- */

typedef int32_t (*CellAdecCbMsg)(
    CellAdecHandle  handle,
    CellAdecMsgType msgType,
    int32_t         msgData,
    void           *callbackArg
);

typedef struct {
    CellAdecCbMsg  callbackFunc ATTRIBUTE_PRXPTR;
    void          *callbackArg  ATTRIBUTE_PRXPTR;
} CellAdecCb;

/* ---- timestamp alias -------------------------------------------- */

typedef CellCodecTimeStamp CellAdecTimeStamp;

/* ---- access unit info ------------------------------------------- */

typedef struct {
    void              *startAddr ATTRIBUTE_PRXPTR;
    uint32_t           size;
    CellCodecTimeStamp pts;
    uint64_t           userData;
} CellAdecAuInfo;

/* ---- BSI / PCM output ------------------------------------------- */

typedef struct {
    void *bsiInfo ATTRIBUTE_PRXPTR;
} CellAdecPcmAttr;

typedef struct {
    uint32_t        pcmHandle;
    uint32_t        status;
    void           *startAddr ATTRIBUTE_PRXPTR;
    uint32_t        size;
    CellAdecPcmAttr pcmAttr;
    CellAdecAuInfo  auInfo;
} CellAdecPcmItem;

/* ---- LPCM helper types ------------------------------------------ */

typedef struct {
    uint32_t channelNumber;
    uint32_t sampleRate;
    uint32_t sizeOfWord;
    uint32_t audioPayloadSize;
} CellAdecParamLpcm;

typedef struct {
    uint32_t channelNumber;
    uint32_t sampleRate;
    uint32_t outputDataSize;
} CellAdecLpcmInfo;

/* ---- API (9 entry points) --------------------------------------- */

int32_t cellAdecQueryAttr(CellAdecType *type, CellAdecAttr *attr);

int32_t cellAdecOpen(
    CellAdecType     *type,
    CellAdecResource *res,
    CellAdecCb       *cb,
    CellAdecHandle   *handle
);

int32_t cellAdecOpenEx(
    CellAdecType       *type,
    CellAdecResourceEx *res_ex,
    CellAdecCb         *cb,
    CellAdecHandle     *handle
);

int32_t cellAdecClose(CellAdecHandle handle);

int32_t cellAdecStartSeq(CellAdecHandle handle, void *param);

int32_t cellAdecEndSeq(CellAdecHandle handle);

int32_t cellAdecDecodeAu(CellAdecHandle handle, CellAdecAuInfo *auInfo);

int32_t cellAdecGetPcm(CellAdecHandle handle, void *outBuff);

int32_t cellAdecGetPcmItem(
    CellAdecHandle          handle,
    const CellAdecPcmItem **pcmItem
);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CODEC_ADEC_H__ */
