/*
 * PS3 Custom Toolchain — sdk/include-spu/cell/gcm/gcm_spu_cmd.h
 *
 * SPU-side GCM command writer (libgcm_spu_cmd.a).  An SPU job builds RSX
 * method words into a local-store buffer with the same calls a PPU makes
 * into a cellGcm context, then DMAs the finished buffer to a main-memory
 * command segment that the PPU runs with cellGcmSetCallCommand.
 *
 *   static uint32_t ls[4096] __attribute__((aligned(128)));
 *   CellGcmContextData c;
 *   cellGcmSpuSetupContextData(&c, ls, sizeof(ls));
 *   cellGcmSetClearColor(&c, 0xff2040c0);
 *   cellGcmSetClearSurface(&c, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B);
 *   if (cellGcmSpuEnd(&c) != CELL_OK) ...           RETURN appended; overflow refused
 *   cellGcmSpuPutSegment(&c, segment_ea, tag);       DMA out, waits for the tag
 *
 * Names and arguments mirror the PPU cellGcm* calls; each emits the same
 * words as its PPU counterpart (tests/sdk/gcm-spu-cmd-test.sh checks every
 * one byte for byte).  Calls that take a Cg handle on the PPU take a block
 * the PPU prepared instead (<cell/gcm/gcm_spu_prepare.h>), because the SPU
 * cannot read the program: cellGcmSetVertexProgram / SetFragmentProgram
 * copy prepared words, cellGcmSetVertexProgramParameter and
 * SetFragmentProgramParameter take a resolved parameter.
 *
 * Every call reserves its whole command at once: it is written completely
 * or not at all.  A command that does not fit is refused and its words are
 * added to the overflow count, so cellGcmSpuEnd reports OVERFLOW instead of
 * the buffer ever wrapping.  A measuring context (begin == NULL) writes
 * nothing and only counts, which gives the exact size of a command list.
 * The counts saturate at 0xffffffff rather than wrap, so a refusal is
 * never cancelled: once the overflow count is non-zero, End reports
 * OVERFLOW.  A measured size of 0xffffffff bytes means "does not fit".
 *
 * The PSL1GHT spellings listed at the end are thin aliases of these calls.
 */
#ifndef PS3TC_CELL_GCM_SPU_CMD_H
#define PS3TC_CELL_GCM_SPU_CMD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host builds of the writer (the encoding tests) rename every symbol so the
 * PPU emitters can be linked into the same program. */
#ifndef CELL_GCM_SPU_FN
#define CELL_GCM_SPU_FN(name) cellGcm##name
#endif
#ifndef CELL_GCM_SPU_RSX
#define CELL_GCM_SPU_RSX(name) rsx##name
#endif

#ifndef CELL_OK
#define CELL_OK 0
#endif
#define CELL_GCM_SPU_ERROR_OVERFLOW  ((int32_t)0x802b0f05)  /* = CELL_GCM_SEGMENT_ERROR_OVERFLOW */
#define CELL_GCM_SPU_ERROR_INVALID   ((int32_t)0x802b0f01)
#define CELL_GCM_SPU_ERROR_STATE     ((int32_t)0x802b0f04)

typedef struct CellGcmSpuContextData {
    uint32_t *begin;          /* local-store buffer; NULL for a measuring context */
    uint32_t *end;            /* last word: kept for the RETURN cellGcmSpuEnd adds */
    uint32_t *current;
    uint32_t measuredWords;   /* measuring context: words the calls would write    */
    uint32_t overflowWords;   /* words refused because they did not fit (saturates) */
    uint32_t ended;           /* CELL_GCM_SPU_CONTEXT_*                             */
} CellGcmSpuContextData;

/* CellGcmSpuContextData.ended */
#define CELL_GCM_SPU_CONTEXT_OPEN    0u  /* calls are written                             */
#define CELL_GCM_SPU_CONTEXT_ENDED   1u  /* cellGcmSpuEnd appended the RETURN              */
#define CELL_GCM_SPU_CONTEXT_NO_ROOM 2u  /* buffer under 4 bytes: everything is refused   */

/* Layout-identical to the PPU CellGcmTexture (24 bytes), so the PPU can
 * hand its texture records to the SPU unchanged. */
typedef struct CellGcmSpuTexture {
    uint8_t  format, mipmap, dimension, cubemap;
    uint32_t remap;
    uint16_t width, height, depth;
    uint8_t  location, _padding;
    uint32_t pitch;
    uint32_t offset;
} CellGcmSpuTexture;

/* A vertex-program uniform resolved on the PPU: register and word count
 * (4 per row).  cellGcmSpuPrepareVertexParameter fills it. */
typedef struct CellGcmSpuVertexParameter {
    uint32_t resIndex;
    uint32_t words;
} CellGcmSpuVertexParameter;

/* A fragment-program uniform resolved on the PPU: one entry per patch of
 * the program's embedded constants, in the order the PPU call writes them.
 * Each patch copies `words` values starting at values[valueIndex] into
 * the ucode at addrOffset + ucodeOffset. */
#define CELL_GCM_SPU_FP_PARAMETER_MAX_PATCHES 32
typedef struct CellGcmSpuFragmentParameter {
    uint32_t patchCount;
    struct { uint32_t ucodeOffset; uint16_t valueIndex; uint16_t words; }
        patch[CELL_GCM_SPU_FP_PARAMETER_MAX_PATCHES];
} CellGcmSpuFragmentParameter;

#ifdef __SPU__
typedef CellGcmSpuContextData CellGcmContextData;
typedef CellGcmSpuTexture CellGcmTexture;
#endif

/* ---- context ---------------------------------------------------------- */

/* Writing context over `sizeBytes` (a multiple of 4; of 16 for
 * cellGcmSpuPutSegment) of local store;
 * the last word is kept for the RETURN.  Under 4 bytes there is no room
 * for it: every call is refused, the overflow count starts at 1 and End
 * returns OVERFLOW without writing. */
void CELL_GCM_SPU_FN(SpuSetupContextData)(CellGcmSpuContextData *ctx, uint32_t *buffer, uint32_t sizeBytes);
/* Measuring context: calls only count. */
void CELL_GCM_SPU_FN(SpuSetupMeasureContext)(CellGcmSpuContextData *ctx);
/* Bytes written so far (measuring: bytes the calls would write), without
 * the RETURN.  Add 4 for the RETURN cellGcmSpuEnd appends.  0xffffffff
 * when the count does not fit in 32 bits. */
uint32_t CELL_GCM_SPU_FN(SpuGetUsedBytes)(const CellGcmSpuContextData *ctx);
uint32_t CELL_GCM_SPU_FN(SpuGetOverflowWords)(const CellGcmSpuContextData *ctx);
/* Append the RETURN.  CELL_GCM_SPU_ERROR_OVERFLOW if any call was refused. */
int32_t CELL_GCM_SPU_FN(SpuEnd)(CellGcmSpuContextData *ctx);
/* Reserve `words` words for a caller-built command (NULL: refused/measuring). */
uint32_t *CELL_GCM_SPU_FN(SpuReserve)(CellGcmSpuContextData *ctx, uint32_t words);

#ifdef __SPU__
/* DMA the ended buffer (rounded up to 16 bytes) to `ea`, which must be
 * 16-byte aligned, and wait for `tag`.  CELL_OK, or STATE if not ended,
 * or the End error. */
int32_t cellGcmSpuPutSegment(const CellGcmSpuContextData *ctx, uint64_t ea, uint32_t tag);
#endif

/* ---- commands (PPU cellGcm* names and arguments) ----------------------- */

void CELL_GCM_SPU_FN(SetNopCommand)(CellGcmSpuContextData *ctx, uint32_t count);
void CELL_GCM_SPU_FN(SetJumpCommand)(CellGcmSpuContextData *ctx, uint32_t offset);
void CELL_GCM_SPU_FN(SetCallCommand)(CellGcmSpuContextData *ctx, uint32_t offset);
void CELL_GCM_SPU_FN(SetReturnCommand)(CellGcmSpuContextData *ctx);
void CELL_GCM_SPU_FN(SetWriteBackEndLabel)(CellGcmSpuContextData *ctx, uint8_t index, uint32_t value);
void CELL_GCM_SPU_FN(SetWriteCommandLabel)(CellGcmSpuContextData *ctx, uint8_t index, uint32_t value);
void CELL_GCM_SPU_FN(SetWaitLabel)(CellGcmSpuContextData *ctx, uint8_t index, uint32_t value);

void CELL_GCM_SPU_FN(SetClearColor)(CellGcmSpuContextData *ctx, uint32_t color);
void CELL_GCM_SPU_FN(SetClearSurface)(CellGcmSpuContextData *ctx, uint32_t mask);
void CELL_GCM_SPU_FN(SetScissor)(CellGcmSpuContextData *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void CELL_GCM_SPU_FN(SetViewport)(CellGcmSpuContextData *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                  float min, float max, const float scale[4], const float offset[4]);
void CELL_GCM_SPU_FN(SetColorMask)(CellGcmSpuContextData *ctx, uint32_t mask);
void CELL_GCM_SPU_FN(SetColorMaskMrt)(CellGcmSpuContextData *ctx, uint32_t mask);
void CELL_GCM_SPU_FN(SetBlendEnable)(CellGcmSpuContextData *ctx, uint32_t enable);
void CELL_GCM_SPU_FN(SetBlendFunc)(CellGcmSpuContextData *ctx, uint16_t sfcolor, uint16_t dfcolor,
                                   uint16_t sfalpha, uint16_t dfalpha);
void CELL_GCM_SPU_FN(SetBlendEquation)(CellGcmSpuContextData *ctx, uint16_t color, uint16_t alpha);
void CELL_GCM_SPU_FN(SetDepthTestEnable)(CellGcmSpuContextData *ctx, uint32_t enable);
void CELL_GCM_SPU_FN(SetDepthMask)(CellGcmSpuContextData *ctx, uint32_t enable);
void CELL_GCM_SPU_FN(SetDepthFunc)(CellGcmSpuContextData *ctx, uint32_t func);
void CELL_GCM_SPU_FN(SetCullFaceEnable)(CellGcmSpuContextData *ctx, uint32_t enable);
void CELL_GCM_SPU_FN(SetFrontFace)(CellGcmSpuContextData *ctx, uint32_t dir);
void CELL_GCM_SPU_FN(SetPolygonOffsetFillEnable)(CellGcmSpuContextData *ctx, uint32_t enable);
void CELL_GCM_SPU_FN(SetPolygonOffset)(CellGcmSpuContextData *ctx, float factor, float units);

void CELL_GCM_SPU_FN(SetVertexDataArray)(CellGcmSpuContextData *ctx, uint8_t index, uint16_t frequency,
                                         uint8_t stride, uint8_t size, uint8_t type, uint8_t location,
                                         uint32_t offset);
void CELL_GCM_SPU_FN(SetVertexData4f)(CellGcmSpuContextData *ctx, uint8_t index, const float v[4]);
void CELL_GCM_SPU_FN(SetFrequencyDividerOperation)(CellGcmSpuContextData *ctx, uint16_t op);
void CELL_GCM_SPU_FN(SetInvalidateVertexCache)(CellGcmSpuContextData *ctx);
void CELL_GCM_SPU_FN(SetDrawArrays)(CellGcmSpuContextData *ctx, uint32_t mode, uint32_t first, uint32_t count);
void CELL_GCM_SPU_FN(SetDrawIndexArray)(CellGcmSpuContextData *ctx, uint8_t mode, uint32_t count,
                                        uint8_t type, uint8_t location, uint32_t indices);

void CELL_GCM_SPU_FN(SetVertexProgramConstants)(CellGcmSpuContextData *ctx, uint32_t start, uint32_t count,
                                                const float *data);
void CELL_GCM_SPU_FN(SetVertexProgramParameterBlock)(CellGcmSpuContextData *ctx, uint32_t baseConst,
                                                     uint32_t constCount, const float *values);
void CELL_GCM_SPU_FN(SetVertexProgramParameter)(CellGcmSpuContextData *ctx,
                                                const CellGcmSpuVertexParameter *param, const float *values);
void CELL_GCM_SPU_FN(SetVertexProgram)(CellGcmSpuContextData *ctx, const uint32_t *preparedWords,
                                       uint32_t wordCount);
void CELL_GCM_SPU_FN(SetFragmentProgram)(CellGcmSpuContextData *ctx, const uint32_t *preparedWords,
                                         uint32_t wordCount);
void CELL_GCM_SPU_FN(SetFragmentProgramParameter)(CellGcmSpuContextData *ctx,
                                                  const CellGcmSpuFragmentParameter *param,
                                                  const float *values, uint32_t addrOffset);
void CELL_GCM_SPU_FN(SetUpdateFragmentProgramParameter)(CellGcmSpuContextData *ctx, uint32_t offset);

void CELL_GCM_SPU_FN(SetTexture)(CellGcmSpuContextData *ctx, uint8_t index, const CellGcmSpuTexture *texture);
void CELL_GCM_SPU_FN(SetTextureControl)(CellGcmSpuContextData *ctx, uint8_t index, uint32_t enable,
                                        uint16_t minlod, uint16_t maxlod, uint8_t maxaniso);
void CELL_GCM_SPU_FN(SetTextureFilter)(CellGcmSpuContextData *ctx, uint8_t index, uint16_t bias,
                                       uint8_t min, uint8_t mag, uint8_t conv);
void CELL_GCM_SPU_FN(SetTextureAddress)(CellGcmSpuContextData *ctx, uint8_t index, uint8_t wraps,
                                        uint8_t wrapt, uint8_t wrapr, uint8_t unsignedRemap,
                                        uint8_t zfunc, uint8_t gamma);
void CELL_GCM_SPU_FN(SetInvalidateTextureCache)(CellGcmSpuContextData *ctx, uint32_t type);

/* ---- PSL1GHT spellings: thin aliases of the calls above ----------------- */

static inline void CELL_GCM_SPU_RSX(SetBlendFunc)(CellGcmSpuContextData *c, uint16_t sfcolor, uint16_t dfcolor,
                                                  uint16_t sfalpha, uint16_t dfalpha)
{ CELL_GCM_SPU_FN(SetBlendFunc)(c, sfcolor, dfcolor, sfalpha, dfalpha); }
static inline void CELL_GCM_SPU_RSX(SetBlendEquation)(CellGcmSpuContextData *c, uint16_t color, uint16_t alpha)
{ CELL_GCM_SPU_FN(SetBlendEquation)(c, color, alpha); }
static inline void CELL_GCM_SPU_RSX(SetScissor)(CellGcmSpuContextData *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ CELL_GCM_SPU_FN(SetScissor)(c, x, y, w, h); }
static inline void CELL_GCM_SPU_RSX(LoadTexture)(CellGcmSpuContextData *c, uint8_t index, const CellGcmSpuTexture *t)
{ CELL_GCM_SPU_FN(SetTexture)(c, index, t); }
static inline void CELL_GCM_SPU_RSX(TextureControl)(CellGcmSpuContextData *c, uint8_t index, uint32_t enable,
                                                    uint16_t minlod, uint16_t maxlod, uint8_t maxaniso)
{ CELL_GCM_SPU_FN(SetTextureControl)(c, index, enable, minlod, maxlod, maxaniso); }
static inline void CELL_GCM_SPU_RSX(TextureFilter)(CellGcmSpuContextData *c, uint8_t index, uint16_t bias,
                                                   uint8_t min, uint8_t mag, uint8_t conv)
{ CELL_GCM_SPU_FN(SetTextureFilter)(c, index, bias, min, mag, conv); }
static inline void CELL_GCM_SPU_RSX(TextureWrapMode)(CellGcmSpuContextData *c, uint8_t index, uint8_t wraps,
                                                     uint8_t wrapt, uint8_t wrapr, uint8_t unsignedRemap,
                                                     uint8_t zfunc, uint8_t gamma)
{ CELL_GCM_SPU_FN(SetTextureAddress)(c, index, wraps, wrapt, wrapr, unsignedRemap, zfunc, gamma); }
static inline void CELL_GCM_SPU_RSX(InvalidateTextureCache)(CellGcmSpuContextData *c, uint32_t type)
{ CELL_GCM_SPU_FN(SetInvalidateTextureCache)(c, type); }
/* PSL1GHT argument order: (type, offset, count, data_type, location). */
static inline void CELL_GCM_SPU_RSX(DrawIndexArray)(CellGcmSpuContextData *c, uint8_t type, uint32_t offset,
                                                    uint32_t count, uint8_t dataType, uint8_t location)
{ CELL_GCM_SPU_FN(SetDrawIndexArray)(c, type, count, dataType, location, offset); }

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_GCM_SPU_CMD_H */
