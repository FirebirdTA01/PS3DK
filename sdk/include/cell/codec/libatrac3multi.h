/* cell/codec/libatrac3multi.h — cellAtracMulti multi-channel ATRAC3 decoder.
 *
 * Clean-room header.  24 entry points for multi-channel ATRAC3
 * (5.1/7.1 surround) decode.  Same shape as cellAtrac (atrac3plus)
 * with channel-type query extensions.  Integrates into libadec
 * via CELL_ADEC_TYPE_ATRACX_2CH/6CH/8CH.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR.
 */
#ifndef __PS3DK_CELL_CODEC_LIBATRAC3MULTI_H__
#define __PS3DK_CELL_CODEC_LIBATRAC3MULTI_H__

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

/* ---- error codes ------------------------------------------------ */
#define CELL_ATRACMULTI_ERROR_FACILITY_ATRAC              CELL_ERROR_CAST(0x80610b00)
#define CELL_ATRACMULTI_OK                                CELL_ERROR_CAST(0x00000000)
#define CELL_ATRACMULTI_ERROR_API_FAIL                    CELL_ERROR_CAST(0x80610b01)
#define CELL_ATRACMULTI_ERROR_READSIZE_OVER_BUFFER        CELL_ERROR_CAST(0x80610b11)
#define CELL_ATRACMULTI_ERROR_UNKNOWN_FORMAT              CELL_ERROR_CAST(0x80610b12)
#define CELL_ATRACMULTI_ERROR_READSIZE_IS_TOO_SMALL       CELL_ERROR_CAST(0x80610b13)
#define CELL_ATRACMULTI_ERROR_ILLEGAL_SAMPLING_RATE       CELL_ERROR_CAST(0x80610b14)
#define CELL_ATRACMULTI_ERROR_ILLEGAL_DATA                CELL_ERROR_CAST(0x80610b15)
#define CELL_ATRACMULTI_ERROR_NO_DECODER                  CELL_ERROR_CAST(0x80610b21)
#define CELL_ATRACMULTI_ERROR_UNSET_DATA                  CELL_ERROR_CAST(0x80610b22)
#define CELL_ATRACMULTI_ERROR_DECODER_WAS_CREATED         CELL_ERROR_CAST(0x80610b23)
#define CELL_ATRACMULTI_ERROR_ALLDATA_WAS_DECODED         CELL_ERROR_CAST(0x80610b31)
#define CELL_ATRACMULTI_ERROR_NODATA_IN_BUFFER            CELL_ERROR_CAST(0x80610b32)
#define CELL_ATRACMULTI_ERROR_NOT_ALIGNED_OUT_BUFFER      CELL_ERROR_CAST(0x80610b33)
#define CELL_ATRACMULTI_ERROR_NEED_SECOND_BUFFER          CELL_ERROR_CAST(0x80610b34)
#define CELL_ATRACMULTI_ERROR_ALLDATA_IS_ONMEMORY         CELL_ERROR_CAST(0x80610b41)
#define CELL_ATRACMULTI_ERROR_ADD_DATA_IS_TOO_BIG         CELL_ERROR_CAST(0x80610b42)
#define CELL_ATRACMULTI_ERROR_NONEED_SECOND_BUFFER        CELL_ERROR_CAST(0x80610b51)
#define CELL_ATRACMULTI_ERROR_UNSET_LOOP_NUM              CELL_ERROR_CAST(0x80610b61)
#define CELL_ATRACMULTI_ERROR_ILLEGAL_SAMPLE              CELL_ERROR_CAST(0x80610b71)
#define CELL_ATRACMULTI_ERROR_ILLEGAL_RESET_BYTE          CELL_ERROR_CAST(0x80610b72)
#define CELL_ATRACMULTI_ERROR_ILLEGAL_PPU_THREAD_PRIORITY CELL_ERROR_CAST(0x80610b81)
#define CELL_ATRACMULTI_ERROR_ILLEGAL_SPU_THREAD_PRIORITY CELL_ERROR_CAST(0x80610b82)
#define CELL_ATRACMULTI_ERROR_API_PARAMETER               CELL_ERROR_CAST(0x80610b91)

#define CELL_ATRACMULTI_ALLDATA_IS_ON_MEMORY             (-1)
#define CELL_ATRACMULTI_NONLOOP_STREAM_DATA_IS_ON_MEMORY (-2)
#define CELL_ATRACMULTI_LOOP_STREAM_DATA_IS_ON_MEMORY    (-3)

/* ---- types ------------------------------------------------------ */
#define CELL_ATRACMULTI_HANDLE_SIZE  512

typedef struct {
    uint8_t uiWorkMem[CELL_ATRACMULTI_HANDLE_SIZE];
} CellAtracMultiHandle __attribute__((aligned(8)));

typedef struct {
    uint8_t *pucWriteAddr   ATTRIBUTE_PRXPTR;
    uint32_t uiWritableByte;
    uint32_t uiMinWriteByte;
    uint32_t uiReadPosition;
} CellAtracMultiBufferInfo;

typedef struct {
    CellSpurs *pSpurs       ATTRIBUTE_PRXPTR;
    uint8_t    priority[8];
} CellAtracMultiExtRes;

/* ---- channel type enum ------------------------------------------ */
typedef enum {
    CELL_ATRACMULTI_CHTYPE_NONE,
    CELL_ATRACMULTI_CHTYPE_LR,
    CELL_ATRACMULTI_CHTYPE_LSRS,
    CELL_ATRACMULTI_CHTYPE_C,
    CELL_ATRACMULTI_CHTYPE_LFE,
    CELL_ATRACMULTI_CHTYPE_LR2,
    CELL_ATRACMULTI_CHTYPE_LSRS2,
} CellAtracMultiChannelType;

/* ---- API (24 entry points) -------------------------------------- */
int32_t cellAtracMultiSetDataAndGetMemSize(CellAtracMultiHandle*, uint8_t*, uint32_t, uint32_t, uint32_t*);
int32_t cellAtracMultiCreateDecoder(CellAtracMultiHandle*, uint8_t*, uint32_t, uint32_t);
int32_t cellAtracMultiCreateDecoderExt(CellAtracMultiHandle*, uint8_t*, uint32_t, CellAtracMultiExtRes*);
int32_t cellAtracMultiDeleteDecoder(CellAtracMultiHandle*);
int32_t cellAtracMultiDecode(CellAtracMultiHandle*, float*, uint32_t*, uint32_t*, int32_t*);
int32_t cellAtracMultiAddStreamData(CellAtracMultiHandle*, uint32_t);
int32_t cellAtracMultiGetSecondBufferInfo(CellAtracMultiHandle*, uint32_t*, uint32_t*);
int32_t cellAtracMultiSetSecondBuffer(CellAtracMultiHandle*, uint8_t*, uint32_t);
int32_t cellAtracMultiGetRemainFrame(CellAtracMultiHandle*, int32_t*);
int32_t cellAtracMultiGetVacantSize(CellAtracMultiHandle*, uint32_t*);
int32_t cellAtracMultiGetStreamDataInfo(CellAtracMultiHandle*, uint8_t**, uint32_t*, uint32_t*);
int32_t cellAtracMultiGetChannel(CellAtracMultiHandle*, uint32_t*);
int32_t cellAtracMultiGetMaxSample(CellAtracMultiHandle*, uint32_t*);
int32_t cellAtracMultiGetNextSample(CellAtracMultiHandle*, uint32_t*);
int32_t cellAtracMultiGetSoundInfo(CellAtracMultiHandle*, int32_t*, int32_t*, int32_t*);
int32_t cellAtracMultiGetNextDecodePosition(CellAtracMultiHandle*, uint32_t*);
int32_t cellAtracMultiGetBitrate(CellAtracMultiHandle*, uint32_t*);
int32_t cellAtracMultiGetLoopInfo(CellAtracMultiHandle*, int32_t*, uint32_t*);
int32_t cellAtracMultiIsSecondBufferNeeded(CellAtracMultiHandle*);
int32_t cellAtracMultiSetLoopNum(CellAtracMultiHandle*, int32_t);
int32_t cellAtracMultiGetBufferInfoForResetting(CellAtracMultiHandle*, uint32_t, CellAtracMultiBufferInfo*);
int32_t cellAtracMultiResetPlayPosition(CellAtracMultiHandle*, uint32_t, uint32_t);
int32_t cellAtracMultiGetInternalErrorInfo(CellAtracMultiHandle*, int32_t*);
int32_t cellAtracMultiGetChannelType(CellAtracMultiHandle*, CellAtracMultiChannelType*);

#ifdef __cplusplus
}
#endif
#endif /* __PS3DK_CELL_CODEC_LIBATRAC3MULTI_H__ */
