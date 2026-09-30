/* cell/codec/vdec_avc.h - H.264/AVC decoding with cellVdec: profile levels, the stream information reported with each picture (CellVdecPicItem.picInfo) and its code values. */
#ifndef PS3TC_CELL_CODEC_VDEC_AVC_H
#define PS3TC_CELL_CODEC_VDEC_AVC_H

#include <stdint.h>
#include <stdbool.h>
#include <cell/codec/vdec.h>

#define CELL_VDEC_AVC_CCD_MAX                        128
#define CELL_VDEC_AVC_LEVEL_1P0                      10
#define CELL_VDEC_AVC_LEVEL_1P1                      11
#define CELL_VDEC_AVC_LEVEL_1P2                      12
#define CELL_VDEC_AVC_LEVEL_1P3                      13
#define CELL_VDEC_AVC_LEVEL_2P0                      20
#define CELL_VDEC_AVC_LEVEL_2P1                      21
#define CELL_VDEC_AVC_LEVEL_2P2                      22
#define CELL_VDEC_AVC_LEVEL_3P0                      30
#define CELL_VDEC_AVC_LEVEL_3P1                      31
#define CELL_VDEC_AVC_LEVEL_3P2                      32
#define CELL_VDEC_AVC_LEVEL_4P0                      40
#define CELL_VDEC_AVC_LEVEL_4P1                      41
#define CELL_VDEC_AVC_LEVEL_4P2                      42
#define CELL_VDEC_AVC_VF_COMPONENT                   0x00
#define CELL_VDEC_AVC_VF_PAL                         0x01
#define CELL_VDEC_AVC_VF_NTSC                        0x02
#define CELL_VDEC_AVC_VF_SECAM                       0x03
#define CELL_VDEC_AVC_VF_MAC                         0x04
#define CELL_VDEC_AVC_VF_UNSPECIFIED                 0x05
#define CELL_VDEC_AVC_CP_ITU_R_BT_709_5              0x01
#define CELL_VDEC_AVC_CP_UNSPECIFIED                 0x02
#define CELL_VDEC_AVC_CP_ITU_R_BT_470_6_SYS_M        0x04
#define CELL_VDEC_AVC_CP_ITU_R_BT_470_6_SYS_BG       0x05
#define CELL_VDEC_AVC_CP_SMPTE_170_M                 0x06
#define CELL_VDEC_AVC_CP_SMPTE_240_M                 0x07
#define CELL_VDEC_AVC_CP_GENERIC_FILM                0x08
#define CELL_VDEC_AVC_TC_ITU_R_BT_709_5              0x01
#define CELL_VDEC_AVC_TC_UNSPECIFIED                 0x02
#define CELL_VDEC_AVC_TC_ITU_R_BT_470_6_SYS_M        0x04
#define CELL_VDEC_AVC_TC_ITU_R_BT_470_6_SYS_BG       0x05
#define CELL_VDEC_AVC_TC_SMPTE_170_M                 0x06
#define CELL_VDEC_AVC_TC_SMPTE_240_M                 0x07
#define CELL_VDEC_AVC_TC_LINEAR                      0x08
#define CELL_VDEC_AVC_TC_LOG_100_1                   0x09
#define CELL_VDEC_AVC_TC_LOG_316_1                   0x0a
#define CELL_VDEC_AVC_MXC_GBR                        0x00
#define CELL_VDEC_AVC_MXC_ITU_R_BT_709_5             0x01
#define CELL_VDEC_AVC_MXC_UNSPECIFIED                0x02
#define CELL_VDEC_AVC_MXC_FCC                        0x04
#define CELL_VDEC_AVC_MXC_ITU_R_BT_470_6_SYS_BG      0x05
#define CELL_VDEC_AVC_MXC_SMPTE_170_M                0x06
#define CELL_VDEC_AVC_MXC_SMPTE_240_M                0x07
#define CELL_VDEC_AVC_MXC_YCGCO                      0x08
#define CELL_VDEC_AVC_FRC_24000DIV1001               0x00
#define CELL_VDEC_AVC_FRC_24                         0x01
#define CELL_VDEC_AVC_FRC_25                         0x02
#define CELL_VDEC_AVC_FRC_30000DIV1001               0x03
#define CELL_VDEC_AVC_FRC_30                         0x04
#define CELL_VDEC_AVC_FRC_50                         0x05
#define CELL_VDEC_AVC_FRC_60000DIV1001               0x06
#define CELL_VDEC_AVC_FRC_60                         0x07
#define CELL_VDEC_AVC_FLG_SPS                        0x0001
#define CELL_VDEC_AVC_FLG_PPS                        0x0002
#define CELL_VDEC_AVC_FLG_AUD                        0x0004
#define CELL_VDEC_AVC_FLG_EO_SEQ                     0x0008
#define CELL_VDEC_AVC_FLG_EO_STREAM                  0x0100
#define CELL_VDEC_AVC_FLG_FILLER_DATA                0x0200
#define CELL_VDEC_AVC_FLG_PIC_TIMING_SEI             0x0400
#define CELL_VDEC_AVC_FLG_BUFF_PERIOD_SEI            0x0800
#define CELL_VDEC_AVC_FLG_USER_DATA_UNREG_SEI        0x1000
#define CELL_VDEC_AVC_ARI_SAR_UNSPECIFIED            0x00
#define CELL_VDEC_AVC_ARI_SAR_1_1                    0x01
#define CELL_VDEC_AVC_ARI_SAR_12_11                  0x02
#define CELL_VDEC_AVC_ARI_SAR_10_11                  0x03
#define CELL_VDEC_AVC_ARI_SAR_16_11                  0x04
#define CELL_VDEC_AVC_ARI_SAR_40_33                  0x05
#define CELL_VDEC_AVC_ARI_SAR_24_11                  0x06
#define CELL_VDEC_AVC_ARI_SAR_20_11                  0x07
#define CELL_VDEC_AVC_ARI_SAR_32_11                  0x08
#define CELL_VDEC_AVC_ARI_SAR_80_33                  0x09
#define CELL_VDEC_AVC_ARI_SAR_18_11                  0x0a
#define CELL_VDEC_AVC_ARI_SAR_15_11                  0x0b
#define CELL_VDEC_AVC_ARI_SAR_64_33                  0x0c
#define CELL_VDEC_AVC_ARI_SAR_160_99                 0x0d
#define CELL_VDEC_AVC_ARI_SAR_4_3                    0x0e
#define CELL_VDEC_AVC_ARI_SAR_3_2                    0x0f
#define CELL_VDEC_AVC_ARI_SAR_2_1                    0x10
#define CELL_VDEC_AVC_ARI_SAR_EXTENDED_SAR           0xff
#define CELL_VDEC_AVC_PCT_I                          0x00
#define CELL_VDEC_AVC_PCT_P                          0x01
#define CELL_VDEC_AVC_PCT_B                          0x02
#define CELL_VDEC_AVC_PCT_UNKNOWN                    0x03
#define CELL_VDEC_AVC_PSTR_FRAME                     0x00
#define CELL_VDEC_AVC_PSTR_FIELD_TOP                 0x01
#define CELL_VDEC_AVC_PSTR_FIELD_BTM                 0x02
#define CELL_VDEC_AVC_PSTR_FIELD_TOP_BTM             0x03
#define CELL_VDEC_AVC_PSTR_FIELD_BTM_TOP             0x04
#define CELL_VDEC_AVC_PSTR_FIELD_TOP_BTM_TOP         0x05
#define CELL_VDEC_AVC_PSTR_FIELD_BTM_TOP_BTM         0x06
#define CELL_VDEC_AVC_PSTR_FRAME_DOUBLING            0x07
#define CELL_VDEC_AVC_PSTR_FRAME_TRIPLING            0x08

#ifdef __cplusplus
extern "C" {
#endif

/* optional, through CellVdecTypeEx.codecSpecificInfo */
typedef struct CellVdecAvcSpecificInfo {
	uint32_t thisSize;                   /* sizeof this structure */
	uint16_t maxDecodedFrameWidth;
	uint16_t maxDecodedFrameHeight;
	bool     disableDeblockingFilter;
	uint8_t  numberOfDecodedFrameBuffer;
} CellVdecAvcSpecificInfo;

typedef struct CellVdecAvcInfo {
	uint16_t horizontalSize;
	uint16_t verticalSize;
	uint8_t  pictureType[2];             /* CELL_VDEC_AVC_PCT_* per field */
	bool     idrPictureFlag;
	uint8_t  aspect_ratio_idc;           /* CELL_VDEC_AVC_ARI_* */
	uint16_t sar_height;
	uint16_t sar_width;
	uint8_t  pic_struct;                 /* CELL_VDEC_AVC_PSTR_* */
	int16_t  picOrderCount[2];
	bool     vui_parameters_present_flag;
	bool     frame_mbs_only_flag;
	bool     video_signal_type_present_flag;
	uint8_t  video_format;               /* CELL_VDEC_AVC_VF_* */
	bool     video_full_range_flag;
	bool     colour_description_present_flag;
	uint8_t  colour_primaries;           /* CELL_VDEC_AVC_CP_* */
	uint8_t  transfer_characteristics;   /* CELL_VDEC_AVC_TC_* */
	uint8_t  matrix_coefficients;        /* CELL_VDEC_AVC_MXC_* */
	bool     timing_info_present_flag;
	uint8_t  frameRateCode;              /* CELL_VDEC_AVC_FRC_* */
	bool     fixed_frame_rate_flag;
	bool     low_delay_hrd_flag;
	bool     entropy_coding_mode_flag;
	uint16_t nalUnitPresentFlags;        /* CELL_VDEC_AVC_FLG_* */
	uint8_t  ccDataLength[2];
	uint8_t  ccData[2][CELL_VDEC_AVC_CCD_MAX];   /* closed-caption data per field */
	uint64_t reserved[2];
} CellVdecAvcInfo;
#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_VDEC_AVC_H */
