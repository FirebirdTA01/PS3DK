/* cell/codec/adec_base.h — cellAdec base types and error codes.
 *
 * Clean-room header.  Provides the CELL_ADEC_ERROR_* constants,
 * CELL_ADEC_MEM_ALIGN, and the PTS invalid sentinel reused from
 * cell/codec/types.h.
 */
#ifndef __PS3DK_CELL_CODEC_ADEC_BASE_H__
#define __PS3DK_CELL_CODEC_ADEC_BASE_H__

#include <cell/error.h>
#include <cell/codec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CELL_ERROR_FACILITY_CODEC = 0x061
 * libadec range: 0x8061_0001 – 0x8061_00ff
 */

#define CELL_ADEC_ERROR_FATAL  CELL_ERROR_CAST(0x80610001)
#define CELL_ADEC_ERROR_SEQ    CELL_ERROR_CAST(0x80610002)
#define CELL_ADEC_ERROR_ARG    CELL_ERROR_CAST(0x80610003)
#define CELL_ADEC_ERROR_BUSY   CELL_ERROR_CAST(0x80610004)
#define CELL_ADEC_ERROR_EMPTY  CELL_ERROR_CAST(0x80610005)

#define CELL_ADEC_MEM_ALIGN  128

#define CELL_ADEC_PTS_INVALID  CELL_CODEC_PTS_INVALID

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CODEC_ADEC_BASE_H__ */
