/* cell/codec/lpcm_adapter.h - error codes of the LPCM decoder behind cellAdec
 * (CELL_ADEC_TYPE_LPCM_*); its parameters are CellAdecParamLpcm in adec.h. */
#ifndef PS3TC_CELL_CODEC_LPCM_ADAPTER_H
#define PS3TC_CELL_CODEC_LPCM_ADAPTER_H

#include <cell/error.h>

#define CELL_ADEC_ERROR_LPCM_FATAL                   CELL_ERROR_CAST(0x80612001)
#define CELL_ADEC_ERROR_LPCM_SEQ                     CELL_ERROR_CAST(0x80612002)
#define CELL_ADEC_ERROR_LPCM_ARG                     CELL_ERROR_CAST(0x80612003)
#define CELL_ADEC_ERROR_LPCM_BUSY                    CELL_ERROR_CAST(0x80612004)
#define CELL_ADEC_ERROR_LPCM_EMPTY                   CELL_ERROR_CAST(0x80612005)

#endif /* PS3TC_CELL_CODEC_LPCM_ADAPTER_H */
