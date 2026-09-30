/* cell/codec/vdec_divx.h - DivX decoding with cellVdec: profile levels, the stream information reported with each picture and its code values. */
#ifndef PS3TC_CELL_CODEC_VDEC_DIVX_H
#define PS3TC_CELL_CODEC_VDEC_DIVX_H

#include <stdint.h>
#include <stdbool.h>
#include <cell/codec/vdec.h>

#define CELL_VDEC_DIVX_QMOBILE                       10
#define CELL_VDEC_DIVX_MOBILE                        11
#define CELL_VDEC_DIVX_HOME_THEATER                  12
#define CELL_VDEC_DIVX_HD_720                        13
#define CELL_VDEC_DIVX_HD_1080                       14
#define CELL_VDEC_DIVX_FRC_UNDEFINED                 0x00
#define CELL_VDEC_DIVX_FRC_24000DIV1001              0x01
#define CELL_VDEC_DIVX_FRC_24                        0x02
#define CELL_VDEC_DIVX_FRC_25                        0x03
#define CELL_VDEC_DIVX_FRC_30000DIV1001              0x04
#define CELL_VDEC_DIVX_FRC_30                        0x05
#define CELL_VDEC_DIVX_FRC_50                        0x06
#define CELL_VDEC_DIVX_FRC_60000DIV1001              0x07
#define CELL_VDEC_DIVX_FRC_60                        0x08
#define CELL_VDEC_DIVX_ARI_PAR_1_1                   0x1
#define CELL_VDEC_DIVX_ARI_PAR_12_11                 0x2
#define CELL_VDEC_DIVX_ARI_PAR_10_11                 0x3
#define CELL_VDEC_DIVX_ARI_PAR_16_11                 0x4
#define CELL_VDEC_DIVX_ARI_PAR_40_33                 0x5
#define CELL_VDEC_DIVX_ARI_PAR_EXTENDED_PAR          0xF
#define CELL_VDEC_DIVX_VCT_I                         0x0
#define CELL_VDEC_DIVX_VCT_P                         0x1
#define CELL_VDEC_DIVX_VCT_B                         0x2
#define CELL_VDEC_DIVX_PSTR_FRAME                    0x0
#define CELL_VDEC_DIVX_PSTR_TOP_BTM                  0x1
#define CELL_VDEC_DIVX_PSTR_BTM_TOP                  0x2
#define CELL_VDEC_DIVX_CP_ITU_R_BT_709               0x01
#define CELL_VDEC_DIVX_CP_UNSPECIFIED                0x02
#define CELL_VDEC_DIVX_CP_ITU_R_BT_470_SYS_M         0x04
#define CELL_VDEC_DIVX_CP_ITU_R_BT_470_SYS_BG        0x05
#define CELL_VDEC_DIVX_CP_SMPTE_170_M                0x06
#define CELL_VDEC_DIVX_CP_SMPTE_240_M                0x07
#define CELL_VDEC_DIVX_CP_GENERIC_FILM               0x08
#define CELL_VDEC_DIVX_TC_ITU_R_BT_709               0x01
#define CELL_VDEC_DIVX_TC_UNSPECIFIED                0x02
#define CELL_VDEC_DIVX_TC_ITU_R_BT_470_SYS_M         0x04
#define CELL_VDEC_DIVX_TC_ITU_R_BT_470_SYS_BG        0x05
#define CELL_VDEC_DIVX_TC_SMPTE_170_M                0x06
#define CELL_VDEC_DIVX_TC_SMPTE_240_M                0x07
#define CELL_VDEC_DIVX_TC_LINEAR                     0x08
#define CELL_VDEC_DIVX_TC_LOG_100_1                  0x09
#define CELL_VDEC_DIVX_TC_LOG_316_1                  0x0a
#define CELL_VDEC_DIVX_MXC_ITU_R_BT_709              0x01
#define CELL_VDEC_DIVX_MXC_UNSPECIFIED               0x02
#define CELL_VDEC_DIVX_MXC_FCC                       0x04
#define CELL_VDEC_DIVX_MXC_ITU_R_BT_470_SYS_BG       0x05
#define CELL_VDEC_DIVX_MXC_SMPTE_170_M               0x06
#define CELL_VDEC_DIVX_MXC_SMPTE_240_M               0x07
#define CELL_VDEC_DIVX_MXC_YCGCO                     0x08

#ifdef __cplusplus
extern "C" {
#endif

/* optional, through CellVdecTypeEx.codecSpecificInfo */
typedef struct CellVdecDivxSpecificInfo {
	uint32_t thisSize;
	uint16_t maxDecodedFrameWidth;
	uint16_t maxDecodedFrameHeight;
} CellVdecDivxSpecificInfo;

typedef struct CellVdecDivxSpecificInfo2 {
	uint32_t thisSize;
	uint16_t maxDecodedFrameWidth;
	uint16_t maxDecodedFrameHeight;
	uint16_t numberOfDecodedFrameBuffer;
} CellVdecDivxSpecificInfo2;

typedef struct CellVdecDivxInfo {
	uint8_t  pictureType;
	uint16_t horizontalSize;
	uint16_t verticalSize;
	uint8_t  pixelAspectRatio;
	uint8_t  parWidth;
	uint8_t  parHeight;
	bool     colourDescription;
	uint8_t  colourPrimaries;
	uint8_t  transferCharacteristics;
	uint8_t  matrixCoefficients;
	uint8_t  pictureStruct;
	uint16_t frameRateCode;
} CellVdecDivxInfo;
#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_VDEC_DIVX_H */
