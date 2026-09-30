/* cell/codec/adec_celp8.h - CELP8 (MPE excitation, 8 kHz) decoding with
 * cellAdec (CELL_ADEC_TYPE_CELP8): the parameter block passed at open time
 * and the stream information reported per frame. */
#ifndef PS3TC_CELL_CODEC_ADEC_CELP8_H
#define PS3TC_CELL_CODEC_ADEC_CELP8_H

#include <stdint.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_ADEC_ERROR_CELP8_BUSY        CELL_ERROR_CAST(0x80612ea1)
#define CELL_ADEC_ERROR_CELP8_EMPTY       CELL_ERROR_CAST(0x80612ea2)
#define CELL_ADEC_ERROR_CELP8_ARG         CELL_ERROR_CAST(0x80612ea3)
#define CELL_ADEC_ERROR_CELP8_SEQ         CELL_ERROR_CAST(0x80612ea4)
#define CELL_ADEC_ERROR_CELP8_CORE_FATAL  CELL_ERROR_CAST(0x80612eb1)
#define CELL_ADEC_ERROR_CELP8_CORE_ARG    CELL_ERROR_CAST(0x80612eb2)
#define CELL_ADEC_ERROR_CELP8_CORE_SEQ    CELL_ERROR_CAST(0x80612eb3)

enum CELL_ADEC_CELP8_EXCITATION_MODE {
	CELL_ADEC_CELP8_EXCITATION_MODE_MPE = 0
};

/* MPE configurations (the number is the bit-rate class) */
enum CELL_ADEC_CELP8_MPE_CONFIG {
	CELL_ADEC_CELP8_MPE_CONFIG_0  = 0,
	CELL_ADEC_CELP8_MPE_CONFIG_2  = 2,
	CELL_ADEC_CELP8_MPE_CONFIG_6  = 6,
	CELL_ADEC_CELP8_MPE_CONFIG_9  = 9,
	CELL_ADEC_CELP8_MPE_CONFIG_12 = 12,
	CELL_ADEC_CELP8_MPE_CONFIG_15 = 15,
	CELL_ADEC_CELP8_MPE_CONFIG_18 = 18,
	CELL_ADEC_CELP8_MPE_CONFIG_21 = 21,
	CELL_ADEC_CELP8_MPE_CONFIG_24 = 24,
	CELL_ADEC_CELP8_MPE_CONFIG_26 = 26
};

/* output PCM word: 32-bit float only */
enum CELL_ADEC_CELP8_WORD_SZ {
	CELL_ADEC_CELP8_WORD_SZ_FLOAT = 0
};

typedef struct {
	uint32_t excitationMode;   /* CELL_ADEC_CELP8_EXCITATION_MODE_* */
	uint32_t sampleRate;       /* Hz */
	uint32_t configuration;    /* CELL_ADEC_CELP8_MPE_CONFIG_* */
	uint32_t wordSize;         /* CELL_ADEC_CELP8_WORD_SZ_* */
} CellAdecParamCelp8;

typedef struct {
	uint32_t excitationMode;
	uint32_t sampleRate;
	uint32_t configuration;
	uint32_t wordSize;
} CellAdecCelp8Info;

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_ADEC_CELP8_H */
