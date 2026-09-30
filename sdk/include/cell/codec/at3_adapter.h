/* cell/codec/at3_adapter.h - ATRAC3 decoding with cellAdec
 * (CELL_ADEC_TYPE_ATRAC3): parameters, stream information, error codes. */
#ifndef PS3TC_CELL_CODEC_AT3_ADAPTER_H
#define PS3TC_CELL_CODEC_AT3_ADAPTER_H

#include <stdint.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int32_t nch;       /* channels */
	int32_t isJoint;   /* 0 dual stereo, 1 joint stereo */
	int32_t nbytes;    /* bytes per access unit */
	int32_t bw_pcm;    /* output sample: CELL_ADEC_ATRAC3_WORD_SZ_* */
} CellAdecParamAtrac3;

typedef struct {
	int32_t nch;
	int32_t isJoint;
	int32_t nbytes;    /* bytes per channel */
} CellAdecAtrac3Info;

#ifdef __cplusplus
}
#endif

#define CELL_ADEC_ATRAC3_WORD_SZ_16BIT               (0x02)
#define CELL_ADEC_ATRAC3_WORD_SZ_24BIT               (0x03)
#define CELL_ADEC_ATRAC3_WORD_SZ_32BIT               (0x04)
#define CELL_ADEC_ATRAC3_WORD_SZ_FLOAT               (0x84)
#define CELL_ADEC_ERROR_AT3_OFFSET                   CELL_ERROR_CAST(0x80612100)
#define CELL_ADEC_ERROR_AT3_OK                       CELL_ERROR_CAST(0x80612100)
#define CELL_ADEC_ERROR_AT3_BUSY                     CELL_ERROR_CAST(0x80612164)
#define CELL_ADEC_ERROR_AT3_EMPTY                    CELL_ERROR_CAST(0x80612165)
#define CELL_ADEC_ERROR_AT3_ERROR                    CELL_ERROR_CAST(0x80612180)

#endif /* PS3TC_CELL_CODEC_AT3_ADAPTER_H */
