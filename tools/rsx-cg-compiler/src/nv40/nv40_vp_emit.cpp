/*
 * NV40 vertex-program lowering.
 *
 * Walks an IRFunction and produces an NV40 ucode stream via VpAssembler.
 *
 * Stage 2 scope: identity passthrough (LoadAttribute → StoreOutput with
 * matching semantic).
 *
 * Stage 3a: uniform reads.
 *   - `uniform float4` allocated top-down from c[467]
 *   - `uniform float4x4` allocated bottom-up from c[256] (four rows)
 *   - StoreOutput whose source is a LoadUniform emits MOV o[N], c[K]
 *
 * Stage 3b: matrix × vector → 4 DP4s.
 *   - IROp::MatVecMul against a uniform matrix defers emit until we see
 *     StoreOutput consume it; then emit 4 DP4s writing W, Z, Y, X to the
 *     destination output, pulling successive matrix rows (base+3..base+0).
 *   - Matches sce-cgc's observed layout — component order, rows descending.
 *
 * sce-cgc's allocator + ucode CONST_SRC encoding are documented in
 * docs/REVERSE_ENGINEERING.md.
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

struct nvfx_reg makeReg(int type, int index)
{
    return nvfx_reg(type, index);
}

struct nvfx_src makeSrc(const struct nvfx_reg& r)
{
    return nvfx_src(const_cast<struct nvfx_reg&>(r));
}

#define VP_OP(NAME) ((NVFX_VP_INST_SLOT_VEC << 7) | NVFX_VP_INST_VEC_OP_##NAME)

// sce-cgc emits the 4 DP4s in the order W, Z, Y, X, pulling matrix rows
// base+3, base+2, base+1, base+0 respectively.
constexpr int kDp4Writemask[4] = {
    NVFX_VP_MASK_W, NVFX_VP_MASK_Z, NVFX_VP_MASK_Y, NVFX_VP_MASK_X };
constexpr int kDp4RowOffset[4]   = { 3, 2, 1, 0 };

struct IoBinding
{
    int         nv40Index = -1;
    std::string semanticName;
    int         semanticIndex = 0;
};

// Placement chosen for each uniform global.  See REVERSE_ENGINEERING.md:
// sce-cgc grows matrices up from c[256] and scalars/vectors down from c[467].
struct ConstBinding
{
    int baseReg  = -1;  // absolute const-bank index (256..467)
    int rowCount = 1;   // 1 for vec4, N for matrix rows
};

class ConstAllocator
{
public:
    // Sce-cgc allocates in declaration order.  Both top-level `uniform`
    // globals and `uniform` function parameters participate — process the
    // entry-point params first (that's their declaration order), then any
    // module-level globals the front-end may have split off.
    void allocate(const IRFunction& entry, const IRModule& module)
    {
        for (const auto& param : entry.parameters)
        {
            if (param.storage == StorageQualifier::Uniform)
                assign(param.name, param.type);
        }
        for (const auto& global : module.globals)
        {
            if (global.storage != StorageQualifier::Uniform) continue;
            if (bindings_.count(global.name)) continue;
            assign(global.name, global.type);
        }
    }

    const ConstBinding* lookup(const std::string& name) const
    {
        auto it = bindings_.find(name);
        return (it == bindings_.end()) ? nullptr : &it->second;
    }

private:
    void assign(const std::string& name, const IRTypeInfo& type)
    {
        ConstBinding b;
        if (type.isMatrix())
        {
            b.rowCount = type.matrixRows;
            b.baseReg  = nextMatrixReg_;
            nextMatrixReg_ += b.rowCount;
        }
        else
        {
            b.rowCount = 1;
            b.baseReg  = nextVectorReg_;
            nextVectorReg_ -= 1;
        }
        bindings_[name] = b;
    }

    int nextMatrixReg_ = 256;
    int nextVectorReg_ = 467;
    std::unordered_map<std::string, ConstBinding> bindings_;
};

// Source descriptor handed to the per-output MOV emit routine.  Either a
// vertex-attribute input or a const-bank entry.
struct ValueSource
{
    enum class Kind { Input, Const };
    Kind kind    = Kind::Input;
    int  regIdx  = 0;  // input bank index (for Input) or absolute const index (for Const)
};

// Records a deferred matrix×vector multiply — we emit the 4 DP4s when the
// result value is consumed by a StoreOutput (matches sce-cgc which writes
// straight to the output register without a temp).
struct MatVecMulBinding
{
    int         matrixBaseReg = -1;  // absolute const index of matrix row 0
    int         vectorInputIdx = -1; // NV40 input-attribute index of the vec4 operand
};

}  // namespace

UcodeOutput lowerVertexProgram(const IRModule& module, const IRFunction& entry)
{
    UcodeOutput out;
    VpAssembler asm_;

    ConstAllocator consts;
    consts.allocate(entry, module);

    // IR values produced by LoadAttribute, LoadUniform, *or* raw function
    // parameters (the front-end leaves direct-passthrough params as values
    // rather than emitting explicit loads).  Mapped to the NV40 source
    // bank + index they resolve to.
    std::unordered_map<IRValueID, ValueSource> valueToSource;

    // Matrix-typed uniforms keep their const-bank base reg here (no valueToSource
    // entry — matrices don't translate to a single source operand).
    std::unordered_map<IRValueID, int> valueToMatrixBase;

    // MatVecMul results — emit the 4 DP4s once the consumer is known.
    std::unordered_map<IRValueID, MatVecMulBinding> valueToMatVecMul;

    // Deferred store-outputs: sce-cgc emits single-instruction StoreOutputs
    // before multi-instruction ones (so the LAST flag lands on the tail of
    // the longest chain — typically the HPOS matvecmul).  Queue the complex
    // ones here and flush at the end of the IR walk.
    struct DeferredStore
    {
        IRValueID   srcId;
        std::string semanticName;
        int         semanticIndex;
    };
    std::vector<DeferredStore> deferredStores;

    // Pre-populate from entry-point parameters.  In/InOut params with a
    // semantic are vertex-attribute inputs; Uniform params are const-bank
    // entries.  Out params are sinks — StoreOutput's dst is their semantic,
    // not their value.
    for (const auto& param : entry.parameters)
    {
        if (param.storage == StorageQualifier::Uniform)
        {
            const ConstBinding* b = consts.lookup(param.name);
            if (!b) continue;

            if (param.type.isMatrix())
            {
                valueToMatrixBase[param.valueId] = b->baseReg;
            }
            else
            {
                valueToSource[param.valueId] =
                    { ValueSource::Kind::Const, b->baseReg };
            }
        }
        else if (param.storage == StorageQualifier::In ||
                 param.storage == StorageQualifier::InOut ||
                 param.storage == StorageQualifier::None)
        {
            if (!param.semanticName.empty())
            {
                const std::string semUpper = toUpper(param.semanticName);
                const int nv40Idx =
                    vertexInputIndex(semUpper, param.semanticIndex);
                if (nv40Idx >= 0)
                {
                    valueToSource[param.valueId] =
                        { ValueSource::Kind::Input, nv40Idx };
                }
            }
        }
    }

    bool emittedSomething = false;

    auto emitStoreOutput =
        [&](IRValueID srcId,
            const std::string& semanticName,
            int semanticIndex) -> bool
    {
        const std::string semUpper = toUpper(semanticName);
        const int outIndex = vertexOutputIndex(semUpper, semanticIndex);
        if (outIndex < 0)
        {
            out.diagnostics.push_back(
                "nv40-vp: unsupported output semantic '" + semanticName + "'");
            return false;
        }

        const struct nvfx_reg dstReg = makeReg(NVFXSR_OUTPUT, outIndex);
        const struct nvfx_reg none   = makeReg(NVFXSR_NONE, 0);

        auto mvIt = valueToMatVecMul.find(srcId);
        if (mvIt != valueToMatVecMul.end())
        {
            const struct nvfx_reg vecReg =
                makeReg(NVFXSR_INPUT, mvIt->second.vectorInputIdx);
            for (int i = 0; i < 4; ++i)
            {
                const int rowReg =
                    mvIt->second.matrixBaseReg + kDp4RowOffset[i];
                const struct nvfx_reg rowConst = makeReg(NVFXSR_CONST, rowReg);

                struct nvfx_src src0 = makeSrc(vecReg);
                struct nvfx_src src1 = makeSrc(rowConst);
                struct nvfx_src src2 = makeSrc(none);

                struct nvfx_insn in = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dstReg),
                    kDp4Writemask[i],
                    src0, src1, src2);
                asm_.emit(in, VP_OP(DP4));
            }
            emittedSomething = true;
            return true;
        }

        auto it = valueToSource.find(srcId);
        if (it == valueToSource.end())
        {
            out.diagnostics.push_back(
                "nv40-vp: StoreOutput source is not a direct Load or matvecmul");
            return false;
        }

        const struct nvfx_reg srcReg =
            (it->second.kind == ValueSource::Kind::Input)
                ? makeReg(NVFXSR_INPUT, it->second.regIdx)
                : makeReg(NVFXSR_CONST, it->second.regIdx);

        struct nvfx_src src0 = makeSrc(srcReg);
        struct nvfx_src src1 = makeSrc(none);
        struct nvfx_src src2 = makeSrc(none);

        struct nvfx_insn in = nvfx_insn(
            0, 0, -1, -1,
            const_cast<struct nvfx_reg&>(dstReg),
            NVFX_VP_MASK_ALL,
            src0, src1, src2);
        asm_.emit(in, VP_OP(MOV));
        emittedSomething = true;
        return true;
    };

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
                const std::string semUpper = toUpper(inst.semanticName);
                const int nv40Idx = vertexInputIndex(semUpper, inst.semanticIndex);
                if (nv40Idx < 0)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: unsupported input semantic '" + inst.semanticName + "'");
                    return out;
                }
                valueToSource[inst.result] =
                    { ValueSource::Kind::Input, nv40Idx };
                break;
            }

            case IROp::LoadUniform:
            {
                const ConstBinding* b = consts.lookup(inst.targetName);
                if (!b)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: LoadUniform for unknown global '" + inst.targetName + "'");
                    return out;
                }
                if (inst.resultType.isMatrix())
                    valueToMatrixBase[inst.result] = b->baseReg;
                else
                    valueToSource[inst.result] =
                        { ValueSource::Kind::Const, b->baseReg };
                break;
            }

            case IROp::MatVecMul:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-vp: MatVecMul with <2 operands");
                    return out;
                }

                auto matIt = valueToMatrixBase.find(inst.operands[0]);
                if (matIt == valueToMatrixBase.end())
                {
                    out.diagnostics.push_back(
                        "nv40-vp: MatVecMul matrix operand is not a uniform matrix");
                    return out;
                }

                auto vecIt = valueToSource.find(inst.operands[1]);
                if (vecIt == valueToSource.end() ||
                    vecIt->second.kind != ValueSource::Kind::Input)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: MatVecMul vector operand must be a vertex input "
                        "(other patterns land later)");
                    return out;
                }

                valueToMatVecMul[inst.result] =
                    { matIt->second, vecIt->second.regIdx };
                break;
            }

            case IROp::StoreOutput:
            {
                if (inst.operands.empty())
                {
                    out.diagnostics.push_back("nv40-vp: StoreOutput with no operand");
                    return out;
                }

                const IRValueID srcId = inst.operands[0];

                // Multi-instruction expansions (matvecmul) are deferred until
                // after all single-instruction StoreOutputs have been emitted.
                if (valueToMatVecMul.count(srcId))
                {
                    deferredStores.push_back(
                        { srcId, inst.semanticName, inst.semanticIndex });
                    break;
                }

                if (!emitStoreOutput(srcId, inst.semanticName, inst.semanticIndex))
                    return out;
                break;
            }

            case IROp::Return:
                break;

            default:
                out.diagnostics.push_back(
                    "nv40-vp: unsupported IR op (stage 3a handles MOV from attr/uniform only)");
                return out;
            }
        }
    }

    for (const auto& def : deferredStores)
    {
        if (!emitStoreOutput(def.srcId, def.semanticName, def.semanticIndex))
            return out;
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
