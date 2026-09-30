/* cell/codec/mp3_adapter.h - MPEG-1/2 Layer III decoding with cellAdec
 * (CELL_ADEC_TYPE_MP3): parameters, per-frame header information, error
 * codes. */
#ifndef PS3TC_CELL_CODEC_MP3_ADAPTER_H
#define PS3TC_CELL_CODEC_MP3_ADAPTER_H

#include <stdint.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int32_t bw_pcm;    /* output sample: CELL_ADEC_MP3_WORD_SZ_* */
} CellAdecParamMP3;

/* the frame just decoded */
typedef struct {
	volatile unsigned int  ui_header;                 /* MPEG frame header */
	volatile unsigned int  ui_main_data_begin;
	volatile unsigned int  ui_main_data_remain_size;
	volatile unsigned int  ui_main_data_now_size;
	volatile unsigned char uc_crc;                    /* 0: protected by a CRC */
	volatile unsigned char uc_mode;                   /* 0 stereo, 1 joint, 2 dual, 3 mono */
	volatile unsigned char uc_mode_extension;
	volatile unsigned char uc_copyright;
	volatile unsigned char uc_original;
	volatile unsigned char uc_emphasis;
	volatile unsigned char uc_crc_error_flag;         /* set when the CRC did not match */
	volatile int           i_error_code;
} CellAdecMP3Info;

#ifdef __cplusplus
}
#endif

#define MP3_BW_FLOAT                                 (4)
#define MP3_BW_16BIT                                 (3)
#define MP3_BW_18BIT                                 (2)
#define MP3_BW_20BIT                                 (1)
#define MP3_BW_24BIT                                 (0)
#define CELL_ADEC_MP3_WORD_SZ_16BIT                  MP3_BW_16BIT
#define CELL_ADEC_MP3_WORD_SZ_FLOAT                  MP3_BW_FLOAT
#define CELL_ADEC_ERROR_MP3_OFFSET                   (0x80612700U)
#define CELL_ADEC_ERROR_MP3_OK                       CELL_ERROR_CAST(0x80612700)
#define CELL_ADEC_ERROR_MP3_BUSY                     CELL_ERROR_CAST(0x80612764)
#define CELL_ADEC_ERROR_MP3_EMPTY                    CELL_ERROR_CAST(0x80612765)
#define CELL_ADEC_ERROR_MP3_ERROR                    CELL_ERROR_CAST(0x80612781)
#define CELL_ADEC_ERROR_MP3_LOST_SYNC                CELL_ERROR_CAST(0x80612782)
#define CELL_ADEC_ERROR_MP3_NOT_L3                   CELL_ERROR_CAST(0x80612783)
#define CELL_ADEC_ERROR_MP3_BAD_BITRATE              CELL_ERROR_CAST(0x80612784)
#define CELL_ADEC_ERROR_MP3_BAD_SFREQ                CELL_ERROR_CAST(0x80612785)
#define CELL_ADEC_ERROR_MP3_BAD_EMPHASIS             CELL_ERROR_CAST(0x80612786)
#define CELL_ADEC_ERROR_MP3_BAD_BLKTYPE              CELL_ERROR_CAST(0x80612787)
#define CELL_ADEC_ERROR_MP3_BAD_VERSION              CELL_ERROR_CAST(0x8061278c)
#define CELL_ADEC_ERROR_MP3_BAD_MODE                 CELL_ERROR_CAST(0x8061278d)
#define CELL_ADEC_ERROR_MP3_BAD_MODE_EXT             CELL_ERROR_CAST(0x8061278e)
#define CELL_ADEC_ERROR_MP3_HUFFMAN_NUM              CELL_ERROR_CAST(0x80612796)
#define CELL_ADEC_ERROR_MP3_HUFFMAN_CASE_ID          CELL_ERROR_CAST(0x80612797)
#define CELL_ADEC_ERROR_MP3_SCALEFAC_COMPRESS        CELL_ERROR_CAST(0x80612798)
#define CELL_ADEC_ERROR_MP3_HGETBIT                  CELL_ERROR_CAST(0x80612799)
#define CELL_ADEC_ERROR_MP3_FLOATING_EXCEPTION       CELL_ERROR_CAST(0x8061279a)
#define CELL_ADEC_ERROR_MP3_ARRAY_OVERFLOW           CELL_ERROR_CAST(0x8061279b)
#define CELL_ADEC_ERROR_MP3_STEREO_PROCESSING        CELL_ERROR_CAST(0x8061279c)
#define CELL_ADEC_ERROR_MP3_JS_BOUND                 CELL_ERROR_CAST(0x8061279d)
#define CELL_ADEC_ERROR_MP3_PCMOUT                   CELL_ERROR_CAST(0x8061279e)

#endif /* PS3TC_CELL_CODEC_MP3_ADAPTER_H */
