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
        IRValueID samplerId  = 0;
        IRValueID uvId       = 0;
        bool      projective = false;  // true → emit TXP (0x18) instead of TEX (0x17)
        bool      cube       = false;  // true → sampler is samplerCUBE; sets
                                       //         DISABLE_PC bit 31 on hw[3]
    };

    // IR value → type, for samplers we populate from the param list
    // (already done via `valueToType` seeded above).  The TexSample
    // handler reads `samplerId`'s type to decide TEX vs cube.
    std::unordered_map<IRValueID, TexBinding> valueToTex;

    // Deferred arithmetic op with one INPUT (varying) operand and one
    // FP-uniform operand.  Emit at the point of consumption by a
    // StoreOutput so the arithmetic result can flow straight to the
    // output register.  Per sce-cgc byte probes:
    //   - ADD emits src0=CONST(uniform), src1=INPUT(varying)
    //   - MUL emits src0=INPUT(varying), src1=CONST(uniform)
    //   (operand order in C source is ignored — sce-cgc canonicalises)
    // Per sce-cgc byte probes, all (input, uniform) binary opcodes
    // canonicalise the same way:
    //   ADD:                src0 = CONST (uniform), src1 = INPUT
    //   MUL/MIN/MAX/DP3/DP4: src0 = INPUT,          src1 = CONST (uniform)
    enum class FpArithOp { Add, Mul, Min, Max, Dot3, Dot4 };
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

    // Lookup map for value → type info.  IRFunction::getValue() only
    // returns stored values (constants + parameters), not instruction
    // results; we fill this as we walk the block so later handlers
    // (e.g. Dot choosing DP3 vs DP4) can probe the producer's type.
    std::unordered_map<IRValueID, IRTypeInfo> valueToType;
    for (const auto& p : entry.parameters)
        valueToType[p.valueId] = p.type;

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

    // Comparisons and ternary select:
    //
    //   `(vcol.x > 0.5f) ? a : b`
    //
    // sce-cgc lowers this to three instructions + two inline blocks:
    //   SGTRC RC.x, f[COL0], {0.5, 0, 0, 0}.x ;  # SGT writes CC, no reg
    //   <inline {0.5, 0, 0, 0}>
    //   MOVR R0, a ;                              # unconditional true case
    //   <inline zero for uniform a>
    //   MOVR R0(EQ.x), b ;                        # conditional over CC
    //   <inline zero for uniform b>
    //
    // Encoding deltas vs regular MOV/ADD:
    //   - SGT has OUT_NONE + COND_WRITE_ENABLE + OUT_REG=0x3F (sentinel)
    //     and OUTMASK=0x1 (only CC.x).  Source 1 (the scalar literal) is
    //     swizzled .xxxx.
    //   - The conditional MOV has cc_cond=EQ and cc_swz=(X,X,X,X)
    //     overriding the default TR / identity.
    struct ScalarExtract { IRValueID baseId = 0; int lane = 0; };
    std::unordered_map<IRValueID, ScalarExtract> valueToScalarExtract;

    struct CmpBinding
    {
        uint8_t   opcode      = NVFX_FP_OP_OPCODE_SGT;
        IRValueID lhsBase     = 0;
        int       lhsLane     = 0;
        // RHS shape:
        //   Literal: inline {rhsLiteral, 0, 0, 0} as a const block
        //   Uniform: append zero block, record ucode offset for rhsUniformId
        //   Varying: no inline block — RHS feeds through SRC1 as a
        //            second INPUT varying; needs a MOVH promote of the
        //            LHS lane into an H-reg temp because NV40 FP can
        //            only route ONE per-instruction INPUT varying.
        enum class RhsKind { Literal, Uniform, Varying };
        RhsKind   rhsKind        = RhsKind::Literal;
        float     rhsLiteral     = 0.0f;
        IRValueID rhsUniformId   = 0;
        IRValueID rhsVaryingBase = 0;
        int       rhsVaryingLane = 0;
    };
    std::unordered_map<IRValueID, CmpBinding> valueToCmp;

    struct SelectBinding
    {
        IRValueID cmpId    = 0;
        IRValueID trueId   = 0;  // value when condition is TRUE
        IRValueID falseId  = 0;
    };
    std::unordered_map<IRValueID, SelectBinding> valueToSelect;

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

    // Scalar-function-unit unary ops (RCP / RSQ).  Both read one
    // scalar lane of a varying and broadcast the result to all four
    // output lanes.  sce-cgc emits the 2-insn shape:
    //     MOVH H0.<lane>, f[input];
    //     <OP>R R0, H0;
    // regardless of whether the source expression was written as
    // `1.0/x` (Cg Div with lhs=1) or as `rsqrt(x)`; we normalise
    // both into this binding.
    struct FpScalarUnaryBinding
    {
        enum class Kind { Rcp, Rsq };
        Kind      kind     = Kind::Rcp;
        IRValueID inputId  = 0;
        int       lane     = 0;  // 0..3 — which lane of the input to read
    };
    std::unordered_map<IRValueID, FpScalarUnaryBinding> valueToScalarUnary;

    // `length(vec3)` — sce-cgc lowers to MOVH + DP3R + DIVSQR + MOVR.
    // DIVSQR (Sony NV40+RSX extension, opcode 0x3B) computes
    // `src0 / sqrt(src1)`; sce-cgc's sqrt(dot(v,v)) trick reads back
    // the same DP3R result as both `|src0|` and `src1`, exploiting
    // `|x|/sqrt(x) == sqrt(x)` for x >= 0.
    struct FpLengthBinding
    {
        IRValueID inputId = 0;
        int       lanes   = 3;   // typically 3 (vec3 length)
    };
    std::unordered_map<IRValueID, FpLengthBinding> valueToLength;

    // `normalize(vec3)` — sce-cgc lowers to MOVH + DP3R + MOVR(w=1) +
    // DIVSQR.  The w=1 MOV fires mid-sequence so DIVSQR is the END-
    // carrying last instruction.  The binding captures the base
    // varying and optional "wrap with w=1" flag so StoreOutput can
    // emit either shape: bare normalize (no w inject) or the more
    // common `float4(normalize(v), 1.0)` pattern.
    struct FpNormalizeBinding
    {
        IRValueID inputId  = 0;
        int       lanes    = 3;
        bool      wrapW1   = false;  // true iff StoreOutput sees
                                     // `float4(normalize(v), 1.0)`
    };
    std::unordered_map<IRValueID, FpNormalizeBinding> valueToNormalize;

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

            // Record the instruction's result type so downstream
            // lowerings can look it up (e.g. Dot uses the operand's
            // vectorSize to pick DP3 vs DP4).
            if (inst.result != InvalidIRValue)
                valueToType[inst.result] = inst.resultType;

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
            case IROp::TexSampleProj:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-fp: TexSample with <2 operands");
                    return out;
                }
                // tex2D(sampler, uv) / tex2Dproj / texCUBE.
                // operands[0] = sampler, [1] = uv.
                TexBinding tb;
                tb.samplerId  = inst.operands[0];
                tb.uvId       = inst.operands[1];
                tb.projective = (inst.op == IROp::TexSampleProj);

                // Look up the sampler's type to detect cube.
                auto sTyIt = valueToType.find(tb.samplerId);
                if (sTyIt != valueToType.end() &&
                    sTyIt->second.baseType == IRType::SamplerCube)
                {
                    tb.cube = true;
                }

                valueToTex[inst.result] = tb;
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

            case IROp::VecShuffle:
            {
                if (inst.operands.size() != 1) break;
                const int outLanes = inst.resultType.vectorSize;

                // Scalar extract (vcol.x, vcol.y, ...): resultType is
                // a scalar and the mask's low 2 bits name the source
                // lane.  Used by comparisons (`x > 0.5`) and scalar
                // math on vec components.
                if (outLanes == 1)
                {
                    ScalarExtract se;
                    se.baseId = inst.operands[0];
                    se.lane   = inst.swizzleMask & 3;
                    valueToScalarExtract[inst.result] = se;
                    break;
                }

                // Identity-prefix shuffle (e.g. `.xyz` from a vec4 →
                // vec3, `.xy` → vec2).  No-op at the NV40 level
                // because the narrower destination op (DP3, etc.)
                // naturally ignores trailing lanes.  Non-identity
                // vector shuffles need SRC-slot swizzle bit plumbing;
                // lands in a later stage.
                bool identity = true;
                for (int k = 0; k < outLanes && k < 4; ++k)
                {
                    const int idx = (inst.swizzleMask >> (k * 2)) & 3;
                    if (idx != k) { identity = false; break; }
                }
                if (!identity) break;   // leave unlowered — caller will error
                // Alias with no modifier flags: pass-through.
                SrcMod m;
                m.baseId = inst.operands[0];
                valueToMod[inst.result] = m;
                break;
            }

            case IROp::CmpGt:
            case IROp::CmpGe:
            case IROp::CmpLt:
            case IROp::CmpLe:
            case IROp::CmpEq:
            case IROp::CmpNe:
            {
                if (inst.operands.size() < 2) break;

                // LHS: scalar extract of a varying (for `vcol.x > k`).
                auto seIt = valueToScalarExtract.find(inst.operands[0]);
                if (seIt == valueToScalarExtract.end()) break;
                auto inIt = valueToInputSrc.find(seIt->second.baseId);
                if (inIt == valueToInputSrc.end()) break;

                // RHS: either a float constant or an FP uniform.
                CmpBinding cb;
                switch (inst.op)
                {
                case IROp::CmpGt: cb.opcode = NVFX_FP_OP_OPCODE_SGT; break;
                case IROp::CmpGe: cb.opcode = NVFX_FP_OP_OPCODE_SGE; break;
                case IROp::CmpLt: cb.opcode = NVFX_FP_OP_OPCODE_SLT; break;
                case IROp::CmpLe: cb.opcode = NVFX_FP_OP_OPCODE_SLE; break;
                case IROp::CmpEq: cb.opcode = NVFX_FP_OP_OPCODE_SEQ; break;
                case IROp::CmpNe: cb.opcode = NVFX_FP_OP_OPCODE_SNE; break;
                default: break;
                }
                cb.lhsBase = seIt->second.baseId;
                cb.lhsLane = seIt->second.lane;

                IRValue* rhsValue = entry.getValue(inst.operands[1]);
                if (auto* rhsConst = dynamic_cast<IRConstant*>(rhsValue);
                    rhsConst && std::holds_alternative<float>(rhsConst->value))
                {
                    cb.rhsKind    = CmpBinding::RhsKind::Literal;
                    cb.rhsLiteral = std::get<float>(rhsConst->value);
                }
                else if (auto uIt = valueToFpUniform.find(inst.operands[1]);
                         uIt != valueToFpUniform.end())
                {
                    cb.rhsKind      = CmpBinding::RhsKind::Uniform;
                    cb.rhsUniformId = inst.operands[1];
                }
                else if (auto rhsSe = valueToScalarExtract.find(inst.operands[1]);
                         rhsSe != valueToScalarExtract.end())
                {
                    auto rhsBaseIn = valueToInputSrc.find(rhsSe->second.baseId);
                    if (rhsBaseIn == valueToInputSrc.end()) break;
                    cb.rhsKind         = CmpBinding::RhsKind::Varying;
                    cb.rhsVaryingBase  = rhsSe->second.baseId;
                    cb.rhsVaryingLane  = rhsSe->second.lane;
                }
                else
                {
                    // Unknown shape — leave unlowered; Select emit
                    // will diagnose.
                    break;
                }
                valueToCmp[inst.result] = cb;
                break;
            }

            case IROp::Select:
            {
                if (inst.operands.size() < 3) break;
                SelectBinding sb;
                sb.cmpId   = inst.operands[0];
                sb.trueId  = inst.operands[1];
                sb.falseId = inst.operands[2];
                valueToSelect[inst.result] = sb;
                break;
            }

            case IROp::Dot:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-fp: Dot with <2 operands");
                    return out;
                }

                const SrcMod ma = resolveSrcMods(inst.operands[0]);
                const SrcMod mb = resolveSrcMods(inst.operands[1]);

                auto aInput   = valueToInputSrc.find(ma.baseId);
                auto bInput   = valueToInputSrc.find(mb.baseId);
                auto aUniform = valueToFpUniform.find(ma.baseId);
                auto bUniform = valueToFpUniform.find(mb.baseId);

                // Determine DP3 vs DP4 from the operand's original
                // (pre-resolve) IR type — the .xyz shuffle narrows
                // the type to vec3 so DP3 is correct even when the
                // underlying base is a vec4 varying.
                auto tyIt = valueToType.find(inst.operands[0]);
                const int lanes =
                    (tyIt != valueToType.end()) ? tyIt->second.vectorSize : 4;
                const FpArithOp dotOp =
                    (lanes == 3) ? FpArithOp::Dot3 : FpArithOp::Dot4;

                FpArithBinding bind;
                bind.op = dotOp;
                if (aInput != valueToInputSrc.end() && bUniform != valueToFpUniform.end())
                {
                    bind.inputId    = ma.baseId;
                    bind.inputAbs   = ma.absMod;
                    bind.inputNeg   = ma.negMod;
                    bind.uniformId  = mb.baseId;
                    bind.uniformAbs = mb.absMod;
                    bind.uniformNeg = mb.negMod;
                }
                else if (aUniform != valueToFpUniform.end() && bInput != valueToInputSrc.end())
                {
                    bind.inputId    = mb.baseId;
                    bind.inputAbs   = mb.absMod;
                    bind.inputNeg   = mb.negMod;
                    bind.uniformId  = ma.baseId;
                    bind.uniformAbs = ma.absMod;
                    bind.uniformNeg = ma.negMod;
                }
                else
                {
                    out.diagnostics.push_back(
                        "nv40-fp: Dot supported only for (varying, uniform) pairs today");
                    return out;
                }
                valueToArith[inst.result] = bind;
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

            case IROp::RSqrt:
            {
                // `rsqrt(scalar_lane_of_varying)`.  IR shape:
                //   %a = shuffle float %v   (ScalarExtract{v, lane})
                //   %b = rsqrt float %a
                if (inst.operands.size() != 1) break;
                auto seIt = valueToScalarExtract.find(inst.operands[0]);
                if (seIt == valueToScalarExtract.end()) break;

                FpScalarUnaryBinding bind;
                bind.kind    = FpScalarUnaryBinding::Kind::Rsq;
                bind.inputId = seIt->second.baseId;
                bind.lane    = seIt->second.lane;
                valueToScalarUnary[inst.result] = bind;
                break;
            }

            case IROp::Div:
            {
                // Recognise `1.0 / <scalar>` as reciprocal — sce-cgc
                // constant-folds Cg's `1.0/x` expression to RCP.  The
                // scalar RHS must be a ScalarExtract (`v.x`, `v.y`, …)
                // so we know which lane to pre-load into H0.
                if (inst.operands.size() != 2) break;
                IRValue* lhs = entry.getValue(inst.operands[0]);
                auto* c = dynamic_cast<IRConstant*>(lhs);
                if (!c || !std::holds_alternative<float>(c->value))  break;
                if (std::get<float>(c->value) != 1.0f)               break;

                auto seIt = valueToScalarExtract.find(inst.operands[1]);
                if (seIt == valueToScalarExtract.end())              break;

                FpScalarUnaryBinding bind;
                bind.kind    = FpScalarUnaryBinding::Kind::Rcp;
                bind.inputId = seIt->second.baseId;
                bind.lane    = seIt->second.lane;
                valueToScalarUnary[inst.result] = bind;
                break;
            }

            case IROp::Length:
            {
                // `length(vec3)` — resolve the input through SrcMod so
                // identity-prefix shuffles (`v.xyz`) pass through
                // transparently.  Lanes come from the shuffle's result
                // type (same mechanism as Dot's DP3/DP4 pick).
                if (inst.operands.size() != 1) break;
                const SrcMod m = resolveSrcMods(inst.operands[0]);
                if (valueToInputSrc.find(m.baseId) == valueToInputSrc.end())
                    break;  // non-varying length() lands in a later pass

                int lanes = 3;
                auto tyIt = valueToType.find(inst.operands[0]);
                if (tyIt != valueToType.end() && tyIt->second.vectorSize > 0)
                    lanes = tyIt->second.vectorSize;

                FpLengthBinding bind;
                bind.inputId = m.baseId;
                bind.lanes   = lanes;
                valueToLength[inst.result] = bind;
                break;
            }

            case IROp::Normalize:
            {
                // `normalize(vec3)` — same input-resolution path as
                // Length.  The `wrapW1` flag gets set later when
                // VecConstruct sees this feeding into `float4(nrm, 1)`.
                if (inst.operands.size() != 1) break;
                const SrcMod m = resolveSrcMods(inst.operands[0]);
                if (valueToInputSrc.find(m.baseId) == valueToInputSrc.end())
                    break;

                int lanes = 3;
                auto tyIt = valueToType.find(inst.operands[0]);
                if (tyIt != valueToType.end() && tyIt->second.vectorSize > 0)
                    lanes = tyIt->second.vectorSize;

                FpNormalizeBinding bind;
                bind.inputId = m.baseId;
                bind.lanes   = lanes;
                valueToNormalize[inst.result] = bind;
                break;
            }

            case IROp::VecConstruct:
            {
                // Shape A: `float4(normalize(vec3), 1.0f)` — 2 operands
                // where operand 0 is a Normalize result and operand 1
                // is the scalar constant 1.0.  Fold into the
                // normalize binding with wrapW1=true so StoreOutput
                // emits sce-cgc's MOVH/DP3/MOVR-w/DIVSQR sequence.
                if (inst.operands.size() == 2)
                {
                    auto nIt = valueToNormalize.find(inst.operands[0]);
                    if (nIt != valueToNormalize.end())
                    {
                        IRValue* wv = entry.getValue(inst.operands[1]);
                        auto* wc = dynamic_cast<IRConstant*>(wv);
                        if (wc && std::holds_alternative<float>(wc->value) &&
                            std::get<float>(wc->value) == 1.0f)
                        {
                            FpNormalizeBinding bind = nIt->second;
                            bind.wrapW1 = true;
                            valueToNormalize[inst.result] = bind;
                            break;
                        }
                    }
                }

                // Shape B: `float4(k0, k1, k2, k3)` where every kN is
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

                    const uint8_t texOpcode =
                        texIt->second.projective
                            ? NVFX_FP_OP_OPCODE_TXP
                            : NVFX_FP_OP_OPCODE_TEX;

                    struct nvfx_insn in = nvfx_insn(
                        saturate ? 1 : 0, 0,
                        sampIt->second,  // tex_unit — packed into hw[0] bits 17-20
                        -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        src0, src1, src2);
                    in.precision = dstPrecision;
                    // samplerCUBE: the HW needs to know the input is a
                    // 3-component direction, not a 2D (u,v).  sce-cgc
                    // signals that by setting DISABLE_PC (bit 31 of
                    // hw[3]) — via nvfx_insn's `disable_pc` field.
                    if (texIt->second.cube)
                        in.disable_pc = 1;
                    asm_.emit(in, texOpcode);
                    emittedSomething = true;

                    // Track varying input usage for the container's
                    // attributeInputMask + texCoordsInputMask fields.
                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(uvIt->second);
                    if (uvIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        uvIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        const int n = uvIt->second - NVFX_FP_OP_INPUT_SRC_TC(0);
                        attrs.texCoordsInputMask |= uint16_t{1} << n;
                        // sce-cgc clears the texCoords2D bit for any
                        // TEXCOORD used projectively or as a cube-map
                        // direction — the HW needs more than just xy
                        // (perspective divide for TXP; 3D direction
                        // for cube).
                        if (texIt->second.projective || texIt->second.cube)
                            attrs.texCoords2D &= ~(uint16_t{1} << n);
                    }
                    break;
                }

                // Ternary select `(cond) ? trueV : falseV`:
                //   SGT RC.x, <input>.<lane>, {cmpConst, 0, 0, 0}.x
                //   <inline {cmpConst, 0, 0, 0}>
                //   MOVR R_out, trueV
                //   <inline trueV (zero block for uniform)>
                //   MOVR R_out(EQ.x), falseV
                //   <inline falseV>
                auto selIt = valueToSelect.find(srcId);
                if (selIt != valueToSelect.end())
                {
                    const auto& sb = selIt->second;
                    auto cmpIt = valueToCmp.find(sb.cmpId);
                    if (cmpIt == valueToCmp.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: Select needs a CmpGt/CmpGe/... constant-threshold binding");
                        return out;
                    }
                    const auto& cb = cmpIt->second;
                    auto inIt = valueToInputSrc.find(cb.lhsBase);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: Select: comparison LHS must be a scalar of a varying");
                        return out;
                    }

                    // Each branch may be a uniform, a literal vec4, or
                    // a varying.  We enumerate the shape per branch;
                    // emit dispatches later.
                    enum class BranchKind { Uniform, Literal, Varying, Unknown };
                    struct BranchInfo
                    {
                        BranchKind  kind        = BranchKind::Unknown;
                        unsigned    uniformIdx  = 0;    // entry param index (Uniform)
                        const float* literalVals = nullptr;  // (Literal)
                        int         inputSrc    = 0;    // NVFX INPUT_SRC_* (Varying)
                    };
                    auto classifyBranch = [&](IRValueID id) -> BranchInfo
                    {
                        BranchInfo bi;
                        if (auto uIt = valueToFpUniform.find(id);
                            uIt != valueToFpUniform.end())
                        {
                            bi.kind = BranchKind::Uniform;
                            bi.uniformIdx = uIt->second;
                        }
                        else if (auto lIt = valueToLiteralVec4.find(id);
                                 lIt != valueToLiteralVec4.end())
                        {
                            bi.kind = BranchKind::Literal;
                            bi.literalVals = lIt->second.vals;
                        }
                        else if (auto vIt = valueToInputSrc.find(id);
                                 vIt != valueToInputSrc.end())
                        {
                            bi.kind = BranchKind::Varying;
                            bi.inputSrc = vIt->second;
                        }
                        return bi;
                    };
                    BranchInfo trueBr  = classifyBranch(sb.trueId);
                    BranchInfo falseBr = classifyBranch(sb.falseId);
                    if (trueBr.kind  == BranchKind::Unknown ||
                        falseBr.kind == BranchKind::Unknown)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: Select: branches must be uniforms, literal vec4s, or varyings");
                        return out;
                    }
                    // Both-literal sce-cgc uses a swizzle-share trick
                    // we don't reproduce yet.  Literal trueBr plus any
                    // other branch is fine — sce-cgc just lays the
                    // literal out as a fresh inline block.
                    if (trueBr.kind == BranchKind::Literal &&
                        falseBr.kind == BranchKind::Literal)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: Select: both-literal branches need a "
                            "swizzle-sharing trick we don't emit yet");
                        return out;
                    }

                    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};

                    auto recordUniformOffset =
                        [&](unsigned paramIdx, uint32_t offset)
                    {
                        for (auto& eu : attrs.embeddedUniforms)
                            if (eu.entryParamIndex == paramIdx)
                            { eu.ucodeByteOffsets.push_back(offset); break; }
                    };

                    // Helper: emit the post-instruction inline block
                    // for a branch value at its appropriate ucode
                    // offset.  Uniforms get a zero block + offset
                    // record; literals get the raw 4-float inline.
                    // Varyings don't have a tail block (they feed via
                    // INPUT_SRC / SRC0).  Must be called IMMEDIATELY
                    // after the emit that reads the value.
                    auto appendBranchTail = [&](const BranchInfo& bi)
                    {
                        if (bi.kind == BranchKind::Uniform)
                        {
                            recordUniformOffset(bi.uniformIdx, asm_.currentByteSize());
                            asm_.appendConstBlock(zeros);
                        }
                        else if (bi.kind == BranchKind::Literal)
                        {
                            asm_.appendConstBlock(bi.literalVals);
                        }
                    };

                    auto maskTcBit = [&](int inputSrc)
                    {
                        if (inputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            inputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = inputSrc - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoordsInputMask |= uint16_t{1} << n;
                        }
                    };

                    // ============ Varying-RHS path ============
                    // Sce-cgc reshapes the compare for a varying RHS
                    // because NV40 FP can only route ONE INPUT varying
                    // per instruction.  The LHS varying gets promoted
                    // to an H-reg temp via MOVH (with per-lane
                    // writemask), then the RHS varying is read via
                    // SRC1=INPUT on the SGT.  Sequence:
                    //   MOVH H2.x, f[lhs_input]        (fp16 promote)
                    //   MOVR R0, trueUniform           (preload moved early)
                    //   <inline zero block for trueUniform>
                    //   SGTRC RC.x, H2, f[rhs_input]
                    //   FENCBR
                    //   MOVR R0(EQ.x), falseUniform    (conditional overwrite)
                    //   <inline zero block for falseUniform>
                    //
                    // H2 chosen because H0/H1 alias R0 which we need
                    // to write as the final output.
                    if (cb.rhsKind == CmpBinding::RhsKind::Varying)
                    {
                        if (trueBr.kind != BranchKind::Uniform ||
                            falseBr.kind != BranchKind::Uniform)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select: varying-RHS compare currently "
                                "requires both branches to be uniforms");
                            return out;
                        }
                        auto rhsIn = valueToInputSrc.find(cb.rhsVaryingBase);
                        if (rhsIn == valueToInputSrc.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select varying RHS did not resolve to an input");
                            return out;
                        }

                        // --- MOVH H2.x, f[lhs_input] ---
                        struct nvfx_reg h2Dst = nvfx_reg(NVFXSR_TEMP, 2);
                        h2Dst.is_fp16 = 1;
                        const struct nvfx_reg lhsIn = nvfx_reg(NVFXSR_INPUT, inIt->second);
                        struct nvfx_src movhS0 = nvfx_src(const_cast<struct nvfx_reg&>(lhsIn));
                        if (cb.lhsLane != 0)
                        {
                            movhS0.swz[0] = movhS0.swz[1] = movhS0.swz[2] = movhS0.swz[3] =
                                static_cast<uint8_t>(cb.lhsLane);
                        }
                        struct nvfx_src movhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src movhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn movh = nvfx_insn(
                            0, 0, -1, -1, h2Dst,
                            NVFX_FP_MASK_X, movhS0, movhS1, movhS2);
                        movh.precision = FLOAT16;
                        asm_.emit(movh, NVFX_FP_OP_OPCODE_MOV);

                        // --- MOVR R0, trueUniform (preload) ---
                        const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                        struct nvfx_src preS0 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        struct nvfx_src preS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src preS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn preIn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_ALL, preS0, preS1, preS2);
                        preIn.precision = dstPrecision;
                        asm_.emit(preIn, NVFX_FP_OP_OPCODE_MOV);
                        recordUniformOffset(trueBr.uniformIdx, asm_.currentByteSize());
                        asm_.appendConstBlock(zeros);

                        // --- SGT RC.x, H2, f[rhs_input] ---
                        const struct nvfx_reg ccDst = nvfx_reg(NVFXSR_NONE, 0x3F);
                        // SRC0 = H2 (TEMP idx 2 with HALF)
                        struct nvfx_reg h2Src = nvfx_reg(NVFXSR_TEMP, 2);
                        h2Src.is_fp16 = 1;
                        struct nvfx_src cS0 = nvfx_src(h2Src);
                        // SRC1 = INPUT (the RHS varying's attribute index
                        // will be driven by INPUT_SRC on hw[0]).  No
                        // per-lane swizzle needed for lane 0; replicate
                        // otherwise.
                        const struct nvfx_reg rhsIn_reg = nvfx_reg(NVFXSR_INPUT, rhsIn->second);
                        struct nvfx_src cS1 = nvfx_src(const_cast<struct nvfx_reg&>(rhsIn_reg));
                        if (cb.rhsVaryingLane != 0)
                        {
                            cS1.swz[0] = cS1.swz[1] = cS1.swz[2] = cS1.swz[3] =
                                static_cast<uint8_t>(cb.rhsVaryingLane);
                        }
                        struct nvfx_src cS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn cmpV = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(ccDst),
                            NVFX_FP_MASK_X, cS0, cS1, cS2);
                        cmpV.cc_update = 1;
                        asm_.emit(cmpV, cb.opcode);

                        // --- FENCBR ---
                        asm_.emitFencbr();

                        // --- MOVR R0(EQ.x), falseUniform ---
                        struct nvfx_src fS0 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        struct nvfx_src fS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src fS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn mvFalseV = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_ALL, fS0, fS1, fS2);
                        mvFalseV.precision = dstPrecision;
                        mvFalseV.cc_test  = 1;
                        mvFalseV.cc_cond  = NVFX_FP_OP_COND_EQ;
                        mvFalseV.cc_swz[0] = 0;
                        mvFalseV.cc_swz[1] = 0;
                        mvFalseV.cc_swz[2] = 0;
                        mvFalseV.cc_swz[3] = 0;
                        asm_.emit(mvFalseV, NVFX_FP_OP_OPCODE_MOV);
                        recordUniformOffset(falseBr.uniformIdx, asm_.currentByteSize());
                        asm_.appendConstBlock(zeros);

                        // Container flags: both LHS and RHS varyings
                        // contribute to attributeInputMask; for TC
                        // lanes, also update texCoordsInputMask.
                        // sce-cgc clears texCoords2D for any TC used
                        // as a comparison source (not as a 2D uv).
                        attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(inIt->second);
                        attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(rhsIn->second);
                        maskTcBit(inIt->second);
                        maskTcBit(rhsIn->second);
                        if (rhsIn->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            rhsIn->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = rhsIn->second - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoords2D &= ~(uint16_t{1} << n);
                        }

                        emittedSomething = true;
                        break;
                    }

                    // --- SGT/SGE/... RC.x, varying.<lane>, {lit}.x ---
                    const struct nvfx_reg ccDst = nvfx_reg(NVFXSR_NONE, 0x3F);
                    const struct nvfx_reg inReg = nvfx_reg(NVFXSR_INPUT, inIt->second);
                    const struct nvfx_reg coReg = nvfx_reg(NVFXSR_CONST, 0);

                    struct nvfx_src cmpS0 = nvfx_src(const_cast<struct nvfx_reg&>(inReg));
                    // For `vcol.<lane> > k`, OUTMASK stays at .x (the
                    // scalar compare still writes only CC.x) and the
                    // conditional-MOV's cc_swz stays at (X,X,X,X) —
                    // BUT we route the chosen lane of the input into
                    // SRC0.x by setting the source swizzle to
                    // (lane, lane, lane, lane).  For the X case
                    // specifically, sce-cgc keeps the default identity
                    // swizzle (.xyzw); only non-X lanes replicate.
                    if (cb.lhsLane != 0)
                    {
                        cmpS0.swz[0] = cmpS0.swz[1] =
                        cmpS0.swz[2] = cmpS0.swz[3] =
                            static_cast<uint8_t>(cb.lhsLane);
                    }
                    struct nvfx_src cmpS1 = nvfx_src(const_cast<struct nvfx_reg&>(coReg));
                    cmpS1.swz[0] = cmpS1.swz[1] = cmpS1.swz[2] = cmpS1.swz[3] = 0;  // .x
                    struct nvfx_src cmpS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));

                    struct nvfx_insn cmpInsn = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(ccDst),
                        NVFX_FP_MASK_X,    // only CC.x
                        cmpS0, cmpS1, cmpS2);
                    cmpInsn.cc_update = 1;     // COND_WRITE_ENABLE
                    asm_.emit(cmpInsn, cb.opcode);

                    // RHS inline block — either a literal vec4 (with
                    // the threshold in .x) or a zero-initialised block
                    // patched by the runtime with the uniform's value.
                    if (cb.rhsKind == CmpBinding::RhsKind::Literal)
                    {
                        const float cmpLit[4] =
                            { cb.rhsLiteral, 0.0f, 0.0f, 0.0f };
                        asm_.appendConstBlock(cmpLit);
                    }
                    else  // RhsKind::Uniform
                    {
                        auto rhsUIt = valueToFpUniform.find(cb.rhsUniformId);
                        if (rhsUIt == valueToFpUniform.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select RHS uniform did not resolve");
                            return out;
                        }
                        const uint32_t rhsOffset = asm_.currentByteSize();
                        for (auto& eu : attrs.embeddedUniforms)
                            if (eu.entryParamIndex == rhsUIt->second)
                            { eu.ucodeByteOffsets.push_back(rhsOffset); break; }
                        asm_.appendConstBlock(zeros);
                    }

                    // Decide which branch goes in the unconditional
                    // (preload) MOV vs the conditional MOV — varyings
                    // can't be preloaded (they're read via SRC0=INPUT,
                    // no inline block), so they always ride the
                    // conditional slot.  Flip the test code when the
                    // trueBranch is the varying — the MOV with CC
                    // tests NE (condition-was-true) instead of EQ.
                    BranchInfo preloadBr      = trueBr;
                    BranchInfo conditionalBr  = falseBr;
                    uint8_t    condCode       = NVFX_FP_OP_COND_EQ;
                    if (trueBr.kind == BranchKind::Varying &&
                        falseBr.kind != BranchKind::Varying)
                    {
                        preloadBr     = falseBr;
                        conditionalBr = trueBr;
                        condCode      = NVFX_FP_OP_COND_NE;
                    }
                    else if (trueBr.kind == BranchKind::Varying &&
                             falseBr.kind == BranchKind::Varying)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: Select: two varying branches not yet supported");
                        return out;
                    }

                    auto makeBranchSrc0 = [&](const BranchInfo& bi,
                                              uint32_t* hw0InputSrcOut) -> struct nvfx_src
                    {
                        // Reset INPUT_SRC output; only Varying sets it.
                        if (hw0InputSrcOut) *hw0InputSrcOut = 0;
                        if (bi.kind == BranchKind::Varying)
                        {
                            if (hw0InputSrcOut) *hw0InputSrcOut = 1;
                            struct nvfx_reg r = nvfx_reg(NVFXSR_INPUT, bi.inputSrc);
                            return nvfx_src(r);
                        }
                        // Uniform or Literal → CONST source slot 0.
                        struct nvfx_reg r = nvfx_reg(NVFXSR_CONST, 0);
                        return nvfx_src(r);
                    };

                    // --- MOVR R_out, preloadBr (unconditional) ---
                    uint32_t preIs = 0;
                    struct nvfx_src mvS0 = makeBranchSrc0(preloadBr, &preIs);
                    struct nvfx_src mvS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mvPre = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        mvS0, mvS1, mvS2);
                    mvPre.precision = dstPrecision;
                    asm_.emit(mvPre, NVFX_FP_OP_OPCODE_MOV);
                    appendBranchTail(preloadBr);
                    if (preloadBr.kind == BranchKind::Varying)
                    {
                        attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(preloadBr.inputSrc);
                        if (preloadBr.inputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            preloadBr.inputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = preloadBr.inputSrc - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoordsInputMask |= uint16_t{1} << n;
                            attrs.texCoords2D &= ~(uint16_t{1} << n);
                        }
                    }

                    // --- MOVR R_out(COND.x), conditionalBr ---
                    uint32_t condIs = 0;
                    struct nvfx_src fS0 = makeBranchSrc0(conditionalBr, &condIs);
                    struct nvfx_src fS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src fS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mvCond = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        fS0, fS1, fS2);
                    mvCond.precision = dstPrecision;
                    mvCond.cc_test   = 1;
                    mvCond.cc_cond   = condCode;
                    // cc_swz is always (X,X,X,X) — SGT writes CC.x
                    // regardless of lhs lane (OUTMASK stays .x).
                    mvCond.cc_swz[0] = 0;
                    mvCond.cc_swz[1] = 0;
                    mvCond.cc_swz[2] = 0;
                    mvCond.cc_swz[3] = 0;
                    asm_.emit(mvCond, NVFX_FP_OP_OPCODE_MOV);
                    appendBranchTail(conditionalBr);
                    if (conditionalBr.kind == BranchKind::Varying)
                    {
                        attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(conditionalBr.inputSrc);
                        if (conditionalBr.inputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            conditionalBr.inputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = conditionalBr.inputSrc - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoordsInputMask |= uint16_t{1} << n;
                            attrs.texCoords2D &= ~(uint16_t{1} << n);
                        }
                    }

                    // Track varying-input mask bit for the LHS varying
                    // of the compare itself.
                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(inIt->second);

                    emittedSomething = true;
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

                // RCP / RSQ — scalar-function-unit unary ops on a
                // scalar varying lane.  Two-insn shape:
                //     MOVH H0.<lane>, f[input];
                //     <OP>R R0, H0;      (last = END-bit carrier)
                // Both instructions default-precision (MOVH is FP16
                // via is_fp16 + precision bit; OP is FP32 since it
                // broadcasts to R0 at full precision).
                auto suIt = valueToScalarUnary.find(srcId);
                if (suIt != valueToScalarUnary.end())
                {
                    const auto& bind = suIt->second;
                    auto inIt = valueToInputSrc.find(bind.inputId);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scalar-unary input failed to resolve");
                        return out;
                    }

                    // MOVH H0.<lane>, f[input]
                    struct nvfx_reg hDst = nvfx_reg(NVFXSR_TEMP, 0);
                    hDst.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t laneMask =
                        (bind.lane == 1) ? NVFX_FP_MASK_Y :
                        (bind.lane == 2) ? NVFX_FP_MASK_Z :
                        (bind.lane == 3) ? NVFX_FP_MASK_W :
                                           NVFX_FP_MASK_X;
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1,
                        hDst, laneMask, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    // <RCP|RSQ>R R0, H0
                    const uint8_t scalarOp =
                        (bind.kind == FpScalarUnaryBinding::Kind::Rsq)
                            ? NVFX_FP_OP_OPCODE_RSQ
                            : NVFX_FP_OP_OPCODE_RCP;
                    struct nvfx_src opS0 = nvfx_src(hDst);  // H0 (fp16 alias)
                    struct nvfx_src opS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src opS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn opI = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL,
                        opS0, opS1, opS2);
                    opI.precision = dstPrecision;
                    asm_.emit(opI, scalarOp);

                    emittedSomething = true;
                    break;
                }

                // `length(vec3)` = sqrt(dot(v,v)) — 4-insn shape:
                //     MOVH H0.<mask>, f[input];
                //     DP3R R0.x, H0, H0;
                //     DIVSQR R0.x, |R0|, R0;       ; sce-cgc's sqrt trick
                //     MOVR R0, R0.x;               ; broadcast (END-carrier)
                auto lenIt = valueToLength.find(srcId);
                if (lenIt != valueToLength.end())
                {
                    const auto& bind = lenIt->second;
                    auto inIt = valueToInputSrc.find(bind.inputId);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: length() input failed to resolve");
                        return out;
                    }

                    // MOVH H0.<xyz|xyzw>, f[input]
                    struct nvfx_reg hDst = nvfx_reg(NVFXSR_TEMP, 0);
                    hDst.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t loadMask =
                        (bind.lanes >= 4) ? NVFX_FP_MASK_ALL :
                        (bind.lanes == 3) ? uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z) :
                        (bind.lanes == 2) ? uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y) :
                                             NVFX_FP_MASK_X;
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1,
                        hDst, loadMask, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    // DP3R R0.x, H0, H0
                    const struct nvfx_reg r0 = nvfx_reg(NVFXSR_TEMP, 0);
                    struct nvfx_src dpS0 = nvfx_src(hDst);
                    struct nvfx_src dpS1 = nvfx_src(hDst);
                    struct nvfx_src dpS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t dpOp =
                        (bind.lanes >= 4) ? NVFX_FP_OP_OPCODE_DP4
                                          : NVFX_FP_OP_OPCODE_DP3;
                    struct nvfx_insn dp = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(r0),
                        NVFX_FP_MASK_X, dpS0, dpS1, dpS2);
                    asm_.emit(dp, dpOp);

                    // DIVSQR R0.x, |R0|, R0  →  src0/sqrt(src1).  Both
                    // reads are R0.x (the DP3 result) — the |.| on
                    // src0 is what makes sce-cgc's sqrt identity hold.
                    struct nvfx_src dsS0 = nvfx_src(const_cast<struct nvfx_reg&>(r0));
                    dsS0.abs = 1;
                    struct nvfx_src dsS1 = nvfx_src(const_cast<struct nvfx_reg&>(r0));
                    struct nvfx_src dsS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn ds = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(r0),
                        NVFX_FP_MASK_X, dsS0, dsS1, dsS2);
                    asm_.emit(ds, NVFX_FP_OP_OPCODE_DIVRSQ_NV40RSX);

                    // MOVR R0, R0.x  (broadcast scalar → vec4)
                    struct nvfx_src movS0 = nvfx_src(const_cast<struct nvfx_reg&>(r0));
                    movS0.swz[0] = movS0.swz[1] = movS0.swz[2] = movS0.swz[3] = 0;  // xxxx
                    struct nvfx_src movS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src movS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mov = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_ALL, movS0, movS1, movS2);
                    mov.precision = dstPrecision;
                    asm_.emit(mov, NVFX_FP_OP_OPCODE_MOV);

                    emittedSomething = true;
                    break;
                }

                // `normalize(vec3)` — 4-or-5-insn shape depending on
                // whether the caller wrapped with `float4(.., 1.0)`:
                //     MOVH H0.xyz, f[input];
                //     DP3R R0.z, H0, H0;              (lenSq in .z)
                //   [ MOVR R0.w, {1.0, 0, 0, 0}.x; ]  (only if wrapW1)
                //     DIVSQR R0.xyz, H0, R0.z;        (last — END-carrier)
                auto nrmIt = valueToNormalize.find(srcId);
                if (nrmIt != valueToNormalize.end())
                {
                    const auto& bind = nrmIt->second;
                    auto inIt = valueToInputSrc.find(bind.inputId);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: normalize() input failed to resolve");
                        return out;
                    }

                    // MOVH H0.xyz, f[input]
                    struct nvfx_reg hDst = nvfx_reg(NVFXSR_TEMP, 0);
                    hDst.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t loadMask =
                        (bind.lanes >= 4) ? NVFX_FP_MASK_ALL :
                        (bind.lanes == 3) ? uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z) :
                        (bind.lanes == 2) ? uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y) :
                                             NVFX_FP_MASK_X;
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1,
                        hDst, loadMask, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    // DP3R R0.z, H0, H0  (lenSq lands in .z so the
                    // final DIVSQR reads R0.z as src1 — matches sce-cgc)
                    const struct nvfx_reg r0 = nvfx_reg(NVFXSR_TEMP, 0);
                    struct nvfx_src dpS0 = nvfx_src(hDst);
                    struct nvfx_src dpS1 = nvfx_src(hDst);
                    struct nvfx_src dpS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t dpOp =
                        (bind.lanes >= 4) ? NVFX_FP_OP_OPCODE_DP4
                                          : NVFX_FP_OP_OPCODE_DP3;
                    struct nvfx_insn dp = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(r0),
                        NVFX_FP_MASK_Z, dpS0, dpS1, dpS2);
                    asm_.emit(dp, dpOp);

                    // Optional: MOVR R0.w = 1.0 via inline const block.
                    // sce-cgc emits the disasm form `{...}.x` — SRC0 is
                    // swizzled .xxxx so any dest-mask lane reads the
                    // first float of the inline block.
                    if (bind.wrapW1)
                    {
                        const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                        struct nvfx_src kS0 = nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        kS0.swz[0] = kS0.swz[1] = kS0.swz[2] = kS0.swz[3] = 0;  // .xxxx
                        struct nvfx_src kS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src kS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn kMov = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(r0),
                            NVFX_FP_MASK_W, kS0, kS1, kS2);
                        asm_.emit(kMov, NVFX_FP_OP_OPCODE_MOV);

                        const float oneXyzw[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                        asm_.appendConstBlock(oneXyzw);
                    }

                    // DIVSQR R0.xyz, H0, R0.z   (broadcast lenSq via .z swizzle)
                    struct nvfx_src dsS0 = nvfx_src(hDst);
                    struct nvfx_src dsS1 = nvfx_src(const_cast<struct nvfx_reg&>(r0));
                    dsS1.swz[0] = dsS1.swz[1] = dsS1.swz[2] = dsS1.swz[3] = 2;  // zzzz
                    struct nvfx_src dsS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t dsMask =
                        (bind.lanes >= 4) ? NVFX_FP_MASK_ALL :
                        (bind.lanes == 3) ? uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z) :
                        (bind.lanes == 2) ? uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y) :
                                             NVFX_FP_MASK_X;
                    struct nvfx_insn ds = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        dsMask, dsS0, dsS1, dsS2);
                    ds.precision = dstPrecision;
                    asm_.emit(ds, NVFX_FP_OP_OPCODE_DIVRSQ_NV40RSX);

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
                        // MUL / MIN / MAX / DP3 / DP4 share
                        // (src0=INPUT, src1=CONST) canonical placement —
                        // verified against sce-cgc.
                        switch (bind.op)
                        {
                        case FpArithOp::Min:  opcode = NVFX_FP_OP_OPCODE_MIN; break;
                        case FpArithOp::Max:  opcode = NVFX_FP_OP_OPCODE_MAX; break;
                        case FpArithOp::Dot3: opcode = NVFX_FP_OP_OPCODE_DP3; break;
                        case FpArithOp::Dot4: opcode = NVFX_FP_OP_OPCODE_DP4; break;
                        case FpArithOp::Mul:
                        default:              opcode = NVFX_FP_OP_OPCODE_MUL; break;
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
