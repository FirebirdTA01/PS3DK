/*
 * NV40 fragment-program lowering.
 *
 * Stage 4 scope: identity varying passthrough (a single
 * `out color = in color;` style assignment).  Walks the IRFunction,
 * resolves IR value IDs to their NV40 FP input-source codes, and
 * emits a single MOV via FpAssembler.
 *
 * Container emit (.fpo / CgBinaryFragmentProgram) and the alphakill /
 * texformat / sample family land in subsequent stage-4 commits;
 * those features need their own reverse-engineering passes against
 * sce-cgc — recorded incrementally in docs/REVERSE_ENGINEERING.md.
 */

#include "nv40_emit.h"
#include "nv40_fp_assembler.h"

#include "nvfx_shader.h"

#include "ir.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace nv40::detail
{

// Container layer reads this from lowerFragmentProgram via the
// trailing FpAttributes* parameter (nullable when the caller only
// wants ucode).
namespace
{

std::string toUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

// SET_VERTEX_ATTRIB_OUTPUT_MASK bits used by both VP attributeOutputMask
// and FP attributeInputMask.  Reading COLOR sets both front + back
// diffuse (bits 0 + 2).  Reading TEXCOORDn sets bit (14+n).
inline uint32_t fpAttrMaskBitForInputSrc(int inputSrc)
{
    if (inputSrc == NVFX_FP_OP_INPUT_SRC_COL0)
        return (1u << 0) | (1u << 2);  // front diffuse + back diffuse
    if (inputSrc == NVFX_FP_OP_INPUT_SRC_COL1)
        return (1u << 1) | (1u << 3);  // front specular + back specular
    if (inputSrc == NVFX_FP_OP_INPUT_SRC_FOGC)
        return 1u << 4;
    if (inputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
        inputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
        return 1u << (14 + (inputSrc - NVFX_FP_OP_INPUT_SRC_TC(0)));
    return 0;
}

// Map Cg fragment-input semantic → NV40 FP INPUT_SRC code.
// (Per-instruction field in hw[0]; the SRC slot itself just sets
// register-type = INPUT.)
int fragmentInputSrc(const std::string& semanticUpper, int semanticIndex)
{
    if (semanticUpper == "POSITION" || semanticUpper == "WPOS")
        return NVFX_FP_OP_INPUT_SRC_POSITION;
    if (semanticUpper == "COLOR" || semanticUpper == "COL")
        return semanticIndex == 1 ? NVFX_FP_OP_INPUT_SRC_COL1
                                  : NVFX_FP_OP_INPUT_SRC_COL0;
    if (semanticUpper == "FOG" || semanticUpper == "FOGC")
        return NVFX_FP_OP_INPUT_SRC_FOGC;
    if (semanticUpper == "TEXCOORD" || semanticUpper == "TEX")
        return NVFX_FP_OP_INPUT_SRC_TC(semanticIndex);
    if (semanticUpper == "FACE")
        return NV40_FP_OP_INPUT_SRC_FACING;
    return -1;
}

// Map Cg fragment-output semantic → result-register index.
//   COLOR → R0  (result.color)
//   DEPTH → R1  (result.depth — only .z is meaningful)
int fragmentOutputIndex(const std::string& semanticUpper, int /*semanticIndex*/)
{
    if (semanticUpper == "COLOR" || semanticUpper == "COL") return 0;
    if (semanticUpper == "DEPTH" || semanticUpper == "DEPTH0") return 1;
    return -1;
}

}  // namespace

UcodeOutput lowerFragmentProgram(const IRModule& module, const IRFunction& entry,
                                 FpAttributes* attrsOut)
{
    UcodeOutput out;
    FpAssembler asm_;
    FpAttributes attrs;

    // IR value → NV40 FP INPUT_SRC code (e.g. NVFX_FP_OP_INPUT_SRC_COL0).
    // Populated from In/None parameters (the front-end leaves direct
    // varying reads as the parameter value, no explicit LoadVarying)
    // and from any LoadAttribute / LoadVarying instructions.
    std::unordered_map<IRValueID, int> valueToInputSrc;

    // sampler IR value → texture-unit index.  sce-cgc assigns tex
    // units in declaration order starting from 0; verified on the
    // single-sampler tex_f.cg case (sampler `tex` → TEXUNIT0).
    std::unordered_map<IRValueID, int> valueToTexUnit;

    // Records a deferred TexSample — emit the TEX instruction at the
    // point of consumption (matches sce-cgc's `TEXR R0, f[TEX0], TEX0`
    // which writes straight to R0 with no temp).
    struct TexBinding
    {
        IRValueID samplerId = 0;
        IRValueID uvId      = 0;
    };
    std::unordered_map<IRValueID, TexBinding> valueToTex;

    int nextTexUnit = 0;

    for (const auto& param : entry.parameters)
    {
        const bool isSampler = (param.type.baseType == IRType::Sampler2D ||
                                param.type.baseType == IRType::SamplerCube);
        if (param.storage == StorageQualifier::Uniform && isSampler)
        {
            valueToTexUnit[param.valueId] = nextTexUnit++;
            continue;
        }

        const bool isInput = (param.storage == StorageQualifier::In   ||
                              param.storage == StorageQualifier::InOut ||
                              param.storage == StorageQualifier::None);
        if (!isInput) continue;
        if (param.semanticName.empty()) continue;

        const std::string semUpper = toUpper(param.semanticName);
        const int srcCode = fragmentInputSrc(semUpper, param.semanticIndex);
        if (srcCode >= 0)
            valueToInputSrc[param.valueId] = srcCode;
    }

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
            case IROp::LoadVarying:
            {
                const std::string semUpper = toUpper(inst.semanticName);
                const int srcCode = fragmentInputSrc(semUpper, inst.semanticIndex);
                if (srcCode < 0)
                {
                    out.diagnostics.push_back(
                        "nv40-fp: unsupported input semantic '" + inst.semanticName + "'");
                    return out;
                }
                valueToInputSrc[inst.result] = srcCode;
                break;
            }

            case IROp::TexSample:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-fp: TexSample with <2 operands");
                    return out;
                }
                // tex2D(sampler, uv) — operands[0] = sampler, [1] = uv.
                valueToTex[inst.result] = { inst.operands[0], inst.operands[1] };
                break;
            }

            case IROp::StoreOutput:
            {
                if (inst.operands.empty())
                {
                    out.diagnostics.push_back("nv40-fp: StoreOutput with no operand");
                    return out;
                }

                const std::string semUpper = toUpper(inst.semanticName);
                const int outIndex = fragmentOutputIndex(semUpper, inst.semanticIndex);
                if (outIndex < 0)
                {
                    out.diagnostics.push_back(
                        "nv40-fp: unsupported output semantic '" + inst.semanticName + "'");
                    return out;
                }

                const IRValueID srcId = inst.operands[0];
                const struct nvfx_reg dstReg = nvfx_reg(NVFXSR_OUTPUT, outIndex);
                const struct nvfx_reg none   = nvfx_reg(NVFXSR_NONE, 0);

                // tex2D result consumed by StoreOutput: emit TEX writing
                // straight to the output register (sce-cgc's pattern —
                // no temp, no follow-up MOV).
                auto texIt = valueToTex.find(srcId);
                if (texIt != valueToTex.end())
                {
                    auto sampIt = valueToTexUnit.find(texIt->second.samplerId);
                    if (sampIt == valueToTexUnit.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: TexSample sampler did not resolve to a uniform sampler");
                        return out;
                    }
                    auto uvIt = valueToInputSrc.find(texIt->second.uvId);
                    if (uvIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: TexSample UV did not resolve to a varying input "
                            "(only direct TEXCOORDn passthrough supported)");
                        return out;
                    }

                    const struct nvfx_reg uvReg =
                        nvfx_reg(NVFXSR_INPUT, uvIt->second);

                    struct nvfx_src src0 = nvfx_src(const_cast<struct nvfx_reg&>(uvReg));
                    struct nvfx_src src1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src src2 = nvfx_src(const_cast<struct nvfx_reg&>(none));

                    struct nvfx_insn in = nvfx_insn(
                        0, 0,
                        sampIt->second,  // tex_unit — packed into hw[0] bits 17-20
                        -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        src0, src1, src2);
                    asm_.emit(in, NVFX_FP_OP_OPCODE_TEX);
                    emittedSomething = true;

                    // Track varying input usage for the container's
                    // attributeInputMask + texCoordsInputMask fields.
                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(uvIt->second);
                    if (uvIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        uvIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (uvIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }
                    break;
                }

                auto it = valueToInputSrc.find(srcId);
                if (it == valueToInputSrc.end())
                {
                    out.diagnostics.push_back(
                        "nv40-fp: StoreOutput source is not a direct varying input or "
                        "tex2D result (arithmetic lowering lands later)");
                    return out;
                }

                const struct nvfx_reg srcReg =
                    nvfx_reg(NVFXSR_INPUT, it->second);

                struct nvfx_src src0 = nvfx_src(const_cast<struct nvfx_reg&>(srcReg));
                struct nvfx_src src1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                struct nvfx_src src2 = nvfx_src(const_cast<struct nvfx_reg&>(none));

                struct nvfx_insn in = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dstReg),
                    NVFX_FP_MASK_ALL,
                    src0, src1, src2);
                asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                emittedSomething = true;

                attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(it->second);
                if (it->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                    it->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                {
                    attrs.texCoordsInputMask |=
                        uint16_t{1} << (it->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                }
                break;
            }

            case IROp::Return:
                break;

            default:
                out.diagnostics.push_back(
                    "nv40-fp: unsupported IR op (stage 4 handles direct varying "
                    "passthrough only)");
                return out;
            }
        }
    }

    if (!emittedSomething)
    {
        out.diagnostics.push_back("nv40-fp: no StoreOutput seen in entry point");
        return out;
    }

    asm_.markEnd();
    out.words = asm_.words();
    out.ok    = true;

    // Finalise FP attributes from assembler state.  registerCount stays
    // at the sce-cgc minimum of 2 unless the program actually allocates
    // more temps (R registers grow upward from R0 = result.color).
    const int regs = asm_.numTempRegs();
    if (regs > attrs.registerCount)
        attrs.registerCount = static_cast<uint8_t>(regs);

    if (attrsOut)
        *attrsOut = attrs;
    return out;
}

}  // namespace nv40::detail
