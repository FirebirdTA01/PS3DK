/*
 * PS3 Custom Toolchain — sdk/include/cell/gcm/gcm_spu_prepare.h
 *
 * PPU side of the SPU command writer (<cell/gcm/gcm_spu_cmd.h> on the SPU):
 * resolves the Cg-handle calls once on the PPU into plain blocks an SPU
 * job can DMA in and pass to the same-named SPU calls.
 *
 *   cellGcmSpuPrepareVertexProgram     the exact words cellGcmSetVertexProgram
 *                                      writes, for the SPU cellGcmSetVertexProgram
 *   cellGcmSpuPrepareFragmentProgram   likewise for cellGcmSetFragmentProgram
 *                                      at a fixed ucode offset
 *   cellGcmSpuPrepareVertexParameter   register + word count for
 *                                      cellGcmSetVertexProgramParameter
 *   cellGcmSpuPrepareFragmentParameter the ucode patch list for
 *                                      cellGcmSetFragmentProgramParameter
 *
 * The structs are the ones in the SPU header; both sides use only 32-bit
 * fields, so a block DMAs across unchanged.  Static inline: needs
 * <cell/gcm.h> (or, in a host test, gcm_cg_bridge.h) first.
 */
#ifndef PS3TC_CELL_GCM_SPU_PREPARE_H
#define PS3TC_CELL_GCM_SPU_PREPARE_H

#ifndef PS3TC_CELL_GCM_CG_BRIDGE_H
#include <cell/gcm.h>
#endif
#include <stdint.h>
#include <string.h>
#include <sys/lv2_types.h>

/* The SPU header's shared types, without its function declarations. */
#ifndef PS3TC_CELL_GCM_SPU_CMD_H
#define CELL_GCM_SPU_FP_PARAMETER_MAX_PATCHES 32
typedef struct CellGcmSpuVertexParameter {
    uint32_t resIndex;
    uint32_t words;
} CellGcmSpuVertexParameter;
typedef struct CellGcmSpuFragmentParameter {
    uint32_t patchCount;
    struct { uint32_t ucodeOffset; uint16_t valueIndex; uint16_t words; }
        patch[CELL_GCM_SPU_FP_PARAMETER_MAX_PATCHES];
} CellGcmSpuFragmentParameter;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A capture context: the callback refuses and marks the capture failed,
 * so a call that does not fit, even partly, is reported. */
typedef struct { CellGcmContextData ctx; uint32_t refused; } Ps3tcSpuCapture;

static inline int32_t ps3tc_spu_prepare_refuse(CellGcmContextData *ctx, uint32_t count)
{
    (void)count;
    ((Ps3tcSpuCapture *)(void *)ctx)->refused = 1;
    return -1;
}

static inline int32_t ps3tc_spu_prepare_capture(Ps3tcSpuCapture *c, uint32_t *out, uint32_t maxWords)
{
    if (!out || maxWords < 2u) return -1;
    c->ctx.begin = c->ctx.current = out;
    c->ctx.end = out + maxWords;     /* every word usable: no RETURN here */
    c->ctx.callback = (CellGcmContextCallback)(uintptr_t)lv2_fn_to_callback_ea((const void *)ps3tc_spu_prepare_refuse);
    c->refused = 0;
    return 0;
}

/* Words cellGcmSetVertexProgram(ctx, prog, ucode) writes, into out
 * (at most maxWords).  CELL_OK and *words, or -1 if it did not fit. */
static inline int32_t cellGcmSpuPrepareVertexProgram(CGprogram prog, void *ucode, uint32_t *out,
                                                     uint32_t maxWords, uint32_t *words)
{
    Ps3tcSpuCapture c;
    if (!prog || ps3tc_spu_prepare_capture(&c, out, maxWords)) return -1;
    cellGcmSetVertexProgram(&c.ctx, prog, ucode);
    if (c.refused || c.ctx.current == c.ctx.begin) return -1;
    if (words) *words = (uint32_t)(c.ctx.current - c.ctx.begin);
    return 0;
}

static inline int32_t cellGcmSpuPrepareFragmentProgram(CGprogram prog, uint32_t offset, uint32_t *out,
                                                       uint32_t maxWords, uint32_t *words)
{
    Ps3tcSpuCapture c;
    if (!prog || ps3tc_spu_prepare_capture(&c, out, maxWords)) return -1;
    cellGcmSetFragmentProgram(&c.ctx, prog, offset);
    if (c.refused || c.ctx.current == c.ctx.begin) return -1;
    if (words) *words = (uint32_t)(c.ctx.current - c.ctx.begin);
    return 0;
}

/* -1 for a parameter with no register (the PPU call writes nothing). */
static inline int32_t cellGcmSpuPrepareVertexParameter(CGparameter param, CellGcmSpuVertexParameter *out)
{
    const CgBinaryParameter *pp = (const CgBinaryParameter *)param;
    if (!pp || !out || pp->resIndex < 0) return -1;
    out->resIndex = (uint32_t)pp->resIndex;
    out->words = ps3tc_cg_rows_for_type(pp->type) * 4u;
    return 0;
}

/* The patches cellGcmSetFragmentProgramParameter would write, in its
 * order.  -1 if there are more than CELL_GCM_SPU_FP_PARAMETER_MAX_PATCHES;
 * a parameter with no embedded constants gives patchCount 0. */
static inline int32_t cellGcmSpuPrepareFragmentParameter(CGprogram prog, CGparameter param,
                                                         CellGcmSpuFragmentParameter *out)
{
    const CgBinaryParameter *pp = (const CgBinaryParameter *)param;
    uint32_t n = 0;
    if (!prog || !pp || !out) return -1;
    const uint32_t type = (uint32_t)pp->type;
    const int is_matrix = ((type >= 1049u && type <= 1064u) || (type >= 1029u && type <= 1044u))
                          && pp->res == 0x0CB8u;
    unsigned rows = is_matrix ? ps3tc_cg_rows_for_type(pp->type) : 1u;
    const CgBinaryParameter *leaf = is_matrix ? pp + 1 : pp;
    for (unsigned r = 0; r < rows; ++r, ++leaf) {
        if (is_matrix && leaf->var != 0x1006u) break;
        if (!leaf->embeddedConst) { if (is_matrix) continue; break; }
        const uint32_t *embed = (const uint32_t *)((const uint8_t *)prog + leaf->embeddedConst);
        for (uint32_t i = 1; i <= embed[0]; ++i) {
            if (n == CELL_GCM_SPU_FP_PARAMETER_MAX_PATCHES) return -1;
            out->patch[n].ucodeOffset = embed[i];
            out->patch[n].valueIndex = (uint16_t)(is_matrix ? r * 4u : 0u);
            out->patch[n].words = (uint16_t)(is_matrix ? 4u : ps3tc_cg_cols_for_type(pp->type));
            ++n;
        }
    }
    out->patchCount = n;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_GCM_SPU_PREPARE_H */
