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
#include <unordered_set>

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
                                 const rsx_cg::CompileOptions& /*opts*/,
                                 FpAttributes* attrsOut)
{
    // opts is currently unused.  The first real gate will be
    // `fastmath` — controls MAD fusion + fp16 COLOR varying promote.
    // Each new optimization pass we write should check `opts` here
    // (or in a helper module) rather than becoming unconditional.
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

    // FP uniform IR value → index into the entry's parameter list.
    // FP uniforms (non-sampler) are inlined as 16-byte zero-initialised
    // const blocks after each using instruction; the container emitter
    // stitches the per-use offsets into CgBinaryEmbeddedConstant
    // records so the runtime can patch the uniform's value in.
    std::unordered_map<IRValueID, unsigned> valueToFpUniform;

    // Records a deferred TexSample — emit the TEX instruction at the
    // point of consumption (matches sce-cgc's `TEXR R0, f[TEX0], TEX0`
    // which writes straight to R0 with no temp).
    struct TexBinding
    {
        IRValueID samplerId = 0;
        IRValueID uvId      = 0;
    };
    std::unordered_map<IRValueID, TexBinding> valueToTex;

    // Deferred arithmetic op with one INPUT (varying) operand and one
    // FP-uniform operand.  Emit at the point of consumption by a
    // StoreOutput so the arithmetic result can flow straight to the
    // output register.  Per sce-cgc byte probes:
    //   - ADD emits src0=CONST(uniform), src1=INPUT(varying)
    //   - MUL emits src0=INPUT(varying), src1=CONST(uniform)
    //   (operand order in C source is ignored — sce-cgc canonicalises)
    // Per sce-cgc byte probes, all four opcodes take (input, uniform)
    // canonicalised the same way:
    //   ADD:      src0 = CONST (uniform),  src1 = INPUT
    //   MUL/MIN/MAX: src0 = INPUT,           src1 = CONST (uniform)
    enum class FpArithOp { Add, Mul, Min, Max };
    struct FpArithBinding
    {
        FpArithOp op;
        IRValueID inputId   = 0;
        IRValueID uniformId = 0;
        bool      inputAbs   = false;
        bool      inputNeg   = false;
        bool      uniformAbs = false;
        bool      uniformNeg = false;
    };
    std::unordered_map<IRValueID, FpArithBinding> valueToArith;

    // MAD fusion: Add(Mul(input, uniformMul), addend) where addend is
    // either a literal vec4 or another uniform.  sce-cgc lowers this
    // to a four-block sequence:
    //
    //   MOVH H0, f[input]        # promote varying to fp16 H-temp
    //   MOVR R1, preloadSrc      # preload one const-source into R1
    //   <16-byte zero block for preloadSrc's uniform if any>
    //   FENCBR
    //   MADR R_out, H0, <A>, <B> # MAD = H0 * multiplier + addend
    //   <16-byte inline block for whichever of multiplier/addend wasn't preloaded>
    //
    // Scheduling rule (verified from sce-cgc byte probes):
    //   - If addend is a LITERAL: preload multiplier uniform into R1,
    //     leave literal inline at MAD SRC2.
    //   - If addend is a UNIFORM: preload addend uniform into R1,
    //     leave multiplier inline at MAD SRC1.
    struct FpMadBinding
    {
        IRValueID inputId;               // varying operand to Mul
        IRValueID multiplierUniformId;   // uniform operand to Mul
        IRValueID addendId;              // Add's second operand — literal OR uniform
        bool      addendIsLiteral = false;
    };
    std::unordered_map<IRValueID, FpMadBinding> valueToMad;

    // Saturate is a modifier on the producing instruction — bit 31 of
    // hw[0].  Rather than plumbing it through every binding type, we
    // alias the saturate-result IR value to its source and track the
    // saturate flag separately, then look both up at emit time.
    std::unordered_map<IRValueID, IRValueID> saturateAlias;
    std::unordered_set<IRValueID>            saturatedResults;

    // Source-side modifiers: abs (NV40 FP SRC_ABS bit 29/18 by slot)
    // and negate (NV40 FP REG_NEGATE bit 17 of the common source
    // bits).  We record a chain map `id → {base, abs, neg}` and
    // resolve transitively at emit time.
    //
    // Combining rules matching sce-cgc (and standard Cg semantics):
    //   - abs(neg(x)) = abs(x)     → abs wins, neg cleared
    //   - neg(abs(x)) = -abs(x)    → both flags stay set
    //   - neg(neg(x)) = x          → neg toggles
    struct SrcMod { IRValueID baseId = 0; bool absMod = false; bool negMod = false; };
    std::unordered_map<IRValueID, SrcMod> valueToMod;

    auto resolveSrcMods = [&](IRValueID id)
    {
        SrcMod r; r.baseId = id;
        for (int hops = 0; hops < 16; ++hops)
        {
            auto it = valueToMod.find(r.baseId);
            if (it == valueToMod.end()) break;
            if (it->second.absMod) { r.absMod = true; r.negMod = false; }
            if (it->second.negMod) r.negMod = !r.negMod;
            r.baseId = it->second.baseId;
        }
        return r;
    };

    // Literal `float4(k0, k1, k2, k3)` where all kN are constants —
    // deferred so the consuming StoreOutput can emit FENCBR + MOV +
    // 16-byte inline const block (matches sce-cgc's `FENCBR; MOVR R0,
    // {0x...}; END` pattern for a direct-literal shader).  Non-trivial
    // lifetimes (literal feeding arithmetic or texture sampling) land
    // in later stages.
    struct LiteralVec4
    {
        float vals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };
    std::unordered_map<IRValueID, LiteralVec4> valueToLiteralVec4;

    int nextTexUnit = 0;

    for (size_t pi = 0; pi < entry.parameters.size(); ++pi)
    {
        const auto& param = entry.parameters[pi];
        const bool isSampler = (param.type.baseType == IRType::Sampler2D ||
                                param.type.baseType == IRType::SamplerCube);
        if (param.storage == StorageQualifier::Uniform && isSampler)
        {
            valueToTexUnit[param.valueId] = nextTexUnit++;
            continue;
        }
        if (param.storage == StorageQualifier::Uniform)
        {
            // Non-sampler FP uniform: inline-const block per use.
            valueToFpUniform[param.valueId] = static_cast<unsigned>(pi);
            attrs.embeddedUniforms.push_back({ static_cast<unsigned>(pi), {} });
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

            case IROp::Saturate:
            {
                if (inst.operands.size() != 1) break;
                // Alias the saturate result to its source so binding
                // lookups resolve transparently; flag the result as
                // saturated so the emit site sets insn.sat = 1.
                saturateAlias[inst.result]     = inst.operands[0];
                saturatedResults.insert(inst.result);
                break;
            }

            case IROp::Abs:
            case IROp::Neg:
            {
                if (inst.operands.size() != 1) break;
                SrcMod m;
                m.baseId = inst.operands[0];
                if (inst.op == IROp::Abs) m.absMod = true;
                else                      m.negMod = true;
                valueToMod[inst.result] = m;
                break;
            }

            case IROp::Add:
            case IROp::Sub:
            case IROp::Mul:
            case IROp::Min:
            case IROp::Max:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-fp: arithmetic op with <2 operands");
                    return out;
                }

                const IRValueID a = inst.operands[0];
                const IRValueID b = inst.operands[1];

                // MAD-fusion detection — only for Add.  Match
                // Add(MulBinding, literal_or_uniform); commutative.
                if (inst.op == IROp::Add)
                {
                    auto tryMad = [&](IRValueID mulId, IRValueID addendId) -> bool
                    {
                        auto mIt = valueToArith.find(mulId);
                        if (mIt == valueToArith.end() ||
                            mIt->second.op != FpArithOp::Mul)
                            return false;

                        const bool addLit = valueToLiteralVec4.count(addendId) > 0;
                        const bool addUni = valueToFpUniform.count(addendId)  > 0;
                        if (!addLit && !addUni) return false;

                        FpMadBinding b;
                        b.inputId             = mIt->second.inputId;
                        b.multiplierUniformId = mIt->second.uniformId;
                        b.addendId            = addendId;
                        b.addendIsLiteral     = addLit;
                        valueToMad[inst.result] = b;
                        return true;
                    };
                    if (tryMad(a, b) || tryMad(b, a)) break;
                }

                // Resolve source modifiers: the IR might hand us
                // `abs(input) + uniform`, in which case operand[0] is
                // an IROp::Abs result but its base is the varying.
                const SrcMod ma = resolveSrcMods(a);
                const SrcMod mb = resolveSrcMods(b);

                auto aInput   = valueToInputSrc.find(ma.baseId);
                auto bInput   = valueToInputSrc.find(mb.baseId);
                auto aUniform = valueToFpUniform.find(ma.baseId);
                auto bUniform = valueToFpUniform.find(mb.baseId);

                FpArithBinding bind;
                bind.op = FpArithOp::Add;
                switch (inst.op)
                {
                case IROp::Add: bind.op = FpArithOp::Add; break;
                case IROp::Sub: bind.op = FpArithOp::Add; break;  // emit as ADD with negate on src1
                case IROp::Mul: bind.op = FpArithOp::Mul; break;
                case IROp::Min: bind.op = FpArithOp::Min; break;
                case IROp::Max: bind.op = FpArithOp::Max; break;
                default: break;  // unreachable
                }
                if (aInput != valueToInputSrc.end() && bUniform != valueToFpUniform.end())
                {
                    bind.inputId         = ma.baseId;
                    bind.inputAbs        = ma.absMod;
                    bind.inputNeg        = ma.negMod;
                    bind.uniformId       = mb.baseId;
                    bind.uniformAbs      = mb.absMod;
                    bind.uniformNeg      = mb.negMod;
                }
                else if (aUniform != valueToFpUniform.end() && bInput != valueToInputSrc.end())
                {
                    bind.inputId         = mb.baseId;
                    bind.inputAbs        = mb.absMod;
                    bind.inputNeg        = mb.negMod;
                    bind.uniformId       = ma.baseId;
                    bind.uniformAbs      = ma.absMod;
                    bind.uniformNeg      = ma.negMod;
                }
                else
                {
                    out.diagnostics.push_back(
                        "nv40-fp: arithmetic ops supported only for (varying, uniform) pairs "
                        "(uniform+uniform / literal arithmetic lands later)");
                    return out;
                }
                // Sub lowers to Add with the 2nd operand (the uniform
                // in our canonical pattern) negated.  NV40 ADD is
                // commutative so this stays bit-exact regardless of
                // which Cg source is first.  Verified byte probe:
                // `vcol - tint` → `ADDR R0, -tint, f[COL0]`.
                if (inst.op == IROp::Sub)
                    bind.uniformNeg = !bind.uniformNeg;

                valueToArith[inst.result] = bind;
                break;
            }

            case IROp::VecConstruct:
            {
                // Recognise `float4(k0, k1, k2, k3)` where every kN is
                // a scalar float constant.  Other VecConstruct shapes
                // (mixed input + const, narrower constructors) land
                // with the arithmetic pass.
                if (inst.operands.size() != 4) break;

                LiteralVec4 lv;
                bool allConst = true;
                for (int k = 0; k < 4; ++k)
                {
                    IRValue* v = entry.getValue(inst.operands[k]);
                    auto* c = dynamic_cast<IRConstant*>(v);
                    if (!c || !std::holds_alternative<float>(c->value))
                    {
                        allConst = false;
                        break;
                    }
                    lv.vals[k] = std::get<float>(c->value);
                }
                if (!allConst) break;

                valueToLiteralVec4[inst.result] = lv;
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

                IRValueID srcId = inst.operands[0];
                // Resolve saturate-alias chain: `%B = sat %A`, `stout %B`
                // should emit the instruction that produces %A with
                // insn.sat = 1.
                bool saturate = false;
                while (true)
                {
                    auto sIt = saturateAlias.find(srcId);
                    if (sIt == saturateAlias.end()) break;
                    if (saturatedResults.count(srcId)) saturate = true;
                    srcId = sIt->second;
                }

                // Determine if the target output is half-typed — that
                // promotes to MOVH / writes H0 instead of R0 (bit 7
                // OUT_REG_HALF + bit 22 PRECISION=FP16).  We only
                // detect this for the entry-point output parameter
                // carrying the matching semantic.
                bool halfOutput = false;
                for (const auto& p : entry.parameters)
                {
                    if (p.storage != StorageQualifier::Out) continue;
                    if (toUpper(p.semanticName) != semUpper)  continue;
                    if (p.semanticIndex != inst.semanticIndex) continue;
                    if (p.type.elementType == IRType::Float16) halfOutput = true;
                    break;
                }

                const struct nvfx_reg dstReg = halfOutput
                    ? [&]{ struct nvfx_reg r = nvfx_reg(NVFXSR_OUTPUT, outIndex); r.is_fp16 = 1; return r; }()
                    : nvfx_reg(NVFXSR_OUTPUT, outIndex);
                const uint8_t dstPrecision = halfOutput ? FLOAT16 : FLOAT32;
                const struct nvfx_reg none   = nvfx_reg(NVFXSR_NONE, 0);

                // CgBinaryFragmentProgram.outputFromH0: 1 iff the
                // result.color sink (output index 0) is written at
                // half precision (H0 rather than R0).
                if (halfOutput && outIndex == 0)
                    attrs.outputFromH0 = 1;

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
                        saturate ? 1 : 0, 0,
                        sampIt->second,  // tex_unit — packed into hw[0] bits 17-20
                        -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        src0, src1, src2);
                    in.precision = dstPrecision;
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

                // Literal vec4 going straight to an output: sce-cgc
                // emits FENCBR + MOV R_out, c[inline] + 16-byte block.
                auto lcIt = valueToLiteralVec4.find(srcId);
                if (lcIt != valueToLiteralVec4.end())
                {
                    asm_.emitFencbr();

                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    struct nvfx_src src0 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    struct nvfx_src src1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src src2 = nvfx_src(const_cast<struct nvfx_reg&>(none));

                    struct nvfx_insn in = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        src0, src1, src2);
                    in.precision = dstPrecision;
                    asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);

                    asm_.appendConstBlock(lcIt->second.vals);
                    emittedSomething = true;
                    break;
                }

                // MAD fusion — emit the MOVH / MOVR / FENCBR / MADR
                // four-block sequence.  Helper for the uniform-source
                // ucode-offset bookkeeping.
                auto recordUniformOffsetHere =
                    [&](IRValueID uValueId, uint32_t offset) -> bool
                {
                    auto uIt = valueToFpUniform.find(uValueId);
                    if (uIt == valueToFpUniform.end()) return false;
                    for (auto& eu : attrs.embeddedUniforms)
                    {
                        if (eu.entryParamIndex == uIt->second)
                        {
                            eu.ucodeByteOffsets.push_back(offset);
                            return true;
                        }
                    }
                    return false;
                };

                auto mdIt = valueToMad.find(srcId);
                if (mdIt != valueToMad.end())
                {
                    const auto& mb = mdIt->second;
                    auto inIt = valueToInputSrc.find(mb.inputId);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: MAD varying operand failed to resolve");
                        return out;
                    }

                    // MOVH H0, f[input]
                    struct nvfx_reg hDst = nvfx_reg(NVFXSR_TEMP, 0);
                    hDst.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1,
                        hDst, NVFX_FP_MASK_ALL, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    // Decide which uniform to preload into R1.  See
                    // the header comment on FpMadBinding.
                    const IRValueID preloadUniformId =
                        mb.addendIsLiteral ? mb.multiplierUniformId
                                           : mb.addendId;

                    // MOVR R1, preloadUniform + 16-byte zero block.
                    struct nvfx_reg rDst = nvfx_reg(NVFXSR_TEMP, 1);
                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    struct nvfx_src mvrS0 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    struct nvfx_src mvrS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvrS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mvr = nvfx_insn(
                        0, 0, -1, -1,
                        rDst, NVFX_FP_MASK_ALL, mvrS0, mvrS1, mvrS2);
                    asm_.emit(mvr, NVFX_FP_OP_OPCODE_MOV);

                    const uint32_t preloadOffset = asm_.currentByteSize();
                    recordUniformOffsetHere(preloadUniformId, preloadOffset);
                    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    asm_.appendConstBlock(zeros);

                    // FENCBR
                    asm_.emitFencbr();

                    // MADR R_out, H0, src1, src2
                    // - If addend is a LITERAL: src1 = R1 (multiplier), src2 = inline literal
                    // - If addend is a UNIFORM: src1 = CONST (multiplier inline), src2 = R1 (addend)
                    struct nvfx_src madS0 = nvfx_src(hDst);         // H0
                    struct nvfx_src madS1;
                    struct nvfx_src madS2;
                    if (mb.addendIsLiteral)
                    {
                        madS1 = nvfx_src(rDst);                     // R1 (multiplier preloaded)
                        madS2 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));  // CONST (inline literal)
                    }
                    else
                    {
                        madS1 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));  // CONST (multiplier inline)
                        madS2 = nvfx_src(rDst);                     // R1 (addend preloaded)
                    }

                    struct nvfx_insn mad = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        madS0, madS1, madS2);
                    mad.precision = dstPrecision;
                    asm_.emit(mad, NVFX_FP_OP_OPCODE_MAD);

                    // Inline block after MAD — either the literal or
                    // the (non-preloaded) uniform.
                    const uint32_t postMadOffset = asm_.currentByteSize();
                    if (mb.addendIsLiteral)
                    {
                        auto lIt = valueToLiteralVec4.find(mb.addendId);
                        asm_.appendConstBlock(lIt->second.vals);
                    }
                    else
                    {
                        recordUniformOffsetHere(mb.multiplierUniformId, postMadOffset);
                        asm_.appendConstBlock(zeros);
                    }

                    // Track varying-input mask bits.
                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    emittedSomething = true;
                    break;
                }

                // Arithmetic (ADD / MUL) with one varying + one uniform:
                // sce-cgc emits a single instruction with the uniform
                // inlined as a zero-initialised 16-byte block after it.
                // NO FENCBR here — sce-cgc only emits FENCBR before
                // pure const-reading MOVs, verified against add_f and
                // mul_f ucode byte probes.
                auto arIt = valueToArith.find(srcId);
                if (arIt != valueToArith.end())
                {
                    const auto& bind = arIt->second;
                    auto iIt = valueToInputSrc.find(bind.inputId);
                    auto uIt = valueToFpUniform.find(bind.uniformId);
                    if (iIt == valueToInputSrc.end() ||
                        uIt == valueToFpUniform.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: arithmetic operand failed to resolve");
                        return out;
                    }

                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    const struct nvfx_reg inputReg = nvfx_reg(NVFXSR_INPUT, iIt->second);

                    struct nvfx_src srcIn    = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src srcConst = nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    struct nvfx_src src2     = nvfx_src(const_cast<struct nvfx_reg&>(none));

                    srcIn.abs    = bind.inputAbs   ? 1 : 0;
                    srcIn.negate = bind.inputNeg   ? 1 : 0;
                    srcConst.abs    = bind.uniformAbs ? 1 : 0;
                    srcConst.negate = bind.uniformNeg ? 1 : 0;

                    uint8_t opcode = NVFX_FP_OP_OPCODE_ADD;
                    struct nvfx_src slot0 = srcConst;
                    struct nvfx_src slot1 = srcIn;
                    if (bind.op == FpArithOp::Add)
                    {
                        opcode = NVFX_FP_OP_OPCODE_ADD;
                        // ADD: src0=CONST, src1=INPUT (sce-cgc canonical)
                        slot0 = srcConst;
                        slot1 = srcIn;
                    }
                    else
                    {
                        // MUL / MIN / MAX share (src0=INPUT, src1=CONST)
                        // canonical placement — verified against sce-cgc.
                        switch (bind.op)
                        {
                        case FpArithOp::Min: opcode = NVFX_FP_OP_OPCODE_MIN; break;
                        case FpArithOp::Max: opcode = NVFX_FP_OP_OPCODE_MAX; break;
                        case FpArithOp::Mul:
                        default:             opcode = NVFX_FP_OP_OPCODE_MUL; break;
                        }
                        slot0 = srcIn;
                        slot1 = srcConst;
                    }

                    struct nvfx_insn in = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        slot0, slot1, src2);
                    in.precision = dstPrecision;
                    asm_.emit(in, opcode);

                    const uint32_t ucodeByteOffset = asm_.currentByteSize();
                    for (auto& eu : attrs.embeddedUniforms)
                    {
                        if (eu.entryParamIndex == uIt->second)
                        {
                            eu.ucodeByteOffsets.push_back(ucodeByteOffset);
                            break;
                        }
                    }

                    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    asm_.appendConstBlock(zeros);

                    // Track varying-input mask bits.
                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(iIt->second);
                    if (iIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        iIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (iIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    emittedSomething = true;
                    break;
                }

                // FP uniform consumed directly (MOV R_out, uniform).
                // Sce-cgc pattern:
                //   FENCBR ;
                //   MOVR R_out, c[inline] ;
                //   <16 bytes zero-initialised — runtime patches>
                auto uIt = valueToFpUniform.find(srcId);
                if (uIt != valueToFpUniform.end())
                {
                    asm_.emitFencbr();

                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    struct nvfx_src src0 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    struct nvfx_src src1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src src2 = nvfx_src(const_cast<struct nvfx_reg&>(none));

                    struct nvfx_insn in = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        src0, src1, src2);
                    in.precision = dstPrecision;
                    asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);

                    // Record the ucode offset of the inline-const
                    // block we're about to append (4 u32 per block).
                    const uint32_t ucodeByteOffset = asm_.currentByteSize();
                    for (auto& eu : attrs.embeddedUniforms)
                    {
                        if (eu.entryParamIndex == uIt->second)
                        {
                            eu.ucodeByteOffsets.push_back(ucodeByteOffset);
                            break;
                        }
                    }

                    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    asm_.appendConstBlock(zeros);
                    emittedSomething = true;
                    break;
                }

                // Resolve abs/neg modifiers on the direct-MOV path so
                // `color = abs(vcol)` / `color = -vcol` emit correctly.
                SrcMod mods = resolveSrcMods(srcId);
                auto it = valueToInputSrc.find(mods.baseId);
                if (it == valueToInputSrc.end())
                {
                    out.diagnostics.push_back(
                        "nv40-fp: StoreOutput source is not a direct varying input, "
                        "tex2D result, or literal vec4 (arithmetic lowering lands later)");
                    return out;
                }

                const struct nvfx_reg srcReg =
                    nvfx_reg(NVFXSR_INPUT, it->second);

                struct nvfx_src src0 = nvfx_src(const_cast<struct nvfx_reg&>(srcReg));
                struct nvfx_src src1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                struct nvfx_src src2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                src0.abs    = mods.absMod ? 1 : 0;
                src0.negate = mods.negMod ? 1 : 0;

                struct nvfx_insn in = nvfx_insn(
                    saturate ? 1 : 0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dstReg),
                    NVFX_FP_MASK_ALL,
                    src0, src1, src2);
                // MOV with a source modifier: sce-cgc emits it at
                // FP16 precision (MOVH variant) even when the dst
                // stays R0.  Verified via abs_input_f / neg_input_f
                // byte probes — both produce PRECISION=1 (FP16).
                in.precision = (mods.absMod || mods.negMod) ? FLOAT16 : dstPrecision;
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
