/* cell/codec/types.h — shared types for the codec subsystem.
 *
 * Clean-room header.  CellCodecTimeStamp is the 64-bit split
 * timestamp the cell SDK codec ABI uses (upper 32 + lower 32).
 * CellCodecEsFilterId identifies an elementary-stream codec via
 * a two-tier major/minor filter ID with supplemental info.
 */
#ifndef __PS3DK_CELL_CODEC_TYPES_H__
#define __PS3DK_CELL_CODEC_TYPES_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_CODEC_PTS_INVALID  0xffffffff
#define CELL_CODEC_DTS_INVALID  0xffffffff

typedef struct {
    uint32_t upper;
    uint32_t lower;
} CellCodecTimeStamp;

typedef struct {
    uint32_t filterIdMajor;
    uint32_t filterIdMinor;
    uint32_t supplementalInfo1;
    uint32_t supplementalInfo2;
} CellCodecEsFilterId;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CODEC_TYPES_H__ */
