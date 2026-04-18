/*
 * NV40 vertex-program lowering.
 *
 * Walks an IRFunction and produces an NV40 ucode stream via VpAssembler.
 * Stage 2 scope: identity passthrough (LoadAttribute → StoreOutput with
 * matching semantic).  More complex patterns — constants, arithmetic,
 * matrix math, branching — land in stage 3.
 */

#include "nv40_emit.h"
#include "nv40_vp_assembler.h"

#include "nv40_vertprog.h"
#include "nv30_vertprog.h"
#include "nvfx_shader.h"

#include "ir.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace nv40::detail
{

namespace
{

std::string toUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Map Cg input semantic → NV40 vertex-attribute index.  Based on the
// NV_vertex_program binding layout enumerated in nvfx_shader.h.
int vertexInputIndex(const std::string& semanticUpper, int semanticIndex)
{
    if (semanticUpper == "POSITION") return NVFX_VP_INST_IN_POS;
    if (semanticUpper == "NORMAL")   return NVFX_VP_INST_IN_NORMAL;
    if (semanticUpper == "COLOR")    return semanticIndex == 1
                                         ? NVFX_VP_INST_IN_COL1
                                         : NVFX_VP_INST_IN_COL0;
    if (semanticUpper == "TEXCOORD") return NVFX_VP_INST_IN_TC(semanticIndex);
    if (semanticUpper == "FOG")      return NVFX_VP_INST_IN_FOGC;
    if (semanticUpper == "BLENDWEIGHT" || semanticUpper == "WEIGHT")
        return NVFX_VP_INST_IN_WEIGHT;
    return -1;
}

// Map Cg output semantic → NV40 vertex-program DEST index.
int vertexOutputIndex(const std::string& semanticUpper, int semanticIndex)
{
    if (semanticUpper == "POSITION" || semanticUpper == "HPOS")
        return NV40_VP_INST_DEST_POS;
    if (semanticUpper == "COLOR" || semanticUpper == "COL")
        return semanticIndex == 1 ? NV40_VP_INST_DEST_COL1
                                   : NV40_VP_INST_DEST_COL0;
    if (semanticUpper == "BCOL" || semanticUpper == "BACKCOLOR")
        return semanticIndex == 1 ? NV40_VP_INST_DEST_BFC1
                                   : NV40_VP_INST_DEST_BFC0;
    if (semanticUpper == "FOG" || semanticUpper == "FOGC")
        return NV40_VP_INST_DEST_FOGC;
    if (semanticUpper == "PSIZE" || semanticUpper == "POINTSIZE")
        return NV40_VP_INST_DEST_PSZ;
    if (semanticUpper == "TEXCOORD" || semanticUpper == "TEX")
        return NV40_VP_INST_DEST_TC(semanticIndex);
    return -1;
}

// Helpers that build the nvfx structs with C++-friendly syntax.  The
// donor headers provide C-style inline constructors (nvfx_reg, nvfx_src,
// nvfx_insn); we just wrap them.
struct nvfx_reg makeReg(int type, int index)
{
    return nvfx_reg(type, index);
}

struct nvfx_src makeSrc(const struct nvfx_reg& r)
{
    return nvfx_src(const_cast<struct nvfx_reg&>(r));
}

#define VP_OP(NAME) ((NVFX_VP_INST_SLOT_VEC << 7) | NVFX_VP_INST_VEC_OP_##NAME)

struct IoBinding
{
    int         nv40Index = -1;    // NV40 register index
    std::string semanticName;      // original semantic (upper-cased)
    int         semanticIndex = 0;
};

}  // namespace

UcodeOutput lowerVertexProgram(const IRModule& /*module*/, const IRFunction& entry)
{
    UcodeOutput out;
    VpAssembler asm_;

    // Map from IRValueID produced by LoadAttribute → the NV40 input
    // register index we'll reference from downstream sources.
    std::unordered_map<IRValueID, IoBinding> valueToInput;

    bool emittedSomething = false;

    for (const auto& blockPtr : entry.blocks)
    {
        if (!blockPtr) continue;
        for (const auto& instPtr : blockPtr->instructions)
        {
            if (!instPtr) continue;
            const IRInstruction& inst = *instPtr;

            switch (inst.op)
            {
            case IROp::LoadAttribute:
            {
                IoBinding b;
                b.semanticName  = toUpper(inst.semanticName);
                b.semanticIndex = inst.semanticIndex;
                b.nv40Index     = vertexInputIndex(b.semanticName, b.semanticIndex);
                if (b.nv40Index < 0)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: unsupported input semantic '" + inst.semanticName + "'");
                    return out;
                }
                valueToInput[inst.result] = std::move(b);
                break;
            }

            case IROp::StoreOutput:
            {
                if (inst.operands.empty())
                {
                    out.diagnostics.push_back(
                        "nv40-vp: StoreOutput with no operand");
                    return out;
                }

                const IRValueID srcId = inst.operands[0];
                auto it = valueToInput.find(srcId);
                if (it == valueToInput.end())
                {
                    // Only the identity passthrough pattern is handled
                    // in this stage.  Anything that requires arithmetic
                    // or constants needs a temp allocator + regalloc
                    // pass that lands in stage 3.
                    out.diagnostics.push_back(
                        "nv40-vp: StoreOutput source is not a direct attribute load "
                        "(stage 2 only supports identity shaders)");
                    return out;
                }

                const std::string semUpper = toUpper(inst.semanticName);
                const int outIndex =
                    vertexOutputIndex(semUpper, inst.semanticIndex);
                if (outIndex < 0)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: unsupported output semantic '" + inst.semanticName + "'");
                    return out;
                }

                // Build MOV dst=o[outIndex], src0=v[inIndex], xyzw identity.
                const struct nvfx_reg  dstReg = makeReg(NVFXSR_OUTPUT, outIndex);
                const struct nvfx_reg  srcReg = makeReg(NVFXSR_INPUT,  it->second.nv40Index);
                const struct nvfx_reg  none   = makeReg(NVFXSR_NONE, 0);

                struct nvfx_src src0 = makeSrc(srcReg);
                struct nvfx_src src1 = makeSrc(none);
                struct nvfx_src src2 = makeSrc(none);

                struct nvfx_insn in = nvfx_insn(
                    /* sat   */ 0,
                    /* op    */ 0,           // filled below via VP_OP
                    /* unit  */ -1,
                    /* target*/ -1,
                    /* dst   */ const_cast<struct nvfx_reg&>(dstReg),
                    /* mask  */ NVFX_VP_MASK_ALL,
                    src0, src1, src2);

                asm_.emit(in, VP_OP(MOV));
                emittedSomething = true;
                break;
            }

            case IROp::Return:
                // Nothing to emit for identity path — StoreOutput already
                // placed the result into the output register.
                break;

            default:
                out.diagnostics.push_back(
                    "nv40-vp: unsupported IR op (stage 2 only handles identity)");
                return out;
            }
        }
    }

    if (!emittedSomething)
    {
        out.diagnostics.push_back("nv40-vp: no StoreOutput seen in entry point");
        return out;
    }

    asm_.markLast();
    out.words = asm_.words();
    out.ok    = true;
    return out;
}

}  // namespace nv40::detail
