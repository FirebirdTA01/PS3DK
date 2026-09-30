/* cell/codec/mpegbc_adapter.h - MPEG-1/2 audio (Layer I/II, multichannel)
 * decoding with cellAdec (CELL_ADEC_TYPE_MPEG_L2): parameters, stream
 * information, channel and mode values, error codes. */
#ifndef PS3TC_CELL_CODEC_MPEGBC_ADAPTER_H
#define PS3TC_CELL_CODEC_MPEGBC_ADAPTER_H

#include <stdint.h>
#include <cell/error.h>

#define CELL_ADEC_BSI_M2BC_SAMPLE_FREQUENCY_44       0
#define CELL_ADEC_BSI_M2BC_SAMPLE_FREQUENCY_48       1
#define CELL_ADEC_BSI_M2BC_SAMPLE_FREQUENCY_32       2
#define CELL_ADEC_BSI_M2BC_ERROR_PROTECTION_NONE     0
#define CELL_ADEC_BSI_M2BC_ERROR_PROTECTION_EXIST    1
#define CELL_ADEC_BSI_M2BC_BITRATE_32                1
#define CELL_ADEC_BSI_M2BC_BITRATE_48                2
#define CELL_ADEC_BSI_M2BC_BITRATE_56                3
#define CELL_ADEC_BSI_M2BC_BITRATE_64                4
#define CELL_ADEC_BSI_M2BC_BITRATE_80                5
#define CELL_ADEC_BSI_M2BC_BITRATE_96                6
#define CELL_ADEC_BSI_M2BC_BITRATE_112               7
#define CELL_ADEC_BSI_M2BC_BITRATE_128               8
#define CELL_ADEC_BSI_M2BC_BITRATE_160               9
#define CELL_ADEC_BSI_M2BC_BITRATE_192               10
#define CELL_ADEC_BSI_M2BC_BITRATE_224               11
#define CELL_ADEC_BSI_M2BC_BITRATE_256               12
#define CELL_ADEC_BSI_M2BC_BITRATE_320               13
#define CELL_ADEC_BSI_M2BC_BITRATE_384               14
#define CELL_ADEC_BSI_M2BC_STEREO_MODE_STERO         0
#define CELL_ADEC_BSI_M2BC_STEREO_MODE_JOINTSTERO    1
#define CELL_ADEC_BSI_M2BC_STEREO_MODE_DUAL          2
#define CELL_ADEC_BSI_M2BC_STEREO_MODE_MONO          3
#define CELL_ADEC_BSI_M2BC_STEREO_EXMODE_0           0
#define CELL_ADEC_BSI_M2BC_STEREO_EXMODE_1           1
#define CELL_ADEC_BSI_M2BC_STEREO_EXMODE_2           2
#define CELL_ADEC_BSI_M2BC_STEREO_EXMODE_3           3
#define CELL_ADEC_BSI_M2BC_EMPHASIS_NONE             0
#define CELL_ADEC_BSI_M2BC_EMPHASIS_50_15            1
#define CELL_ADEC_BSI_M2BC_EMPHASIS_CCITT            3
#define CELL_ADEC_BSI_M2BC_COPYRIGHT_NONE            0
#define CELL_ADEC_BSI_M2BC_COPYRIGHT_ON              0
#define CELL_ADEC_BSI_M2BC_ORIGINAL_COPY             0
#define CELL_ADEC_BSI_M2BC_ORIGINAL_ORIGINAL         1
#define CELL_ADEC_BSI_M2BC_SURROUND_NONE             0
#define CELL_ADEC_BSI_M2BC_SURROUND_MONO             1
#define CELL_ADEC_BSI_M2BC_SURROUND_STEREO           2
#define CELL_ADEC_BSI_M2BC_SURROUND_SECOND           3
#define CELL_ADEC_BSI_M2BC_CENTER_NONE               0
#define CELL_ADEC_BSI_M2BC_CENTER_EXIST              1
#define CELL_ADEC_BSI_M2BC_CENTER_FHANTOM            3
#define CELL_ADEC_BSI_M2BC_LFE_NONE                  0
#define CELL_ADEC_BSI_M2BC_LFE_EXIST                 1
#define CELL_ADEC_BSI_M2BC_AUDIOMIX_LARGE            0
#define CELL_ADEC_BSI_M2BC_AUDIOMIX_SMALLE           1
#define CELL_ADEC_BSI_M2BC_MCEXTENSION_2CH           0
#define CELL_ADEC_BSI_M2BC_MCEXTENSION_5CH           1
#define CELL_ADEC_BSI_M2BC_MCEXTENSION_7CH           2
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_MONO            0
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_DUAL            1
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R             2
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R_S           3
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R_C           4
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R_LS_RS       5
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R_C_S         6
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R_C_LS_RS     7
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_LL_RR_CC_LS_RS_LC_RC 8
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_MONO_SECONDSTEREO 9
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R_SECONDSTEREO 10
#define CELL_ADEC_BSI_M2BC_CH_CONFIG_L_R_C_SECONDSTEREO 11
#define CELL_ADEC_ERROR_M2BC_FATAL                   CELL_ERROR_CAST(0x80612b01)
#define CELL_ADEC_ERROR_M2BC_SEQ                     CELL_ERROR_CAST(0x80612b02)
#define CELL_ADEC_ERROR_M2BC_ARG                     CELL_ERROR_CAST(0x80612b03)
#define CELL_ADEC_ERROR_M2BC_BUSY                    CELL_ERROR_CAST(0x80612b04)
#define CELL_ADEC_ERROR_M2BC_EMPTY                   CELL_ERROR_CAST(0x80612b05)
#define CELL_ADEC_ERROR_M2BC_SYNCF                   CELL_ERROR_CAST(0x80612b11)
#define CELL_ADEC_ERROR_M2BC_LAYER                   CELL_ERROR_CAST(0x80612b12)
#define CELL_ADEC_ERROR_M2BC_BITRATE                 CELL_ERROR_CAST(0x80612b13)
#define CELL_ADEC_ERROR_M2BC_SAMPLEFREQ              CELL_ERROR_CAST(0x80612b14)
#define CELL_ADEC_ERROR_M2BC_VERSION                 CELL_ERROR_CAST(0x80612b15)
#define CELL_ADEC_ERROR_M2BC_MODE_EXT                CELL_ERROR_CAST(0x80612b16)
#define CELL_ADEC_ERROR_M2BC_UNSUPPORT               CELL_ERROR_CAST(0x80612b17)
#define CELL_ADEC_ERROR_M2BC_OPENBS_EX               CELL_ERROR_CAST(0x80612b21)
#define CELL_ADEC_ERROR_M2BC_SYNCF_EX                CELL_ERROR_CAST(0x80612b22)
#define CELL_ADEC_ERROR_M2BC_CRCGET_EX               CELL_ERROR_CAST(0x80612b23)
#define CELL_ADEC_ERROR_M2BC_CRC_EX                  CELL_ERROR_CAST(0x80612b24)
#define CELL_ADEC_ERROR_M2BC_CRCGET                  CELL_ERROR_CAST(0x80612b31)
#define CELL_ADEC_ERROR_M2BC_CRC                     CELL_ERROR_CAST(0x80612b32)
#define CELL_ADEC_ERROR_M2BC_BITALLOC                CELL_ERROR_CAST(0x80612b33)
#define CELL_ADEC_ERROR_M2BC_SCALE                   CELL_ERROR_CAST(0x80612b34)
#define CELL_ADEC_ERROR_M2BC_SAMPLE                  CELL_ERROR_CAST(0x80612b35)
#define CELL_ADEC_ERROR_M2BC_OPENBS                  CELL_ERROR_CAST(0x80612b36)
#define CELL_ADEC_ERROR_M2BC_MC_CRCGET               CELL_ERROR_CAST(0x80612b41)
#define CELL_ADEC_ERROR_M2BC_MC_CRC                  CELL_ERROR_CAST(0x80612b42)
#define CELL_ADEC_ERROR_M2BC_MC_BITALLOC             CELL_ERROR_CAST(0x80612b43)
#define CELL_ADEC_ERROR_M2BC_MC_SCALE                CELL_ERROR_CAST(0x80612b44)
#define CELL_ADEC_ERROR_M2BC_MC_SAMPLE               CELL_ERROR_CAST(0x80612b45)
#define CELL_ADEC_ERROR_M2BC_MC_HEADER               CELL_ERROR_CAST(0x80612b46)
#define CELL_ADEC_ERROR_M2BC_MC_STATUS               CELL_ERROR_CAST(0x80612b47)
#define CELL_ADEC_ERROR_M2BC_AG_CCRCGET              CELL_ERROR_CAST(0x80612b51)
#define CELL_ADEC_ERROR_M2BC_AG_CRC                  CELL_ERROR_CAST(0x80612b52)
#define CELL_ADEC_ERROR_M2BC_AG_BITALLOC             CELL_ERROR_CAST(0x80612b53)
#define CELL_ADEC_ERROR_M2BC_AG_SCALE                CELL_ERROR_CAST(0x80612b54)
#define CELL_ADEC_ERROR_M2BC_AG_SAMPLE               CELL_ERROR_CAST(0x80612b55)
#define CELL_ADEC_ERROR_M2BC_AG_STATUS               CELL_ERROR_CAST(0x80612b57)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t channelNumber;   /* output channels */
	uint32_t downmix;
	uint32_t lfeUpSample;
} CellAdecParamMpmc;

/* The field names keep the reference spelling (errorPprotection,
 * stereoModeEextention, audioMmix, outputFramSize, channelCoufiguration),
 * since code names them. */
typedef struct {
	uint32_t channelNumber;
	uint32_t sampleFreq;
	uint32_t errorPprotection;
	uint32_t bitrateIndex;
	uint32_t stereoMode;
	uint32_t stereoModeEextention;
	uint32_t emphasis;
	uint32_t copyright;
	uint32_t original;
	uint32_t surroundMode;
	uint32_t centerMode;
	uint32_t audioMmix;
	uint32_t outputFramSize;
	uint32_t multiCodecMode;
	uint32_t lfePresent;
	uint32_t channelCoufiguration;
} CellAdecMpmcInfo;

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_MPEGBC_ADAPTER_H */
