/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_cg_bridge.h>
 *
 * Native RSX command emitter for the cellGcmSetVertexProgram /
 * cellGcmSetFragmentProgram / cellGcmSetVertexProgramParameter surface.
 * Reads CgBinaryProgram blobs (reference-Cg-compiler output) directly
 * and writes NV40 command words to the current gcm command buffer —
 * no PSL1GHT rsx{Vertex,Fragment}Program intermediate, no rsxLoad*
 * forwarding.
 *
 * Reference-compiler output is the source of truth; the hardware
 * register encoding is derived here from the parameter-table semantics
 * and container fields rather than being consumed from PSL1GHT-style
 * struct fields.
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

/* Build-time diagnostic trace.  Pass `-DPS3TC_GCM_CG_BRIDGE_TRACE=1` to
 * dump every bridge call's inputs + first-word command-buffer output
 * to stdout.  RPCS3 captures stdout in its log; compare against
 * expected values to find field-decode or emit-order bugs.
 * Off (no overhead) by default. */
#ifndef PS3TC_GCM_CG_BRIDGE_TRACE
#define PS3TC_GCM_CG_BRIDGE_TRACE 0
#endif

#if PS3TC_GCM_CG_BRIDGE_TRACE
#  include <stdio.h>
#  define PS3TC_TRACE(...) do { printf("[cg_bridge] " __VA_ARGS__); fflush(stdout); } while (0)
#else
#  define PS3TC_TRACE(...) ((void)0)
#endif

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
/* Invoke the FIFO-refill callback through the PSL1GHT / cell-SDK PRX
 * calling convention.  The `callback` field is stored as an
 * `ATTRIBUTE_PRXPTR` (a 32-bit handle pointing to an 8-byte
 * [entry, toc] descriptor) rather than a full PPC64 ELFv1 OPD.
 * A direct C function-pointer call on that field lands on garbage.
 *
 * Mirrors PSL1GHT's rsxContextCallback (rsx_function_macros.h).
 * Must be noinline — the asm relies on r3=ctx and r4=count on entry
 * (standard PPC64 arg convention); inlining would let GCC reorder the
 * callers' register allocation and break that invariant. */
/* Invoke the FIFO-wrap callback via the PSL1GHT / cell-SDK PRX calling
 * convention, explicitly arranging the call frame so GCC can't
 * silently break the ABI.
 *
 * The `callback` field stores a 32-bit handle to an 8-byte PRX OPD
 * [entry_32, toc_32] — NOT a full PPC64 ELFv1 OPD.  We load entry
 * into CTR and the TOC into r2 (saving the current TOC in r31),
 * then `bctrl`.  The firmware callback expects
 *   r3 = gcmContextData *ctx
 *   r4 = u32          count
 * as per the standard PPC64 ELFv1 argument registers.
 *
 * Explicit `mr` into r3/r4 from the constraints ensures the ABI-
 * required arg registers are populated no matter what GCC did with
 * ctx/count before this asm.  r3 and r4 are in the clobber list
 * because bctrl replaces r3 with the return value and r4 is
 * destroyed by the callback; without the clobber GCC assumes they
 * keep our input values and reads struct fields through a stale
 * register post-asm (2026-04-18 cube crash).  The mirror copy of
 * this trampoline in PSL1GHT's rsx_function_macros.h doesn't do
 * this and only "works" because it happens to have no code before
 * the asm — adding anything (a printf, a counter write) shifts
 * GCC's register allocation enough to crash.
 *
 * TOC save / restore goes through the PPC64 ELFv1 TOC slot at
 * +24(r1) inside the 128-byte frame we allocate, rather than via
 * r31.  An earlier revision used `mr 31,2` / `mr 2,31` and listed
 * r31 in the clobber list — that forced every consuming TU to
 * compile with -fomit-frame-pointer, because GCC at -O0 pins r31
 * as the frame pointer and rejects the clobber with the diagnostic
 * "31 cannot be used in 'asm' here".  Optimised builds (-O1+)
 * auto-enabled omit-frame-pointer and skated past the issue, but
 * debug builds broke at the include site of <cell/gcm.h>.  Stack-
 * save costs one extra std/ld on the slow callback path, has no
 * register-pressure side effects, and lets debug builds compile
 * cleanly without any per-TU pragma. */
__attribute__((noinline))
static int32_t ps3tc_gcm_invoke_callback(CellGcmContextData *ctx, uint32_t count)
{
    int32_t result;

    __asm__ __volatile__ (
        "mr    3,%1\n"         /* r3 = ctx */
        "mr    4,%2\n"         /* r4 = count */
        "stdu  1,-128(1)\n"
        "std   2,24(1)\n"      /* save caller TOC to our frame */
        "lwz   0,0(%3)\n"      /* r0 = callback entry */
        "lwz   2,4(%3)\n"      /* r2 = callback TOC   */
        "mtctr 0\n"
        "bctrl\n"
        "mr    %0,3\n"         /* result = r3 (callback return) */
        "ld    2,24(1)\n"      /* restore caller TOC */
        "addi  1,1,128\n"
        : "=&r"(result)
        : "r"(ctx), "r"(count), "b"(ctx->callback)
        : "r0","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12",
          "lr","ctr","cr0","cr1","cr5","cr6","cr7","xer","memory"
    );

    return result;
}

static inline uint32_t *ps3tc_gcm_reserve(CellGcmContextData *ctx, uint32_t count)
{
    if ((ctx->current + count) > ctx->end)
    {
        if (ctx->callback && ps3tc_gcm_invoke_callback(ctx, count) != 0)
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
 * NV40 VP_RESULT_EN bit layout (confirmed by PSL1GHT
 * rsx/gcm_sys.h GCM_ATTRIB_OUTPUT_MASK_*):
 *
 *   bit 0      FRONTDIFFUSE  (COL0 when writing o[1])
 *   bit 1      FRONTSPECULAR (COL1)
 *   bit 2      BACKDIFFUSE   (BFC0 / o[3])
 *   bit 3      BACKSPECULAR  (BFC1)
 *   bit 4      FOG / FOGC
 *   bit 5      PSIZE / PSZ   (unconditionally enabled via
 *                             GCM_ATTRIB_OUTPUT_MASK_POINTSIZE)
 *   bit 6..11  UC0..UC5 (user clip planes)
 *   bit 12     TEX8
 *   bit 13     TEX9
 *   bit 14+n   TEXCOORD<n>    (for n = 0..7)
 *
 * Each channel is a single bit.  Earlier revisions of this routine
 * OR'd in bit (2+idx) for the "back" variant of each colour, which
 * declared BFC0/BFC1 as outputs the VP never actually writes — the
 * interpolator then fed zeros down the COL0 path.  The reference
 * basic-triangle sample's black-triangle symptom traced to exactly
 * that.
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
        PS3TC_TRACE("  param[%u] dir=%u var=%u res=%u resIdx=%d sem=\"%s\"\n",
                    (unsigned)i, (unsigned)pp->direction, (unsigned)pp->var,
                    (unsigned)pp->res, (int)pp->resIndex,
                    pp->semantic
                      ? (const char *)((const uint8_t *)p + pp->semantic)
                      : "(null)");
        if (pp->direction != CG_OUT && pp->direction != CG_INOUT) continue;
        if (!pp->semantic) continue;

        const char *sem = (const char *)((const uint8_t *)p + pp->semantic);
        if (!sem || !*sem) continue;

        if (strncmp(sem, "COLOR", 5) == 0 || strncmp(sem, "COL", 3) == 0)
        {
            const int n   = (strncmp(sem, "COLOR", 5) == 0) ? 5 : 3;
            const int idx = (sem[n] >= '0' && sem[n] <= '9') ? (sem[n] - '0') : 0;
            mask |= (1u << idx);  /* front diffuse/specular — single bit */
        }
        else if (strncmp(sem, "BCOL", 4) == 0 || strncmp(sem, "BACKCOLOR", 9) == 0)
        {
            const int n   = (strncmp(sem, "BACKCOLOR", 9) == 0) ? 9 : 4;
            const int idx = (sem[n] >= '0' && sem[n] <= '9') ? (sem[n] - '0') : 0;
            mask |= (4u << idx);  /* back diffuse/specular — bits 2, 3 */
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

    /* One method packet per VP instruction (count=4 each).  RPCS3
     * desyncs on consecutive count=32 packets (verified with VPs of
     * num_insn >= 16); the small-packet shape every shipping sample
     * uses (count <= 28) is empirically safe on both hardware and
     * RPCS3.  See sdk/libgcm_cmd/src/rsx_legacy/commands_impl.h:
     * LoadVertexProgramBlock for the matching change in the
     * rsxLoad* path. */
    /* Reservation: UPLOAD_FROM_ID hdr + 2 args         = 3 words,
     * per insn:    UPLOAD_INST hdr + 4 data words     = 5 words,
     * VP_ATTRIB_EN hdr + arg                          = 2 words,
     * VP_RESULT_EN hdr + arg                          = 2 words,
     * TRANSFORM_TIMEOUT hdr + arg                     = 2 words. */
    const uint32_t reserve = 3u + inst_count * 5u + 2u + 2u + 2u;

    if (!ucode || inst_count == 0 || inst_count > 512) return;

    uint32_t *w = ps3tc_gcm_reserve(ctx, reserve);
    if (!w) return;

    const uint32_t *src = (const uint32_t *)ucode;
    const uint32_t result_en = ps3tc_cg_vp_result_en(p) | GCM_ATTRIB_OUTPUT_MASK_POINTSIZE;

    PS3TC_TRACE("SetVP ucode=%p inst=%u start=%u regs=%u in=0x%x out=0x%x reserve=%u\n",
                ucode, (unsigned)inst_count, (unsigned)inst_start,
                (unsigned)vp->registerCount, (unsigned)vp->attributeInputMask,
                (unsigned)result_en, (unsigned)reserve);
#if PS3TC_GCM_CG_BRIDGE_TRACE
    {
        unsigned total = inst_count * 4u;
        for (unsigned k = 0; k < total; k += 4)
        {
            PS3TC_TRACE("  insn[%u] %08x %08x %08x %08x  lastBit=%u\n",
                        k / 4, src[k], src[k + 1], src[k + 2], src[k + 3],
                        (unsigned)(src[k + 3] & 1u));
        }
    }
#endif

    /* Program the start instruction ID (both args are the start index
     * — the NV40 wants the same value twice). */
    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_FROM_ID, 2);
    *w++ = inst_start;
    *w++ = inst_start;

    /* One UPLOAD_INST packet per VP instruction (count=4) — see
     * reservation comment above for the why. */
    for (uint32_t i = 0; i < inst_count; ++i)
    {
        *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_INST(0), 4);
        *w++ = src[0];
        *w++ = src[1];
        *w++ = src[2];
        *w++ = src[3];
        src += 4;
    }

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_ATTRIB_EN, 1);
    *w++ = vp->attributeInputMask;

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_RESULT_EN, 1);
    *w++ = result_en;

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

    PS3TC_TRACE("SetFP offset=0x%08x inst=%u regs=%u tc_in=0x%04x tc_2d=0x%04x outH0=%u kil=%u\n",
                (unsigned)offset, (unsigned)fp->instructionCount,
                (unsigned)fp->registerCount,
                (unsigned)fp->texCoordsInputMask,
                (unsigned)fp->texCoords2D,
                (unsigned)fp->outputFromH0,
                (unsigned)fp->pixelKill);

    /* Bind the FP ucode address.  The lower 29 bits carry the byte
     * offset within RSX memory; bit 0 of the header-byte carries
     * location (LOCAL vs RSX) — PSL1GHT writes (location+1) | offset
     * which matches the reference-runtime convention. */
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
     *   bits 0-7 : low-byte flags encoding output register mode + KIL.
     *              Matches PSL1GHT cgcomp (compilerfp.cpp):
     *                0x40 = output from R0 (32-bit float)
     *                0x0e = output from H0 (half-precision)
     *                0x80 = shader uses KIL
     *              The CgBinaryFragmentProgram.outputFromH0 flag tells
     *              us which output register the reference Cg compiler
     *              used.  Observed: the reference basic-triangle FP had
     *              outputFromH0=0 (R0) and rendered solid black because
     *              we weren't setting 0x40
     *              — the HW then had no defined output register.
     *   bit  10  : program valid / enable — must be set for the FP
     *              to actually execute.
     *   bits 24-31: number of temps (register-count, clamped ≥ 2 —
     *              NV40 requires a minimum of 2 allocated temps). */
    {
        const uint32_t num_regs = (fp->registerCount > 2)
                                ? fp->registerCount : 2u;

        uint32_t low = 0;
        low |= (fp->outputFromH0 ? 0x0eu : 0x40u);
        if (fp->pixelKill) low |= (1u << 7);

        uint32_t fpcontrol = low
                           | (1u << 10)
                           | (num_regs << 24);

        PS3TC_TRACE("SetFP fp_control=0x%08x (low=0x%x num_regs=%u)\n",
                    (unsigned)fpcontrol, (unsigned)low, (unsigned)num_regs);

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

    PS3TC_TRACE("SetVPParam resIndex=%d type=%u rows=%u first=[%g %g %g %g]\n",
                (int)pp->resIndex, (unsigned)pp->type, rows,
                values[0], values[1], values[2], values[3]);

    /* NV40TCL_VP_UPLOAD_CONST_ID sits at 0x1efc, UPLOAD_CONST_X(0) at
     * 0x1f00 — i.e. exactly 4 bytes further.  One method header with
     * count = (1 + words) has the hardware write the ID first, then
     * auto-increment to 0x1f00 and stream the float payload through
     * CONST_X(0) / Y(0) / Z(0) / W(0) / X(1) / … in one atomic command.
     * Issuing UPLOAD_CONST_ID and UPLOAD_CONST_X(0) as two separate
     * commands doesn't behave the same — the hardware appears to
     * reset the upload state between them and the subsequent draw
     * takes stale / undefined constant values. */
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2u + words);
    if (!w) return;

    *w++ = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, words + 1u);
    *w++ = (uint32_t)pp->resIndex;
    memcpy(w, values, words * sizeof(uint32_t));
}

#ifdef __cplusplus
}
#endif

/* Cell-SDK-convention no-context C++ overloads — forward through the
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
