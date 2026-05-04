/* cell/codec/pamf.h — cellPamf PAMF container parser.
 *
 * Clean-room header.  cellPamf parses PAMF (PlayStation Advanced
 * Media Format) container headers, exposes EP (entry-point) tables
 * and stream metadata, and maps PAMF stream types to audio/video
 * decoder codec types.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR
 * for 4-byte effective-address storage on LP64 hosts.
 */
#ifndef __PS3DK_CELL_CODEC_PAMF_H__
#define __PS3DK_CELL_CODEC_PAMF_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <cell/error.h>
#include <cell/codec/types.h>

#ifdef __PPU__
#include <ppu-types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes ------------------------------------------------ */

/*
 * CELL_ERROR_FACILITY_CODEC = 0x061
 * libpamf range: 0x8061_0501 – 0x8061_05ff
 */

#define CELL_PAMF_ERROR_STREAM_NOT_FOUND     CELL_ERROR_CAST(0x80610501)
#define CELL_PAMF_ERROR_INVALID_PAMF         CELL_ERROR_CAST(0x80610502)
#define CELL_PAMF_ERROR_INVALID_ARG          CELL_ERROR_CAST(0x80610503)
#define CELL_PAMF_ERROR_UNKNOWN_TYPE         CELL_ERROR_CAST(0x80610504)
#define CELL_PAMF_ERROR_UNSUPPORTED_VERSION  CELL_ERROR_CAST(0x80610505)
#define CELL_PAMF_ERROR_UNKNOWN_STREAM       CELL_ERROR_CAST(0x80610506)
#define CELL_PAMF_ERROR_EP_NOT_FOUND         CELL_ERROR_CAST(0x80610507)
#define CELL_PAMF_ERROR_NOT_AVAILABLE        CELL_ERROR_CAST(0x80610508)

/* ---- stream type constants -------------------------------------- */

#define CELL_PAMF_STREAM_TYPE_AVC           0
#define CELL_PAMF_STREAM_TYPE_M2V           1
#define CELL_PAMF_STREAM_TYPE_ATRAC3PLUS    2
#define CELL_PAMF_STREAM_TYPE_PAMF_LPCM     3
#define CELL_PAMF_STREAM_TYPE_AC3           4
#define CELL_PAMF_STREAM_TYPE_USER_DATA     5

#define CELL_PAMF_STREAM_TYPE_VIDEO         20
#define CELL_PAMF_STREAM_TYPE_AUDIO         21

/* ---- init / attribute flags ------------------------------------- */

#define CELL_PAMF_INITIALIZE_VERIFY_ON      0x00000001
#define CELL_PAMF_INITIALIZE_VERIFY_OFF     0x00000000

#define CELL_PAMF_ATTRIBUTE_VERIFY_ON       0x00000001
#define CELL_PAMF_ATTRIBUTE_VERIFY_OFF      0x00000000
#define CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER  0x00000002

/* ---- AVC profile / level / FRC constants ------------------------- */

#define CELL_PAMF_AVC_PROFILE_MAIN   77
#define CELL_PAMF_AVC_PROFILE_HIGH  100

#define CELL_PAMF_AVC_LEVEL_2P1  21
#define CELL_PAMF_AVC_LEVEL_3P0  30
#define CELL_PAMF_AVC_LEVEL_3P1  31
#define CELL_PAMF_AVC_LEVEL_3P2  32
#define CELL_PAMF_AVC_LEVEL_4P1  41
#define CELL_PAMF_AVC_LEVEL_4P2  42

#define CELL_PAMF_AVC_FRC_24000DIV1001  0
#define CELL_PAMF_AVC_FRC_24            1
#define CELL_PAMF_AVC_FRC_25            2
#define CELL_PAMF_AVC_FRC_30000DIV1001  3
#define CELL_PAMF_AVC_FRC_30            4
#define CELL_PAMF_AVC_FRC_50            5
#define CELL_PAMF_AVC_FRC_60000DIV1001  6

/* ---- MPEG-2 Video profile / FRC constants ------------------------ */

#define CELL_PAMF_M2V_MP_ML    1
#define CELL_PAMF_M2V_MP_H14   2
#define CELL_PAMF_M2V_MP_HL    3
#define CELL_PAMF_M2V_UNKNOWN  255

#define CELL_PAMF_M2V_FRC_24000DIV1001  1
#define CELL_PAMF_M2V_FRC_24            2
#define CELL_PAMF_M2V_FRC_25            3
#define CELL_PAMF_M2V_FRC_30000DIV1001  4
#define CELL_PAMF_M2V_FRC_30            5
#define CELL_PAMF_M2V_FRC_50            6
#define CELL_PAMF_M2V_FRC_60000DIV1001  7

/* ---- LPCM / audio constants ------------------------------------- */

#define CELL_PAMF_FS_48kHz         1
#define CELL_PAMF_BIT_LENGTH_16    1
#define CELL_PAMF_BIT_LENGTH_24    3

/* ---- colour / aspect constants ---------------------------------- */

#define CELL_PAMF_ASPECT_RATIO_1_1      1
#define CELL_PAMF_ASPECT_RATIO_12_11    2
#define CELL_PAMF_ASPECT_RATIO_10_11    3
#define CELL_PAMF_ASPECT_RATIO_16_11    4
#define CELL_PAMF_ASPECT_RATIO_40_33    5
#define CELL_PAMF_ASPECT_RATIO_4_3     14

#define CELL_PAMF_COLOUR_PRIMARIES_ITR_R_BT_709         1
#define CELL_PAMF_COLOUR_PRIMARIES_UNSPECIFIED          2
#define CELL_PAMF_COLOUR_PRIMARIES_ITU_R_BT_470_SYS_M   4
#define CELL_PAMF_COLOUR_PRIMARIES_ITU_R_BT_470_SYS_BG  5
#define CELL_PAMF_COLOUR_PRIMARIES_SMPTE_170_M          6
#define CELL_PAMF_COLOUR_PRIMARIES_SMPTE_240_M          7
#define CELL_PAMF_COLOUR_PRIMARIES_GENERIC_FILM         8

#define CELL_PAMF_TRANSFER_CHARACTERISTICS_ITU_R_BT_709         1
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_UNSPECIFIED          2
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_ITU_R_BT_470_SYS_M   4
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_ITU_R_BT_470_SYS_BG  5
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_SMPTE_170_M          6
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_SMPTE_240_M          7
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_LINEAR               8
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_LOG_100_1            9
#define CELL_PAMF_TRANSFER_CHARACTERISTICS_LOG_316_1           10

#define CELL_PAMF_MATRIX_GBR                 0
#define CELL_PAMF_MATRIX_ITU_R_BT_709        1
#define CELL_PAMF_MATRIX_UNSPECIFIED         2
#define CELL_PAMF_MATRIX_FCC                 4
#define CELL_PAMF_MATRIX_ITU_R_BT_470_SYS_BG 5
#define CELL_PAMF_MATRIX_SMPTE_170_M         6
#define CELL_PAMF_MATRIX_SMPTE_240_M         7
#define CELL_PAMF_MATRIX_YCGCO               8

/* ---- stream info structs (output from Reader) -------------------- */

typedef struct {
    uint8_t  profileIdc;
    uint8_t  levelIdc;
    uint8_t  frameMbsOnlyFlag;
    uint8_t  videoSignalInfoFlag;
    uint8_t  frameRateInfo;
    uint8_t  aspectRatioIdc;
    uint16_t sarWidth;
    uint16_t sarHeight;
    uint16_t horizontalSize;
    uint16_t verticalSize;
    uint16_t frameCropLeftOffset;
    uint16_t frameCropRightOffset;
    uint16_t frameCropTopOffset;
    uint16_t frameCropBottomOffset;
    uint8_t  videoFormat;
    uint8_t  videoFullRangeFlag;
    uint8_t  colourPrimaries;
    uint8_t  transferCharacteristics;
    uint8_t  matrixCoefficients;
    uint8_t  entropyCodingModeFlag;
    uint8_t  deblockingFilterFlag;
    uint8_t  minNumSlicePerPictureIdc;
    uint8_t  nfwIdc;
    uint8_t  maxMeanBitrate;
} CellPamfAvcInfo;

typedef struct {
    uint8_t  profileAndLevelIndication;
    bool     progressiveSequence;
    uint8_t  videoSignalInfoFlag;
    uint8_t  frameRateInfo;
    uint8_t  aspectRatioIdc;
    uint16_t sarWidth;
    uint16_t sarHeight;
    uint16_t horizontalSize;
    uint16_t verticalSize;
    uint16_t horizontalSizeValue;
    uint16_t verticalSizeValue;
    uint8_t  videoFormat;
    uint8_t  videoFullRangeFlag;
    uint8_t  colourPrimaries;
    uint8_t  transferCharacteristics;
    uint8_t  matrixCoefficients;
} CellPamfM2vInfo;

typedef struct {
    uint32_t samplingFrequency;
    uint8_t  numberOfChannels;
    uint16_t bitsPerSample;
} CellPamfLpcmInfo;

typedef struct {
    uint32_t samplingFrequency;
    uint8_t  numberOfChannels;
} CellPamfAtrac3plusInfo;

typedef CellPamfAtrac3plusInfo CellPamfAc3Info;

/* ---- EP (entry-point) types ------------------------------------- */

typedef CellCodecTimeStamp CellPamfTimeStamp;

typedef struct {
    uint32_t            indexN;
    uint32_t            nThRefPictureOffset;
    CellCodecTimeStamp  pts;
    uint64_t            rpnOffset;
} CellPamfEp;

typedef struct {
    bool        isPamf;
    uint32_t    index;
    uint32_t    num;
    const void *pCur ATTRIBUTE_PRXPTR;   /* SPRX writes 4-byte EA */
} CellPamfEpIterator;

/* ---- reader opaque type ----------------------------------------- */

typedef struct {
    uint64_t internalData[16];   /* 128 bytes, caller-allocated */
} CellPamfReader;

/* ---- type aliases ----------------------------------------------- */

typedef CellCodecEsFilterId CellPamfEsFilterId;

/* ---- stream-type → codec-type macros ----------------------------
 *
 * These convenience macros require constants from cell/codec/vdec.h
 * and cell/codec/adec.h: CELL_VDEC_CODEC_TYPE_MPEG2, _AVC, _MAX,
 * CELL_ADEC_TYPE_ATRACX, _LPCM_PAMF, _AC3, _RESERVED1.
 * They are safe to omit if those headers are not yet available. */

#ifdef CELL_VDEC_CODEC_TYPE_MPEG2
#define cellPamfStreamTypeToVdecCodecType(pamfStreamType, vdecCodecType) \
    do {                                                                 \
        switch (pamfStreamType) {                                        \
        case CELL_PAMF_STREAM_TYPE_M2V:                                  \
            vdecCodecType = CELL_VDEC_CODEC_TYPE_MPEG2;                  \
            break;                                                       \
        case CELL_PAMF_STREAM_TYPE_AVC:                                  \
            vdecCodecType = CELL_VDEC_CODEC_TYPE_AVC;                    \
            break;                                                       \
        default:                                                         \
            vdecCodecType = CELL_VDEC_CODEC_TYPE_MAX;                    \
            break;                                                       \
        }                                                                \
    } while (0)
#endif

#ifdef CELL_ADEC_TYPE_ATRACX
#define cellPamfStreamTypeToAdecCodecType(pamfStreamType, adecCodecType) \
    do {                                                                  \
        switch (pamfStreamType) {                                         \
        case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS:                            \
            adecCodecType = CELL_ADEC_TYPE_ATRACX;                        \
            break;                                                        \
        case CELL_PAMF_STREAM_TYPE_PAMF_LPCM:                             \
            adecCodecType = CELL_ADEC_TYPE_LPCM_PAMF;                     \
            break;                                                        \
        case CELL_PAMF_STREAM_TYPE_AC3:                                   \
            adecCodecType = CELL_ADEC_TYPE_AC3;                           \
            break;                                                        \
        default:                                                          \
            adecCodecType = CELL_ADEC_TYPE_RESERVED1;                     \
            break;                                                        \
        }                                                                 \
    } while (0)
#endif

/* ---- API entry points (23) -------------------------------------- */

int cellPamfGetHeaderSize(
    const uint8_t *pAddr, uint64_t fileSize, uint64_t *pSize);

int cellPamfGetHeaderSize2(
    const uint8_t *pAddr, uint64_t fileSize, uint32_t attribute, uint64_t *pSize);

int cellPamfGetStreamOffsetAndSize(
    const uint8_t *pAddr, uint64_t fileSize, uint64_t *pOffset, uint64_t *pSize);

int cellPamfVerify(const uint8_t *pAddr, uint64_t fileSize);

int cellPamfReaderInitialize(
    CellPamfReader *pSelf, const uint8_t *pAddr, uint64_t fileSize, uint32_t attribute);

int cellPamfReaderGetPresentationStartTime(
    CellPamfReader *pSelf, CellCodecTimeStamp *pTimeStamp);

int cellPamfReaderGetPresentationEndTime(
    CellPamfReader *pSelf, CellCodecTimeStamp *pTimeStamp);

uint32_t cellPamfReaderGetMuxRateBound(CellPamfReader *pSelf);

uint8_t cellPamfReaderGetNumberOfStreams(CellPamfReader *pSelf);

uint8_t cellPamfReaderGetNumberOfSpecificStreams(
    CellPamfReader *pSelf, uint8_t streamType);

int cellPamfReaderSetStreamWithIndex(
    CellPamfReader *pSelf, uint8_t streamIndex);

int cellPamfReaderSetStreamWithTypeAndChannel(
    CellPamfReader *pSelf, uint8_t streamType, uint8_t ch);

int cellPamfStreamTypeToEsFilterId(
    uint8_t streamType, uint8_t ch, CellCodecEsFilterId *pEsFilterId);

int cellPamfReaderSetStreamWithTypeAndIndex(
    CellPamfReader *pSelf, uint8_t streamType, uint8_t streamIndex);

int cellPamfReaderGetStreamIndex(CellPamfReader *pSelf);

int cellPamfReaderGetStreamTypeAndChannel(
    CellPamfReader *pSelf, uint8_t *pStreamType, uint8_t *pCh);

int cellPamfReaderGetEsFilterId(
    CellPamfReader *pSelf, CellCodecEsFilterId *pEsFilterId);

int cellPamfReaderGetStreamInfo(
    CellPamfReader *pSelf, void *pInfo, uint32_t size);

uint32_t cellPamfReaderGetNumberOfEp(CellPamfReader *pSelf);

int cellPamfReaderGetEpIteratorWithIndex(
    CellPamfReader *pSelf, uint32_t epIndex, CellPamfEpIterator *pIt);

int cellPamfReaderGetEpIteratorWithTimeStamp(
    CellPamfReader *pSelf, CellCodecTimeStamp *pTimeStamp, CellPamfEpIterator *pIt);

int cellPamfEpIteratorGetEp(CellPamfEpIterator *pIt, CellPamfEp *pEp);

int cellPamfEpIteratorMove(CellPamfEpIterator *pIt, int steps, CellPamfEp *pEp);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CODEC_PAMF_H__ */
