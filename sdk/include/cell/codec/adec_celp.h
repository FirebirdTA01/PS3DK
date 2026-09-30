/* cell/codec/adec_celp.h - CELP (RPE excitation, 16 kHz) decoding with
 * cellAdec (CELL_ADEC_TYPE_CELP): the parameter block passed at open time
 * and the stream information reported per frame. */
#ifndef PS3TC_CELL_CODEC_ADEC_CELP_H
#define PS3TC_CELL_CODEC_ADEC_CELP_H

#include <stdint.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_ADEC_ERROR_CELP_BUSY        CELL_ERROR_CAST(0x80612e01)
#define CELL_ADEC_ERROR_CELP_EMPTY       CELL_ERROR_CAST(0x80612e02)
#define CELL_ADEC_ERROR_CELP_ARG         CELL_ERROR_CAST(0x80612e03)
#define CELL_ADEC_ERROR_CELP_SEQ         CELL_ERROR_CAST(0x80612e04)
#define CELL_ADEC_ERROR_CELP_CORE_FATAL  CELL_ERROR_CAST(0x80612e81)
#define CELL_ADEC_ERROR_CELP_CORE_ARG    CELL_ERROR_CAST(0x80612e82)
#define CELL_ADEC_ERROR_CELP_CORE_SEQ    CELL_ERROR_CAST(0x80612e83)

enum CELL_ADEC_CELP_EXCITATION_MODE {
	CELL_ADEC_CELP_EXCITATION_MODE_RPE = 1
};

enum CELL_ADEC_CELP_RPE_CONFIG {
	CELL_ADEC_CELP_RPE_CONFIG_0 = 0,
	CELL_ADEC_CELP_RPE_CONFIG_1 = 1,
	CELL_ADEC_CELP_RPE_CONFIG_2 = 2,
	CELL_ADEC_CELP_RPE_CONFIG_3 = 3
};

/* output PCM word */
enum CELL_ADEC_CELP_WORD_SZ {
	CELL_ADEC_CELP_WORD_SZ_INT16_LE = 0,   /* 16-bit signed, little-endian */
	CELL_ADEC_CELP_WORD_SZ_FLOAT    = 1    /* 32-bit float */
};

typedef struct {
	uint32_t excitationMode;   /* CELL_ADEC_CELP_EXCITATION_MODE_* */
	uint32_t sampleRate;       /* Hz */
	uint32_t configuration;    /* CELL_ADEC_CELP_RPE_CONFIG_* */
	uint32_t wordSize;         /* CELL_ADEC_CELP_WORD_SZ_* */
} CellAdecParamCelp;

typedef struct {
	uint32_t excitationMode;
	uint32_t sampleRate;
	uint32_t configuration;
	uint32_t wordSize;
} CellAdecCelpInfo;

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_ADEC_CELP_H */
