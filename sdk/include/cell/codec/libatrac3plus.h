/* cell/codec/libatrac3plus.h — cellAtrac ATRAC3+ audio codec.
 *
 * Clean-room header.  23 entry points for ATRAC3+ decode: stream
 * data management, decoder create/delete, decode, buffer info,
 * sound-info queries, loop control, and reset/seek.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR.
 */
#ifndef __PS3DK_CELL_CODEC_LIBATRAC3PLUS_H__
#define __PS3DK_CELL_CODEC_LIBATRAC3PLUS_H__

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
#define CELL_ATRAC_ERROR_FACILITY_ATRAC              CELL_ERROR_CAST(0x80610300)
#define CELL_ATRAC_OK                                CELL_ERROR_CAST(0x00000000)
#define CELL_ATRAC_ERROR_API_FAIL                    CELL_ERROR_CAST(0x80610301)
#define CELL_ATRAC_ERROR_READSIZE_OVER_BUFFER        CELL_ERROR_CAST(0x80610311)
#define CELL_ATRAC_ERROR_UNKNOWN_FORMAT              CELL_ERROR_CAST(0x80610312)
#define CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL       CELL_ERROR_CAST(0x80610313)
#define CELL_ATRAC_ERROR_ILLEGAL_SAMPLING_RATE       CELL_ERROR_CAST(0x80610314)
#define CELL_ATRAC_ERROR_ILLEGAL_DATA                CELL_ERROR_CAST(0x80610315)
#define CELL_ATRAC_ERROR_NO_DECODER                  CELL_ERROR_CAST(0x80610321)
#define CELL_ATRAC_ERROR_UNSET_DATA                  CELL_ERROR_CAST(0x80610322)
#define CELL_ATRAC_ERROR_DECODER_WAS_CREATED         CELL_ERROR_CAST(0x80610323)
#define CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED         CELL_ERROR_CAST(0x80610331)
#define CELL_ATRAC_ERROR_NODATA_IN_BUFFER            CELL_ERROR_CAST(0x80610332)
#define CELL_ATRAC_ERROR_NOT_ALIGNED_OUT_BUFFER      CELL_ERROR_CAST(0x80610333)
#define CELL_ATRAC_ERROR_NEED_SECOND_BUFFER          CELL_ERROR_CAST(0x80610334)
#define CELL_ATRAC_ERROR_ALLDATA_IS_ONMEMORY         CELL_ERROR_CAST(0x80610341)
#define CELL_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG         CELL_ERROR_CAST(0x80610342)
#define CELL_ATRAC_ERROR_NONEED_SECOND_BUFFER        CELL_ERROR_CAST(0x80610351)
#define CELL_ATRAC_ERROR_UNSET_LOOP_NUM              CELL_ERROR_CAST(0x80610361)
#define CELL_ATRAC_ERROR_ILLEGAL_SAMPLE              CELL_ERROR_CAST(0x80610371)
#define CELL_ATRAC_ERROR_ILLEGAL_RESET_BYTE          CELL_ERROR_CAST(0x80610372)
#define CELL_ATRAC_ERROR_ILLEGAL_PPU_THREAD_PRIORITY CELL_ERROR_CAST(0x80610381)
#define CELL_ATRAC_ERROR_ILLEGAL_SPU_THREAD_PRIORITY CELL_ERROR_CAST(0x80610382)

/* ---- magic frame-count sentinels -------------------------------- */
#define CELL_ATRAC_ALLDATA_IS_ON_MEMORY             (-1)
#define CELL_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY (-2)
#define CELL_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY    (-3)

/* ---- types ------------------------------------------------------ */
#define CELL_ATRAC_HANDLE_SIZE  512

typedef struct {
    uint8_t uiWorkMem[CELL_ATRAC_HANDLE_SIZE];
} CellAtracHandle __attribute__((aligned(8)));

typedef struct {
    uint8_t *pucWriteAddr   ATTRIBUTE_PRXPTR;
    uint32_t uiWritableByte;
    uint32_t uiMinWriteByte;
    uint32_t uiReadPosition;
} CellAtracBufferInfo;

typedef struct {
    CellSpurs *pSpurs       ATTRIBUTE_PRXPTR;
    uint8_t    priority[8];
} CellAtracExtRes;

/* ---- API (23 entry points) -------------------------------------- */
int32_t cellAtracSetDataAndGetMemSize(CellAtracHandle*, uint8_t*, uint32_t, uint32_t, uint32_t*);
int32_t cellAtracCreateDecoder(CellAtracHandle*, uint8_t*, uint32_t, uint32_t);
int32_t cellAtracCreateDecoderExt(CellAtracHandle*, uint8_t*, uint32_t, CellAtracExtRes*);
int32_t cellAtracDeleteDecoder(CellAtracHandle*);
int32_t cellAtracDecode(CellAtracHandle*, float*, uint32_t*, uint32_t*, int32_t*);
int32_t cellAtracAddStreamData(CellAtracHandle*, uint32_t);
int32_t cellAtracGetSecondBufferInfo(CellAtracHandle*, uint32_t*, uint32_t*);
int32_t cellAtracSetSecondBuffer(CellAtracHandle*, uint8_t*, uint32_t);
int32_t cellAtracGetRemainFrame(CellAtracHandle*, int32_t*);
int32_t cellAtracGetVacantSize(CellAtracHandle*, uint32_t*);
int32_t cellAtracGetStreamDataInfo(CellAtracHandle*, uint8_t**, uint32_t*, uint32_t*);
int32_t cellAtracGetChannel(CellAtracHandle*, uint32_t*);
int32_t cellAtracGetMaxSample(CellAtracHandle*, uint32_t*);
int32_t cellAtracGetNextSample(CellAtracHandle*, uint32_t*);
int32_t cellAtracGetSoundInfo(CellAtracHandle*, int32_t*, int32_t*, int32_t*);
int32_t cellAtracGetNextDecodePosition(CellAtracHandle*, uint32_t*);
int32_t cellAtracGetBitrate(CellAtracHandle*, uint32_t*);
int32_t cellAtracGetLoopInfo(CellAtracHandle*, int32_t*, uint32_t*);
int32_t cellAtracIsSecondBufferNeeded(CellAtracHandle*);
int32_t cellAtracSetLoopNum(CellAtracHandle*, int32_t);
int32_t cellAtracGetBufferInfoForResetting(CellAtracHandle*, uint32_t, CellAtracBufferInfo*);
int32_t cellAtracResetPlayPosition(CellAtracHandle*, uint32_t, uint32_t);
int32_t cellAtracGetInternalErrorInfo(CellAtracHandle*, int32_t*);

#ifdef __cplusplus
}
#endif
#endif /* __PS3DK_CELL_CODEC_LIBATRAC3PLUS_H__ */
