/* cell/codec/vdec_mpeg2.h - MPEG-2 video decoding with cellVdec: profile levels, the stream information reported with each picture and its code values. */
#ifndef PS3TC_CELL_CODEC_VDEC_MPEG2_H
#define PS3TC_CELL_CODEC_VDEC_MPEG2_H

#include <stdint.h>
#include <stdbool.h>
#include <cell/codec/vdec.h>

#define CELL_VDEC_MPEG2_MP_LL                        0
#define CELL_VDEC_MPEG2_MP_ML                        1
#define CELL_VDEC_MPEG2_MP_H14                       2
#define CELL_VDEC_MPEG2_MP_HL                        3
#define CELL_VDEC_MPEG2_FLG_SEQ_HDR                  0x00000001
#define CELL_VDEC_MPEG2_FLG_SEQ_EXT                  0x00000002
#define CELL_VDEC_MPEG2_FLG_SEQ_DSP_EXT              0x00000004
#define CELL_VDEC_MPEG2_FLG_SEQ_USR_DAT              0x00000008
#define CELL_VDEC_MPEG2_FLG_SEQ_END                  0x00000010
#define CELL_VDEC_MPEG2_FLG_GOP_HDR                  0x00000020
#define CELL_VDEC_MPEG2_FLG_GOP_USR_DAT              0x00000040
#define CELL_VDEC_MPEG2_FLG_PIC_HDR_1                0x00000100
#define CELL_VDEC_MPEG2_FLG_PIC_EXT_1                0x00000200
#define CELL_VDEC_MPEG2_FLG_PIC_DSP_EXT_1            0x00000400
#define CELL_VDEC_MPEG2_FLG_PIC_USR_DAT_1            0x00000800
#define CELL_VDEC_MPEG2_FLG_PIC_HDR_2                0x00001000
#define CELL_VDEC_MPEG2_FLG_PIC_EXT_2                0x00002000
#define CELL_VDEC_MPEG2_FLG_PIC_DSP_EXT_2            0x00004000
#define CELL_VDEC_MPEG2_FLG_PIC_USR_DAT_2            0x00008000
#define CELL_VDEC_MPEG2_ARI_SAR_1_1                  0x01
#define CELL_VDEC_MPEG2_ARI_DAR_4_3                  0x02
#define CELL_VDEC_MPEG2_ARI_DAR_16_9                 0x03
#define CELL_VDEC_MPEG2_ARI_DAR_2P21_1               0x04
#define CELL_VDEC_MPEG1_ARI_SAR_1P0                  0x01
#define CELL_VDEC_MPEG1_ARI_SAR_0P6735               0x02
#define CELL_VDEC_MPEG1_ARI_SAR_0P7031               0x03
#define CELL_VDEC_MPEG1_ARI_SAR_0P7615               0x04
#define CELL_VDEC_MPEG1_ARI_SAR_0P8055               0x05
#define CELL_VDEC_MPEG1_ARI_SAR_0P8437               0x06
#define CELL_VDEC_MPEG1_ARI_SAR_0P8935               0x07
#define CELL_VDEC_MPEG1_ARI_SAR_0P9157               0x08
#define CELL_VDEC_MPEG1_ARI_SAR_0P9815               0x09
#define CELL_VDEC_MPEG1_ARI_SAR_1P0255               0x0a
#define CELL_VDEC_MPEG1_ARI_SAR_1P0695               0x0b
#define CELL_VDEC_MPEG1_ARI_SAR_1P0950               0x0c
#define CELL_VDEC_MPEG1_ARI_SAR_1P1575               0x0d
#define CELL_VDEC_MPEG1_ARI_SAR_1P2015               0x0e
#define CELL_VDEC_MPEG2_FRC_FORBIDDEN                0x00
#define CELL_VDEC_MPEG2_FRC_24000DIV1001             0x01
#define CELL_VDEC_MPEG2_FRC_24                       0x02
#define CELL_VDEC_MPEG2_FRC_25                       0x03
#define CELL_VDEC_MPEG2_FRC_30000DIV1001             0x04
#define CELL_VDEC_MPEG2_FRC_30                       0x05
#define CELL_VDEC_MPEG2_FRC_50                       0x06
#define CELL_VDEC_MPEG2_FRC_60000DIV1001             0x07
#define CELL_VDEC_MPEG2_FRC_60                       0x08
#define CELL_VDEC_MPEG2_VF_COMPONENT                 0x00
#define CELL_VDEC_MPEG2_VF_PAL                       0x01
#define CELL_VDEC_MPEG2_VF_NTSC                      0x02
#define CELL_VDEC_MPEG2_VF_SECAM                     0x03
#define CELL_VDEC_MPEG2_VF_MAC                       0x04
#define CELL_VDEC_MPEG2_VF_UNSPECIFIED               0x05
#define CELL_VDEC_MPEG2_CP_FORBIDDEN                 0x00
#define CELL_VDEC_MPEG2_CP_ITU_R_BT_709              0x01
#define CELL_VDEC_MPEG2_CP_UNSPECIFIED               0x02
#define CELL_VDEC_MPEG2_CP_ITU_R_BT_470_2_SYS_M      0x04
#define CELL_VDEC_MPEG2_CP_ITU_R_BT_470_2_SYS_BG     0x05
#define CELL_VDEC_MPEG2_CP_SMPTE_170_M               0x06
#define CELL_VDEC_MPEG2_CP_SMPTE_240_M               0x07
#define CELL_VDEC_MPEG2_TC_FORBIDDEN                 0x00
#define CELL_VDEC_MPEG2_TC_ITU_R_BT_709              0x01
#define CELL_VDEC_MPEG2_TC_UNSPECIFIED               0x02
#define CELL_VDEC_MPEG2_TC_ITU_R_BT_470_2_SYS_M      0x04
#define CELL_VDEC_MPEG2_TC_ITU_R_BT_470_2_SYS_BG     0x05
#define CELL_VDEC_MPEG2_TC_SMPTE_170_M               0x06
#define CELL_VDEC_MPEG2_TC_SMPTE_240_M               0x07
#define CELL_VDEC_MPEG2_TC_LINEAR                    0x08
#define CELL_VDEC_MPEG2_TC_LOG_100_1                 0x09
#define CELL_VDEC_MPEG2_TC_LOG_316_1                 0x0a
#define CELL_VDEC_MPEG2_MXC_FORBIDDEN                0x00
#define CELL_VDEC_MPEG2_MXC_ITU_R_BT_709             0x01
#define CELL_VDEC_MPEG2_MXC_UNSPECIFIED              0x02
#define CELL_VDEC_MPEG2_MXC_FCC                      0x04
#define CELL_VDEC_MPEG2_MXC_ITU_R_BT_470_2_SYS_BG    0x05
#define CELL_VDEC_MPEG2_MXC_SMPTE_170_M              0x06
#define CELL_VDEC_MPEG2_MXC_SMPTE_240_M              0x07
#define CELL_VDEC_MPEG2_PCT_FORBIDDEN                0x00
#define CELL_VDEC_MPEG2_PCT_I                        0x01
#define CELL_VDEC_MPEG2_PCT_P                        0x02
#define CELL_VDEC_MPEG2_PCT_B                        0x03
#define CELL_VDEC_MPEG2_PCT_D                        0x04
#define CELL_VDEC_MPEG2_PSTR_TOP_FIELD               0x01
#define CELL_VDEC_MPEG2_PSTR_BOTTOM_FIELD            0x02
#define CELL_VDEC_MPEG2_PSTR_FRAME                   0x03

#ifdef __cplusplus
extern "C" {
#endif

/* optional, through CellVdecTypeEx.codecSpecificInfo */
typedef struct CellVdecMpeg2SpecificInfo {
	uint32_t thisSize;
	uint16_t maxDecodedFrameWidth;
	uint16_t maxDecodedFrameHeight;
} CellVdecMpeg2SpecificInfo;

/* sequence, GOP and picture header fields of the picture (two entries
 * where a frame carries two fields) */
typedef struct CellVdecMpeg2Info {
	uint16_t horizontal_size;
	uint16_t vertical_size;
	uint8_t  aspect_ratio_information;
	uint8_t  frame_rate_code;
	bool     progressive_sequence;
	bool     low_delay;
	uint8_t  video_format;
	bool     colour_description;
	uint8_t  colour_primaries;
	uint8_t  transfer_characteristics;
	uint8_t  matrix_coefficients;
	uint16_t temporal_reference[2];
	uint8_t  picture_coding_type[2];
	uint8_t  picture_structure[2];
	bool     top_field_first;
	bool     repeat_first_field;
	bool     progressive_frame;
	uint32_t time_code;
	bool     closed_gop;
	bool     broken_link;
	uint16_t vbv_delay[2];
	uint16_t display_horizontal_size;
	uint16_t display_vertical_size;
	uint8_t  number_of_frame_centre_offsets[2];
	uint16_t frame_centre_horizontal_offset[2][3];
	uint16_t frame_centre_vertical_offset[2][3];
	uint32_t headerPresentFlags;
	uint32_t headerRetentionFlags;
	bool     mpeg1Flag;
	uint8_t  ccDataLength[2];
	uint8_t  ccData[2][128];
	uint64_t reserved[2];
} CellVdecMpeg2Info;
#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_VDEC_MPEG2_H */
