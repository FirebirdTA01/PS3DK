/*
 * PS3 Custom Toolchain — cellGcmCg* implementations.
 *
 * Clean-room struct walkers over the CgBinaryProgram layout defined
 * in <Cg/cgBinary.h>.  Each entry point is an offset-to-pointer or
 * struct-field read; we do not re-encode the blob or maintain any
 * hidden per-program runtime state.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <Cg/cgBinary.h>
#include <cell/gcm/gcm_cg_func.h>

/* ---------- helpers ---------------------------------------------------- */

static inline const CgBinaryProgram *as_program(CGprogram p)
{
    return (const CgBinaryProgram *)p;
}

static inline const CgBinaryParameter *as_parameter(CGparameter p)
{
    return (const CgBinaryParameter *)p;
}

/* Resolve a CgBinaryOffset (byte offset from the start of the program
 * blob) to a usable pointer.  Returns NULL when the offset is zero,
 * which the on-disk format uses as "no value". */
static const void *resolve(const CgBinaryProgram *prog, CgBinaryOffset off)
{
    if (!prog || off == 0) return NULL;
    return (const uint8_t *)prog + off;
}

/* Typed helper to find the parameter array base. */
static const CgBinaryParameter *param_base(const CgBinaryProgram *prog)
{
    return (const CgBinaryParameter *)resolve(prog, prog->parameterArray);
}

/* Typed helper for the vertex-program header.  A matching
 * fragment_hdr() helper will land alongside the fragment-specific
 * accessors once we add them (outputFromH0, pixelKill, etc.). */
static const CgBinaryVertexProgram *vertex_hdr(const CgBinaryProgram *prog)
{
    return (const CgBinaryVertexProgram *)resolve(prog, prog->program);
}

/* ---------- container-level queries ------------------------------------ */

void cellGcmCgInitProgram(CGprogram prog)
{
    /* On-disk data is stored big-endian.  The PPU is already big-endian
     * so no byte-swap is required; we expose this entry point only for
     * source compatibility with callers that invoke it unconditionally. */
    (void)prog;
}

void cellGcmCgGetUCode(CGprogram prog, void **ucode_out, uint32_t *size_out)
{
    const CgBinaryProgram *p = as_program(prog);
    if (!p)
    {
        if (ucode_out) *ucode_out = NULL;
        if (size_out)  *size_out  = 0;
        return;
    }
    if (ucode_out) *ucode_out = (void *)resolve(p, p->ucode);
    if (size_out)  *size_out  = p->ucodeSize;
}

uint32_t cellGcmCgGetTotalBinarySize(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    return p ? p->totalSize : 0u;
}

uint32_t cellGcmCgGetProgramProfile(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    return p ? p->profile : 0u;
}

/* ---------- vertex-program attributes ---------------------------------- */

uint32_t cellGcmCgGetRegisterCount(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    const CgBinaryVertexProgram *vh = p ? vertex_hdr(p) : NULL;
    return vh ? vh->registerCount : 0u;
}

uint32_t cellGcmCgGetInstructionSlot(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    const CgBinaryVertexProgram *vh = p ? vertex_hdr(p) : NULL;
    return vh ? vh->instructionSlot : 0u;
}

uint32_t cellGcmCgGetVertexAttribInputMask(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    const CgBinaryVertexProgram *vh = p ? vertex_hdr(p) : NULL;
    return vh ? vh->attributeInputMask : 0u;
}

uint32_t cellGcmCgGetAttribOutputMask(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    const CgBinaryVertexProgram *vh = p ? vertex_hdr(p) : NULL;
    return vh ? vh->attributeOutputMask : 0u;
}

uint32_t cellGcmCgGetVertexUserClipMask(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    const CgBinaryVertexProgram *vh = p ? vertex_hdr(p) : NULL;
    return vh ? vh->userClipMask : 0u;
}

/* Total instruction count.  Both VP and FP container headers expose
 * instructionCount at offset 0 of the program-specific header — a
 * single uint32_t read works for both profiles. */
uint32_t cellGcmCgGetInstructions(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    if (!p) return 0u;
    const uint32_t *hdr = (const uint32_t *)resolve(p, p->program);
    return hdr ? hdr[0] : 0u;
}

/* Mutators — write into the (caller-owned) container header in place.
 * Container is in main memory until uploaded to the GPU, so a normal
 * cast-away-const + assign is safe.  Both VP and FP header layouts
 * carry registerCount + attributeOutputMask but at different offsets
 * and widths, so dispatch on the program's profile tag. */
static int prog_is_fp(const CgBinaryProgram *p)
{
    return p && (p->profile == CG_PROFILE_SCE_FP_RSX);
}

void cellGcmCgSetRegisterCount(CGprogram prog, uint32_t count)
{
    const CgBinaryProgram *p = as_program(prog);
    if (!p) return;
    void *hdr = (void *)resolve(p, p->program);
    if (!hdr) return;
    if (prog_is_fp(p))
        ((CgBinaryFragmentProgram *)hdr)->registerCount = (uint8_t)count;
    else
        ((CgBinaryVertexProgram *)hdr)->registerCount = count;
}

void cellGcmCgSetAttribOutputMask(CGprogram prog, uint32_t mask)
{
    const CgBinaryProgram *p = as_program(prog);
    if (!p) return;
    void *hdr = (void *)resolve(p, p->program);
    if (!hdr) return;
    /* FP doesn't have attributeOutputMask; the call is a no-op there. */
    if (!prog_is_fp(p))
        ((CgBinaryVertexProgram *)hdr)->attributeOutputMask = mask;
}

/* ---------- parameter iteration ---------------------------------------- */

uint32_t cellGcmCgGetCountParameter(CGprogram prog)
{
    const CgBinaryProgram *p = as_program(prog);
    return p ? p->parameterCount : 0u;
}

CGparameter cellGcmCgGetIndexParameter(CGprogram prog, uint32_t index)
{
    const CgBinaryProgram *p = as_program(prog);
    const CgBinaryParameter *base = p ? param_base(p) : NULL;
    if (!base || index >= p->parameterCount) return NULL;
    return (CGparameter)&base[index];
}

CGparameter cellGcmCgGetNamedParameter(CGprogram prog, const char *name)
{
    const CgBinaryProgram *p = as_program(prog);
    const CgBinaryParameter *base = p ? param_base(p) : NULL;
    if (!base || !name) return NULL;

    for (uint32_t i = 0; i < p->parameterCount; ++i)
    {
        const char *pname = (const char *)resolve(p, base[i].name);
        if (pname && strcmp(pname, name) == 0)
            return (CGparameter)&base[i];
    }
    return NULL;
}

CGparameter cellGcmCgGetFirstLeafParameter(CGprogram prog)
{
    return cellGcmCgGetIndexParameter(prog, 0u);
}

CGparameter cellGcmCgGetNextLeafParameter(CGprogram prog, CGparameter param)
{
    const CgBinaryProgram   *p   = as_program(prog);
    const CgBinaryParameter *cur = as_parameter(param);
    if (!p || !cur) return NULL;

    const CgBinaryParameter *base = param_base(p);
    if (!base) return NULL;

    const ptrdiff_t idx = cur - base;
    if (idx < 0 || (uint32_t)idx + 1u >= p->parameterCount) return NULL;
    return (CGparameter)&base[idx + 1];
}

/* ---------- per-parameter accessors ------------------------------------ */

uint32_t cellGcmCgGetParameterType(CGprogram prog, CGparameter param)
{
    (void)prog;
    const CgBinaryParameter *pp = as_parameter(param);
    return pp ? pp->type : 0u;
}

uint32_t cellGcmCgGetParameterResource(CGprogram prog, CGparameter param)
{
    (void)prog;
    const CgBinaryParameter *pp = as_parameter(param);
    return pp ? pp->res : 0u;
}

int32_t cellGcmCgGetParameterResourceIndex(CGprogram prog, CGparameter param)
{
    (void)prog;
    const CgBinaryParameter *pp = as_parameter(param);
    return pp ? pp->resIndex : 0;
}

uint32_t cellGcmCgGetParameterVariability(CGprogram prog, CGparameter param)
{
    (void)prog;
    const CgBinaryParameter *pp = as_parameter(param);
    return pp ? pp->var : 0u;
}

uint32_t cellGcmCgGetParameterDirection(CGprogram prog, CGparameter param)
{
    (void)prog;
    const CgBinaryParameter *pp = as_parameter(param);
    return pp ? pp->direction : 0u;
}

int32_t cellGcmCgGetParameterOrdinalNumber(CGprogram prog, CGparameter param)
{
    (void)prog;
    const CgBinaryParameter *pp = as_parameter(param);
    return pp ? pp->paramno : -1;
}

CGbool cellGcmCgGetParameterReferenced(CGprogram prog, CGparameter param)
{
    (void)prog;
    const CgBinaryParameter *pp = as_parameter(param);
    return pp ? pp->isReferenced : CG_FALSE;
}

const char *cellGcmCgGetParameterName(CGprogram prog, CGparameter param)
{
    const CgBinaryProgram   *p  = as_program(prog);
    const CgBinaryParameter *pp = as_parameter(param);
    if (!p || !pp) return NULL;
    return (const char *)resolve(p, pp->name);
}

const char *cellGcmCgGetParameterSemantic(CGprogram prog, CGparameter param)
{
    const CgBinaryProgram   *p  = as_program(prog);
    const CgBinaryParameter *pp = as_parameter(param);
    if (!p || !pp) return NULL;
    return (const char *)resolve(p, pp->semantic);
}

const float *cellGcmCgGetParameterValues(CGprogram prog, CGparameter param)
{
    const CgBinaryProgram   *p  = as_program(prog);
    const CgBinaryParameter *pp = as_parameter(param);
    if (!p || !pp) return NULL;
    return (const float *)resolve(p, pp->defaultValue);
}

/* ---------- fragment embedded-constant iteration ----------------------- */

static const CgBinaryEmbeddedConstant *embedded_list(const CgBinaryProgram *p,
                                                     const CgBinaryParameter *pp)
{
    if (!p || !pp) return NULL;
    return (const CgBinaryEmbeddedConstant *)resolve(p, pp->embeddedConst);
}

uint32_t cellGcmCgGetEmbeddedConstantCount(CGprogram prog, CGparameter param)
{
    const CgBinaryEmbeddedConstant *ec =
        embedded_list(as_program(prog), as_parameter(param));
    return ec ? ec->ucodeCount : 0u;
}

uint32_t cellGcmCgGetEmbeddedConstantOffset(CGprogram prog, CGparameter param,
                                            uint32_t i)
{
    const CgBinaryEmbeddedConstant *ec =
        embedded_list(as_program(prog), as_parameter(param));
    if (!ec || i >= ec->ucodeCount) return 0u;
    return ec->ucodeOffset[i];
}
