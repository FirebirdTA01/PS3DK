/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_cg_bridge.h>
 *
 * Native RSX command emitter for Sony's cellGcmSetVertexProgram /
 * cellGcmSetFragmentProgram / cellGcmSetVertexProgramParameter surface.
 * Reads CgBinaryProgram blobs (sce-cgc output) directly and writes NV40
 * command words to the current gcm command buffer — no PSL1GHT
 * rsx{Vertex,Fragment}Program intermediate, no rsxLoad* forwarding.
 *
 * Sce-cgc output is the source of truth; the hardware register encoding
 * is derived here from the parameter-table semantics and container
 * fields rather than being consumed from PSL1GHT-style struct fields.
 * When PSL1GHT's librsx is eventually replaced wholesale, the only
 * piece of this file that changes is which gcmContextData / NV40
 * method macros we include — the command sequences stay put.
 *
 * RSX command-buffer primer: writes are (method-header, arg0, arg1, …)
 * where the header packs the NV40 method offset with a count of args
 * following.  The method auto-increments by 4 bytes per subsequent
 * arg unless the NO_INCREMENT bit is set, so we can hand it N=count
 * data words and let the hardware fan them out to consecutive methods.
 */

#ifndef PS3TC_CELL_GCM_CG_BRIDGE_H
#define PS3TC_CELL_GCM_CG_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include <Cg/cg.h>
#include <Cg/cgBinary.h>
#include <rsx/gcm_sys.h>           /* gcmContextData, GCM_LOCATION_*, GCM_ATTRIB_OUTPUT_MASK_* */
#include <rsx/nv40.h>              /* NV40TCL_* method IDs */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- command-buffer primitives ------------------------------------ */
/*
 * Build an RSX method-header word.  The NV40 command-buffer format
 * packs (count << 18) | method_offset; each subsequent data word is
 * written to (method_offset + 4 * i) automatically.
 */
#define PS3TC_GCM_METHOD(m, n)  (((uint32_t)(n) << 18) | (uint32_t)(m))

/* Reserve `count` u32 slots in the command buffer, invoking the
 * gcm callback (which grows the buffer or flushes upstream) when we
 * run up against the current end.  Returns the starting write pointer
 * or NULL on callback failure. */
static inline uint32_t *ps3tc_gcm_reserve(CellGcmContextData *ctx, uint32_t count)
{
    if ((ctx->current + count) > ctx->end)
    {
        if (ctx->callback && ctx->callback(ctx, count) != 0)
            return 0;
    }
    uint32_t *p = ctx->current;
    ctx->current += count;
    return p;
}

/* ---- VP result-mask synthesis ------------------------------------- */
/*
 * sce-cgc writes a compressed / user-visible summary into
 * CgBinaryVertexProgram.attributeOutputMask that doesn't line up bit-
 * for-bit with NV40TCL_VP_RESULT_EN.  We rebuild the hardware mask
 * from the parameter table: walk CgBinaryParameter entries with
 * direction = CG_OUT / CG_INOUT, read each one's semantic string, and
 * flip the NV40 bit that the hardware uses for that output channel.
 *
 * NV40 VP_RESULT_EN bit layout (values observed in PSL1GHT's
 * compilervp.cpp emit_dst against the matching method):
 *
 *   bit 0      HPOS            (vertex shader must always output)
 *   bit 0+2    COL0            (dual-bit signal — 0x5 for COL0)
 *   bit 1+3    COL1            (dual-bit — 0xA)
 *   bit 0+2    BFC0            (front/back-face alias of COL0)
 *   bit 1+3    BFC1            (alias of COL1)
 *   bit 4      FOG / FOGC
 *   bit 5      PSIZE / PSZ      (we unconditionally enable via
 *                                GCM_ATTRIB_OUTPUT_MASK_POINTSIZE
 *                                when building the final register
 *                                value — matches the PSL1GHT convention
 *                                of always permitting point-size)
 *   bit 14+n   TEXCOORD<n>      (0x4000 << n, for n = 0 .. 7)
 */
static inline uint32_t ps3tc_cg_vp_result_en(const CgBinaryProgram *p)
{
    if (!p) return 1u;
    const CgBinaryParameter *params =
        (const CgBinaryParameter *)((const uint8_t *)p + p->parameterArray);

    uint32_t mask = 1u;  /* HPOS always enabled */

    for (uint32_t i = 0; i < p->parameterCount; ++i)
    {
        const CgBinaryParameter *pp = &params[i];
        if (pp->direction != CG_OUT && pp->direction != CG_INOUT) continue;
        if (!pp->semantic) continue;

        const char *sem = (const char *)((const uint8_t *)p + pp->semantic);
        if (!sem || !*sem) continue;

        if (strncmp(sem, "COLOR", 5) == 0 || strncmp(sem, "COL", 3) == 0)
        {
            const int n   = (strncmp(sem, "COLOR", 5) == 0) ? 5 : 3;
            const int idx = (sem[n] >= '0' && sem[n] <= '9') ? (sem[n] - '0') : 0;
            mask |= (1u << idx) | (4u << idx);
        }
        else if (strncmp(sem, "TEXCOORD", 8) == 0)
        {
            const int idx = (sem[8] >= '0' && sem[8] <= '9') ? (sem[8] - '0') : 0;
            mask |= (0x4000u << idx);
        }
        else if (strncmp(sem, "FOG", 3) == 0)
        {
            mask |= (1u << 4);
        }
        else if (strncmp(sem, "PSIZE", 5) == 0 || strncmp(sem, "PSZ", 3) == 0)
        {
            mask |= (1u << 5);
        }
    }
    return mask;
}

/* -------------------------------------------------------------------- *
 * Vertex-program bind — loads ucode to VP instruction memory and sets
 * the VP_ATTRIB_EN / VP_RESULT_EN / TRANSFORM_TIMEOUT state that must
 * accompany a new program.
 * -------------------------------------------------------------------- */
static inline void cellGcmSetVertexProgram(CellGcmContextData *ctx,
                                           CGprogram prog, void *ucode)
{
    if (!ctx || !prog) return;

    const CgBinaryProgram *p = (const CgBinaryProgram *)prog;
    const CgBinaryVertexProgram *vp =
        (const CgBinaryVertexProgram *)((const uint8_t *)p + p->program);

    const uint32_t inst_count = vp->instructionCount;
    const uint32_t inst_start = vp->instructionSlot;
    const uint32_t loop       = inst_count >> 3;             /* full 8-insn chunks */
    const uint32_t rest       = (inst_count & 7u) * 4u;      /* remainder words */

    /* Reservation: UPLOAD_FROM_ID hdr + 2 args                     = 3 words,
     * per loop iter: UPLOAD_INST hdr + 32 data words               = 33 words,
     * remainder:    UPLOAD_INST hdr + rest data words              = 1+rest,
     * VP_ATTRIB_EN hdr + arg                                       = 2 words,
     * VP_RESULT_EN hdr + arg                                       = 2 words,
     * TRANSFORM_TIMEOUT hdr + arg                                  = 2 words. */
    const uint32_t reserve = 3u + loop * 33u
                           + (rest ? (1u + rest) : 0u)
                           + 2u + 2u + 2u;

    uint32_t *w = ps3tc_gcm_reserve(ctx, reserve);
    if (!w) return;

    const uint32_t *src = (const uint32_t *)ucode;

    /* Program the start instruction ID (both args are the start index
     * — the NV40 wants the same value twice). */
    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_FROM_ID, 2);
    *w++ = inst_start;
    *w++ = inst_start;

    /* Upload 8 instructions (32 words) per iteration. */
    for (uint32_t i = 0; i < loop; ++i)
    {
        *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_INST(0), 32);
        memcpy(w,      src,      16 * sizeof(uint32_t));
        memcpy(w + 16, src + 16, 16 * sizeof(uint32_t));
        w   += 32;
        src += 32;
    }
    if (rest)
    {
        *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_INST(0), rest);
        memcpy(w, src, rest * sizeof(uint32_t));
        w += rest;
    }

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_ATTRIB_EN, 1);
    *w++ = vp->attributeInputMask;

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_RESULT_EN, 1);
    *w++ = ps3tc_cg_vp_result_en(p) | GCM_ATTRIB_OUTPUT_MASK_POINTSIZE;

    /* Transform-shader timeout watchdog.  Value is a function of
     * temp-register count — programs using 0..32 temps get the short
     * timeout, programs using 33+ get the longer one. */
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TRANSFORM_TIMEOUT, 1);
    *w++ = (vp->registerCount <= 32u) ? 0x0020FFFFu : 0x0030FFFFu;
}

/* -------------------------------------------------------------------- *
 * Fragment-program bind — the ucode itself is already sitting in RSX
 * memory at `offset` (caller's responsibility); we only update the
 * FP_ADDRESS / FP_CONTROL / TEX_COORD_CONTROL state.
 * -------------------------------------------------------------------- */
static inline void cellGcmSetFragmentProgram(CellGcmContextData *ctx,
                                             CGprogram prog, uint32_t offset)
{
    if (!ctx || !prog) return;

    const CgBinaryProgram *p = (const CgBinaryProgram *)prog;
    const CgBinaryFragmentProgram *fp =
        (const CgBinaryFragmentProgram *)((const uint8_t *)p + p->program);

    /* Bind the FP ucode address.  The lower 29 bits carry the byte
     * offset within RSX memory; bit 0 of the header-byte carries
     * location (LOCAL vs RSX) — PSL1GHT writes (location+1) | offset
     * which matches Sony's convention. */
    {
        uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
        if (!w) return;
        *w++ = PS3TC_GCM_METHOD(NV40TCL_FP_ADDRESS, 1);
        *w++ = ((uint32_t)GCM_LOCATION_RSX + 1u) | (offset & 0x1fffffffu);
    }

    /* One TEX_COORD_CONTROL write per texcoord the FP actually reads.
     * CgBinaryFragmentProgram exposes texCoordsInputMask + texCoords2D
     * but not a separate 3D mask; we default 3D to 0 and revisit when
     * cube / volume-texture samples surface the gap. */
    {
        uint32_t texc  = fp->texCoordsInputMask;
        uint32_t tc2d  = fp->texCoords2D;
        for (int i = 0; texc; ++i)
        {
            if (texc & 1u)
            {
                uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
                if (!w) return;
                *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_COORD_CONTROL(i), 1);
                *w++ = (tc2d & 1u);       /* (tc3d<<4) | tc2d; tc3d = 0 */
            }
            texc >>= 1;
            tc2d >>= 1;
        }
    }

    /* FP_CONTROL:
     *   bits 0-7 : low-byte flags (output-from-H0 = 0x0e, R0 = 0x40,
     *              KIL at bit 7)
     *   bit  10  : "program valid / enable" — must be set for the FP
     *              to actually execute (observed in PSL1GHT's
     *              LoadFragmentProgramLocation write)
     *   bits 24-31: number of temps (register-count, clamped ≥ 2 —
     *              NV40 requires a minimum of 2 allocated temps) */
    {
        const uint32_t num_regs = (fp->registerCount > 2)
                                ? fp->registerCount : 2u;

        uint32_t low = fp->outputFromH0 ? 0x0eu : 0x40u;
        if (fp->pixelKill) low |= (1u << 7);

        uint32_t fpcontrol = low
                           | (1u << 10)
                           | (num_regs << 24);

        uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
        if (!w) return;
        *w++ = PS3TC_GCM_METHOD(NV40TCL_FP_CONTROL, 1);
        *w++ = fpcontrol;
    }
}

/* -------------------------------------------------------------------- *
 * Uniform upload — writes `rows` four-float rows to the VP constant
 * bank starting at the parameter's resource index.
 * -------------------------------------------------------------------- */
static inline unsigned ps3tc_cg_rows_for_type(uint32_t type)
{
    switch (type)
    {
    case CG_FLOAT4x4: case CG_FLOAT4x3: case CG_FLOAT4x2: case CG_FLOAT4x1:
    case CG_HALF4x4:  case CG_HALF4x3:  case CG_HALF4x2:  case CG_HALF4x1:
    case CG_FIXED4x4: case CG_FIXED4x3: case CG_FIXED4x2: case CG_FIXED4x1:
    case CG_INT4x4:   case CG_INT4x3:   case CG_INT4x2:   case CG_INT4x1:
    case CG_BOOL4x4:  case CG_BOOL4x3:  case CG_BOOL4x2:  case CG_BOOL4x1:
        return 4;
    case CG_FLOAT3x4: case CG_FLOAT3x3: case CG_FLOAT3x2: case CG_FLOAT3x1:
    case CG_HALF3x4:  case CG_HALF3x3:  case CG_HALF3x2:  case CG_HALF3x1:
    case CG_FIXED3x4: case CG_FIXED3x3: case CG_FIXED3x2: case CG_FIXED3x1:
    case CG_INT3x4:   case CG_INT3x3:   case CG_INT3x2:   case CG_INT3x1:
    case CG_BOOL3x4:  case CG_BOOL3x3:  case CG_BOOL3x2:  case CG_BOOL3x1:
        return 3;
    case CG_FLOAT2x4: case CG_FLOAT2x3: case CG_FLOAT2x2: case CG_FLOAT2x1:
    case CG_HALF2x4:  case CG_HALF2x3:  case CG_HALF2x2:  case CG_HALF2x1:
    case CG_FIXED2x4: case CG_FIXED2x3: case CG_FIXED2x2: case CG_FIXED2x1:
    case CG_INT2x4:   case CG_INT2x3:   case CG_INT2x2:   case CG_INT2x1:
    case CG_BOOL2x4:  case CG_BOOL2x3:  case CG_BOOL2x2:  case CG_BOOL2x1:
        return 2;
    default:
        return 1;
    }
}

static inline void cellGcmSetVertexProgramParameter(CellGcmContextData *ctx,
                                                    CGparameter param,
                                                    const float *values)
{
    if (!ctx || !param || !values) return;

    const CgBinaryParameter *pp = (const CgBinaryParameter *)param;
    if (pp->resIndex < 0) return;

    const unsigned rows  = ps3tc_cg_rows_for_type(pp->type);
    const unsigned words = rows * 4u;

    /* Reserve: UPLOAD_CONST_ID hdr + 1 arg           = 2 words,
     *          UPLOAD_CONST_X(0) hdr + `words` data  = 1 + words. */
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2u + 1u + words);
    if (!w) return;

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, 1);
    *w++ = (uint32_t)pp->resIndex;

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_CONST_X(0), words);
    memcpy(w, values, words * sizeof(uint32_t));
}

#ifdef __cplusplus
}
#endif

/* Sony-convention no-context C++ overloads — forward through the
 * gCellGcmCurrentContext global. */
#ifdef __cplusplus

static inline void cellGcmSetVertexProgram(CGprogram prog, void *ucode)
{ cellGcmSetVertexProgram(gCellGcmCurrentContext, prog, ucode); }

static inline void cellGcmSetFragmentProgram(CGprogram prog, uint32_t offset)
{ cellGcmSetFragmentProgram(gCellGcmCurrentContext, prog, offset); }

static inline void cellGcmSetVertexProgramParameter(CGparameter param,
                                                    const float *values)
{ cellGcmSetVertexProgramParameter(gCellGcmCurrentContext, param, values); }

#endif  /* __cplusplus */

#endif  /* PS3TC_CELL_GCM_CG_BRIDGE_H */
