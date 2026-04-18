/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_cg_bridge.h>
 *
 * Bridge between Sony-style CgBinaryProgram blobs (our Cg/cgBinary.h
 * layout, what sce-cgc emits) and PSL1GHT's rsx*Program runtime
 * command emitters (rsxLoadVertexProgram / rsxLoadFragmentProgram-
 * Location / rsxSetVertexProgramConstants).
 *
 * Sony's shader-binding API takes CGprogram / CGparameter handles;
 * PSL1GHT's takes rsx{Vertex,Fragment}Program structs.  The two
 * describe the same shader but lay their metadata out differently.
 * The inline wrappers below synthesize a minimal rsx*Program struct
 * on the caller's stack, populate only the fields the Load / Set
 * entry points actually read, and forward.
 *
 * Fields we skip on purpose: num_const, attr_off, const_off,
 * ucode_off.  rsxLoadVertexProgram loops over num_const to upload
 * *internal* constants (generator-synthesized immediates the
 * compiler embedded); Sony's cellGcmSetVertexProgram never uploads
 * these in the binding step — the caller follows up with explicit
 * cellGcmSetVertexProgramParameter calls for real uniforms.  Setting
 * num_const = 0 skips the loop cleanly.
 *
 * Limitation: we read num_regs / insn_start / instructionCount from
 * CgBinaryVertexProgram, but fragment-program field `texcoord3D` is
 * not exposed in CgBinaryFragmentProgram — we default it to 0 and
 * revisit when samples that use cube / volume textures surface the
 * gap.
 */

#ifndef PS3TC_CELL_GCM_CG_BRIDGE_H
#define PS3TC_CELL_GCM_CG_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include <Cg/cg.h>
#include <Cg/cgBinary.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- *
 * VP output mask
 * -------------------------------------------------------------------- *
 *
 * sce-cgc and PSL1GHT cgcomp encode CgBinaryVertexProgram.
 * attributeOutputMask differently.  For a pass-through
 * POSITION + COLOR shader, sce-cgc writes 0x1 (HPOS only), PSL1GHT
 * cgcomp writes 0x5 (HPOS + COL0's dual-bit signal).  The rsx
 * runtime consumes the PSL1GHT encoding, so handing sce-cgc's raw
 * value through leaves COL0 disabled in VP_RESULT_EN and the FP
 * reads garbage for every varying past position.
 *
 * We rebuild the mask from semantics: walk the parameter table,
 * look at output parameters (direction == CG_OUT / CG_INOUT), and
 * re-derive the PSL1GHT bit layout.  Format (matching what
 * PSL1GHT's cgcomp compilervp.cpp emits):
 *
 *   HPOS       bit 0   (always set — vertex shader must output it)
 *   PSZ        bit 5
 *   FOGC       bit 4
 *   COL0       bits 0 + 2    (dual-bit signal, 0x5)
 *   COL1       bits 1 + 3    (dual-bit signal, 0xA)
 *   BFC0       bits 0 + 2    (alias of COL0 — front/back share)
 *   BFC1       bits 1 + 3    (alias of COL1)
 *   TC0..TC7   bits 14 + i   (0x4000 << i)
 */
static inline uint32_t ps3tc_cg_output_mask(const CgBinaryProgram *p)
{
    if (!p) return 1u;  /* HPOS always */
    const CgBinaryParameter *params =
        (const CgBinaryParameter *)((const uint8_t *)p + p->parameterArray);

    uint32_t mask = 1u;  /* HPOS bit always — VP must output position */

    for (uint32_t i = 0; i < p->parameterCount; ++i)
    {
        const CgBinaryParameter *pp = &params[i];
        if (pp->direction != CG_OUT && pp->direction != CG_INOUT) continue;
        if (!pp->semantic) continue;

        const char *sem = (const char *)((const uint8_t *)p + pp->semantic);
        if (!sem || !*sem) continue;

        if (strncmp(sem, "COLOR", 5) == 0 || strncmp(sem, "COL", 3) == 0)
        {
            const int n = (strncmp(sem, "COLOR", 5) == 0) ? 5 : 3;
            const int idx = (sem[n] >= '0' && sem[n] <= '9') ? (sem[n] - '0') : 0;
            mask |= (1u << idx) | (4u << idx);
        }
        else if (strncmp(sem, "TEXCOORD", 8) == 0)
        {
            const int idx =
                (sem[8] >= '0' && sem[8] <= '9') ? (sem[8] - '0') : 0;
            mask |= (0x4000u << idx);
        }
        else if (strncmp(sem, "FOG", 3) == 0)
        {
            mask |= (1u << 4);
        }
        else if (strncmp(sem, "PSIZE", 5) == 0 ||
                 strncmp(sem, "PSZ", 3) == 0)
        {
            mask |= (1u << 5);
        }
        /* POSITION / HPOS already covered by the unconditional bit 0. */
    }
    return mask;
}

/* -------------------------------------------------------------------- *
 * Vertex-program binding
 * -------------------------------------------------------------------- */

static inline void cellGcmSetVertexProgram(CellGcmContextData *thisContext,
                                           CGprogram prog, void *ucode)
{
    if (!thisContext || !prog) return;

    const CgBinaryProgram *p = (const CgBinaryProgram *)prog;
    const CgBinaryVertexProgram *vp =
        (const CgBinaryVertexProgram *)((const uint8_t *)p + p->program);

    rsxVertexProgram rsx;
    memset(&rsx, 0, sizeof(rsx));
    rsx.num_insn    = (uint16_t)vp->instructionCount;
    rsx.num_regs    = (uint16_t)vp->registerCount;
    rsx.insn_start  = (uint16_t)vp->instructionSlot;
    rsx.input_mask  = vp->attributeInputMask;
    rsx.output_mask = ps3tc_cg_output_mask(p);
    /* num_const = 0 → LoadVertexProgram's const-upload loop iterates
     * zero times.  Caller uploads uniforms via SetVertexProgramParameter. */

    rsxLoadVertexProgram(thisContext, &rsx, ucode);
}

/* -------------------------------------------------------------------- *
 * Fragment-program binding (FP ucode lives in RSX memory; caller passes
 * the offset within `location` — GCM_LOCATION_RSX or LOCAL).
 * -------------------------------------------------------------------- */

static inline void cellGcmSetFragmentProgram(CellGcmContextData *thisContext,
                                             CGprogram prog, uint32_t offset)
{
    if (!thisContext || !prog) return;

    const CgBinaryProgram *p = (const CgBinaryProgram *)prog;
    const CgBinaryFragmentProgram *fp =
        (const CgBinaryFragmentProgram *)((const uint8_t *)p + p->program);

    rsxFragmentProgram rsx;
    memset(&rsx, 0, sizeof(rsx));
    rsx.num_insn     = (uint16_t)fp->instructionCount;
    rsx.num_regs     = fp->registerCount;
    /* fp_control low byte carries the output-register-select / KIL flags
     * that PSL1GHT's cgcomp emits inline; the NV40 FP_CONTROL register
     * itself puts the temp-count in bits 24-31, and rsxLoadFragment-
     * ProgramLocation OR's `num_regs << 24` in at upload time.  So the
     * bottom byte here must NOT hold register count — it holds:
     *   0x40 = output from R0 (full precision, default)
     *   0x0e = output from H0 (fp16)
     *   0x80 = KIL (bit 7)
     * Depth-replace does not live in FP_CONTROL on NV40 — it is set by a
     * separate command method; revisit when depth-writing samples land. */
    uint32_t fp_control = fp->outputFromH0 ? 0x0eu : 0x40u;
    if (fp->pixelKill) fp_control |= (1u << 7);
    rsx.fp_control   = fp_control;
    rsx.texcoords    = fp->texCoordsInputMask;
    rsx.texcoord2D   = fp->texCoords2D;
    rsx.texcoord3D   = 0;  /* CgBinaryFragmentProgram doesn't split 2D / 3D;
                            * revisit when cube / volume-texture samples land. */

    rsxLoadFragmentProgramLocation(thisContext, &rsx, offset, GCM_LOCATION_RSX);
}

/* -------------------------------------------------------------------- *
 * Uniform upload
 * -------------------------------------------------------------------- *
 *
 * CgBinaryParameter.type encodes scalar / vector / matrix dimensions
 * as sequential CGtype values.  RSX vertex constants are 4-float
 * registers, so every upload is `rows × 4` floats.  `rows` is the
 * matrix row count (1 for scalars / vectors).
 */

static inline unsigned ps3tc_cg_rows_for_type(uint32_t type)
{
    /* Matrix types laid out in Cg/cg.h group by row count.  Decode:
     * 4-row mats = CG_{HALF,FLOAT,FIXED,INT,BOOL}4x{1,2,3,4};
     * 3-row mats = *3x*; 2-row mats = *2x*; 1-row mats = *1x*. */
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

static inline void cellGcmSetVertexProgramParameter(CellGcmContextData *thisContext,
                                                    CGparameter param,
                                                    const float *values)
{
    if (!thisContext || !param || !values) return;

    const CgBinaryParameter *pp = (const CgBinaryParameter *)param;
    if (pp->resIndex < 0) return;  /* unreferenced parameter */

    const unsigned rows = ps3tc_cg_rows_for_type(pp->type);
    rsxSetVertexProgramConstants(thisContext,
                                 (uint32_t)pp->resIndex,
                                 (uint32_t)rows,
                                 values);
}

#ifdef __cplusplus
}
#endif

/* Sony-convention no-context C++ overloads.  Same gCellGcmCurrentContext
 * forwarding pattern we use for the other command emitters. */
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
