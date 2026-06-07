/*
 * Experimental general NV40 lowering pipeline.
 *
 * This is deliberately opt-in (`--general-lowering` / RSXCG_GENERAL).
 * Recognizers remain the default byte-exact path while this pipeline grows.
 *
 * Confirmed pieces implemented here:
 *   - profile-neutral virtual NV40 instruction records;
 *   - IR op dispatch for simple arithmetic, dot, min/max, mov, tex, saturate;
 *   - SSA value-id -> virtual register mapping;
 *   - first-definition physical temp allocation with LIFO reuse at last-use;
 *   - program-order emission into the existing FP/VP assemblers.
 *
 * Stubs left explicit until the HAL drill lands:
 *   - VP VEC/SCA co-issue pairing;
 *   - FP FENC placement;
 *   - VP DAG linearization/interleave beyond IR program order.
 */

#include "nv40_general_lowering.h"

#include "nv40_fp_assembler.h"
#include "nv40_vp_assembler.h"
#include "nv30_vertprog.h"
#include "nv40_vertprog.h"
#include "nvfx_shader.h"

#include "ir.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace nv40::detail
{
namespace
{

enum class GeneralProfile { Fragment, Vertex };

enum class VOp
{
    Mov,
    Add,
    Mul,
    Mad,
    Dp3,
    Dp4,
    Min,
    Max,
    Rcp,
    Rsq,
    Sin,
    Cos,
    Lg2,
    Ex2,
    DivSqrt,
    Sge,
    Tex
};

enum class VSrcKind
{
    None,
    Temp,
    Input,
    Uniform,
    Literal,
};

struct VSrc
{
    VSrcKind kind = VSrcKind::None;
    int      index = 0;      // virtual temp id, input source, uniform param, or const index
    int      phys = -1;      // filled for Temp after allocation
    bool     fp16 = false;   // FP-only: temp source is an H register
    bool     embeddedUniform = false; // FP uniforms use inline blocks; VP uniforms are const regs
    std::array<float, 4> literal = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<uint8_t, 4> swizzle = {0, 1, 2, 3};
    bool     neg = false;
    bool     abs = false;
};

struct VDst
{
    bool none = false;
    bool output = false;
    int  index = 0;          // virtual temp id or output register index
    int  phys = -1;          // filled for temp destinations after allocation
    int  preferredPhys = -1; // FP-only: optional R/H index pin for precision shaping
    bool fp16 = false;       // FP-only: destination is an H register
    int  writemask = 0xf;
};

struct VInstr
{
    VOp op = VOp::Mov;
    VDst dst;
    std::array<VSrc, 3> srcs;
    int  sourceIndex = -1; // tie-break metadata for the VP list scheduler
    bool sat = false;
    bool ccUpdate = false;
    int  predicate = 0;
    int  texUnit = 0;
    bool disablePc = false;
    int  fpScale = 0;
    int  fpPrecisionOverride = -1;
    bool preservePartialOutputMask = false;
    bool stubCoIssuePartner = false;
    bool stubFenceBefore = false;   // FENCTR
    bool stubFenceBrBefore = false; // FENCBR
};

struct VirtualProgram
{
    std::vector<VInstr> instrs;
    std::unordered_map<IRValueID, int> valueToVReg;
    std::unordered_map<IRValueID, VSrc> valueToSource;
    std::unordered_map<int, int> vregToPhys;
    std::unordered_map<int, bool> vregToFp16;
    int nextVpLiteralConst = 467;
    std::vector<std::string> diagnostics;
};

static std::string toUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

static int componentMask(const IRTypeInfo& type)
{
    const int n = std::max(1, std::min(4, type.componentCount()));
    return (1 << n) - 1;
}

static int vpMaskFromComponentMask(int mask)
{
    int out = 0;
    if (mask & 1) out |= NVFX_VP_MASK_X;
    if (mask & 2) out |= NVFX_VP_MASK_Y;
    if (mask & 4) out |= NVFX_VP_MASK_Z;
    if (mask & 8) out |= NVFX_VP_MASK_W;
    return out;
}

static int vertexInputIndex(const std::string& semanticUpper, int semanticIndex)
{
    if (semanticUpper == "POSITION") return NVFX_VP_INST_IN_POS;
    if (semanticUpper == "NORMAL")   return NVFX_VP_INST_IN_NORMAL;
    if (semanticUpper == "COLOR" || semanticUpper == "COL")
        return semanticIndex == 1 ? NVFX_VP_INST_IN_COL1
                                  : NVFX_VP_INST_IN_COL0;
    if (semanticUpper == "TEXCOORD" || semanticUpper == "TEX")
        return NVFX_VP_INST_IN_TC(semanticIndex);
    if (semanticUpper == "FOG" || semanticUpper == "FOGC")
        return NVFX_VP_INST_IN_FOGC;
    return -1;
}

static int vertexOutputIndex(const std::string& semanticUpper, int semanticIndex)
{
    if (semanticUpper == "POSITION" || semanticUpper == "HPOS")
        return NV40_VP_INST_DEST_POS;
    if (semanticUpper == "COLOR" || semanticUpper == "COL")
        return semanticIndex == 1 ? NV40_VP_INST_DEST_COL1
                                  : NV40_VP_INST_DEST_COL0;
    if (semanticUpper == "TEXCOORD" || semanticUpper == "TEX")
        return NV40_VP_INST_DEST_TC(semanticIndex);
    if (semanticUpper == "FOG" || semanticUpper == "FOGC")
        return NV40_VP_INST_DEST_FOGC;
    return -1;
}

static int vertexOutputPriority(int outIndex)
{
    if (outIndex == NV40_VP_INST_DEST_COL0)
        return 3;
    if (outIndex == NV40_VP_INST_DEST_TC(1))
        return 2;
    if (outIndex == NV40_VP_INST_DEST_TC(0))
        return 1;
    if (outIndex == NV40_VP_INST_DEST_POS)
        return 0;
    return outIndex;
}

static int fragmentInputSrc(const std::string& semanticUpper, int semanticIndex)
{
    if (semanticUpper == "POSITION" || semanticUpper == "WPOS")
        return NVFX_FP_OP_INPUT_SRC_POSITION;
    if (semanticUpper == "COLOR" || semanticUpper == "COL")
        return semanticIndex == 1 ? NVFX_FP_OP_INPUT_SRC_COL1
                                  : NVFX_FP_OP_INPUT_SRC_COL0;
    if (semanticUpper == "TEXCOORD" || semanticUpper == "TEX")
        return NVFX_FP_OP_INPUT_SRC_TC(semanticIndex);
    if (semanticUpper == "FOG" || semanticUpper == "FOGC")
        return NVFX_FP_OP_INPUT_SRC_FOGC;
    return -1;
}

static uint32_t fpAttrMaskBitForInputSrc(int inputSrc)
{
    if (inputSrc == NVFX_FP_OP_INPUT_SRC_COL0)
        return (1u << 0) | (1u << 2);
    if (inputSrc == NVFX_FP_OP_INPUT_SRC_COL1)
        return (1u << 1) | (1u << 3);
    if (inputSrc == NVFX_FP_OP_INPUT_SRC_FOGC)
        return 1u << 4;
    if (inputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
        inputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
        return 1u << (14 + (inputSrc - NVFX_FP_OP_INPUT_SRC_TC(0)));
    return 0;
}

static int fragmentOutputIndex(const std::string& semanticUpper)
{
    if (semanticUpper == "COLOR" || semanticUpper == "COL") return 0;
    if (semanticUpper == "DEPTH" || semanticUpper == "DEPTH0") return 1;
    return -1;
}

static VSrc noneSrc()
{
    return VSrc{};
}

static VSrc tempSrc(int vreg)
{
    VSrc s;
    s.kind = VSrcKind::Temp;
    s.index = vreg;
    return s;
}

static VSrc inputSrc(int index)
{
    VSrc s;
    s.kind = VSrcKind::Input;
    s.index = index;
    return s;
}

static VSrc uniformSrc(int index, bool embedded)
{
    VSrc s;
    s.kind = VSrcKind::Uniform;
    s.index = index;
    s.embeddedUniform = embedded;
    return s;
}

static VSrc literalSrc(const IRConstant& constant)
{
    VSrc s;
    s.kind = VSrcKind::Literal;
    if (std::holds_alternative<float>(constant.value)) {
        s.literal[0] = std::get<float>(constant.value);
    } else if (std::holds_alternative<int32_t>(constant.value)) {
        s.literal[0] = static_cast<float>(std::get<int32_t>(constant.value));
    } else if (std::holds_alternative<uint32_t>(constant.value)) {
        s.literal[0] = static_cast<float>(std::get<uint32_t>(constant.value));
    } else if (std::holds_alternative<bool>(constant.value)) {
        s.literal[0] = std::get<bool>(constant.value) ? 1.0f : 0.0f;
    } else if (std::holds_alternative<std::vector<float>>(constant.value)) {
        const auto& values = std::get<std::vector<float>>(constant.value);
        for (size_t i = 0; i < std::min<size_t>(4, values.size()); ++i)
            s.literal[i] = values[i];
    }
    return s;
}

static void assignSwizzle(VSrc& src, int encoded, int count)
{
    if (encoded == 0 && count <= 1)
        return;
    for (int i = 0; i < 4; ++i) {
        const int shift = (i < count ? i : count - 1) * 2;
        src.swizzle[i] = static_cast<uint8_t>((encoded >> shift) & 3);
    }
}

class GeneralBuilder
{
public:
    GeneralBuilder(GeneralProfile profile, const IRFunction& entry,
                   const IRModule& module)
        : profile_(profile), entry_(entry), module_(module)
    {
        countUses();
        seedParameters();
    }

    VirtualProgram run()
    {
        for (const auto& block : entry_.blocks) {
            if (!block) continue;
            for (const auto& instPtr : block->instructions) {
                if (!instPtr) continue;
                lowerInstruction(*instPtr);
            }
        }
        legalizeInputOperands();
        renumberSourceIndices();
        applyOrderingPass();
        allocatePhysicalTemps();
        return program_;
    }

private:
    GeneralProfile profile_;
    const IRFunction& entry_;
    const IRModule& module_;
    VirtualProgram program_;
    int nextVReg_ = 0;
    std::unordered_map<IRValueID, unsigned> useCount_;
    std::unordered_map<IRValueID, int> matrixUniformBase_;
    std::unordered_map<IRValueID, VSrc> conditionToSource_;
    static int componentRankFromMask(int mask)
    {
        switch (mask) {
        case 0x1: return 0;
        case 0x2: return 1;
        case 0x4: return 2;
        case 0x8: return 3;
        default: return 0;
        }
    }

    void renumberSourceIndices()
    {
        if (profile_ != GeneralProfile::Vertex || program_.instrs.empty())
            return;

        struct RankedInstr
        {
            size_t index = 0;
            int category = 0;
            int primary = 0;
            int secondary = 0;
        };

        std::vector<RankedInstr> ranked;
        ranked.reserve(program_.instrs.size());

        size_t i = 0;
        int matvecGroup = 0;
        while (i < program_.instrs.size()) {
            const VInstr& vi = program_.instrs[i];
            const bool isMatVecGroup =
                i + 3 < program_.instrs.size() &&
                vi.op == VOp::Dp4 &&
                vi.dst.writemask == 0x8 &&
                program_.instrs[i + 1].op == VOp::Dp4 &&
                program_.instrs[i + 1].dst.index == vi.dst.index &&
                program_.instrs[i + 1].dst.writemask == 0x4 &&
                program_.instrs[i + 2].op == VOp::Dp4 &&
                program_.instrs[i + 2].dst.index == vi.dst.index &&
                program_.instrs[i + 2].dst.writemask == 0x2 &&
                program_.instrs[i + 3].op == VOp::Dp4 &&
                program_.instrs[i + 3].dst.index == vi.dst.index &&
                program_.instrs[i + 3].dst.writemask == 0x1;
            if (isMatVecGroup) {
                for (int row = 0; row < 4; ++row) {
                    RankedInstr item;
                    item.index = i + static_cast<size_t>(row);
                    item.category = 0;
                    item.primary = -matvecGroup;
                    item.secondary = componentRankFromMask(
                        program_.instrs[item.index].dst.writemask);
                    ranked.push_back(item);
                }
                ++matvecGroup;
                i += 4;
                continue;
            }

            RankedInstr item;
            item.index = i;
            item.category = (vi.op == VOp::Mov && vi.dst.output) ? 1 : 2;
            item.primary = (item.category == 1)
                ? vertexOutputPriority(vi.dst.index)
                : static_cast<int>(i);
            ranked.push_back(item);
            ++i;
        }

        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const RankedInstr& a, const RankedInstr& b) {
                             if (a.category != b.category)
                                 return a.category < b.category;
                             if (a.primary != b.primary)
                                 return a.primary < b.primary;
                             if (a.secondary != b.secondary)
                                 return a.secondary < b.secondary;
                             return a.index < b.index;
                         });

        for (size_t seq = 0; seq < ranked.size(); ++seq)
            program_.instrs[ranked[seq].index].sourceIndex = static_cast<int>(seq);
    }

    void applyOrderingPass()
    {
        if ((profile_ != GeneralProfile::Vertex &&
             profile_ != GeneralProfile::Fragment) ||
            program_.instrs.size() < 2)
            return;

        const bool dumpOrder = std::getenv("RSX_DUMP_ORDER") != nullptr;
        if (dumpOrder) {
            for (size_t i = 0; i < program_.instrs.size(); ++i) {
                const VInstr& vi = program_.instrs[i];
                std::fprintf(stderr, "pre[%zu] op=%d dstOut=%d dstIdx=%d mask=%x src0=%d/%d src1=%d/%d src2=%d/%d\n",
                             i,
                             static_cast<int>(vi.op),
                             vi.dst.output ? 1 : 0,
                             vi.dst.index,
                             vi.dst.writemask,
                             static_cast<int>(vi.srcs[0].kind), vi.srcs[0].index,
                             static_cast<int>(vi.srcs[1].kind), vi.srcs[1].index,
                             static_cast<int>(vi.srcs[2].kind), vi.srcs[2].index);
            }
        }

        struct Node
        {
            size_t index = 0;
            int readyTime = 0;
            int bankA = 0;
            int bankB = 0;
            bool resourceOverflow = false;
            bool schedulableNow = true;
            int slack = 0;
            int sourceIndex = 0;
        };

        const auto better = [](const Node& a, const Node& b) {
            if (a.bankA != b.bankA) return a.bankA < b.bankA;
            if (a.bankB != b.bankB) return a.bankB < b.bankB;
            if (a.resourceOverflow != b.resourceOverflow)
                return !a.resourceOverflow && b.resourceOverflow;
            if (a.schedulableNow != b.schedulableNow)
                return a.schedulableNow && !b.schedulableNow;
            if (a.slack != b.slack) return a.slack < b.slack;
            return a.sourceIndex > b.sourceIndex;
        };

        // FP-specific latency deferred until an FP dependency-chain fixture
        // constrains it.
        const auto latencyFor = [&](const VInstr& vi) {
            switch (vi.op) {
            case VOp::Tex:
                return 4;
            case VOp::Dp4:
                return 4;
            case VOp::Lg2:
            case VOp::Ex2:
            case VOp::Rcp:
            case VOp::Sin:
            case VOp::Cos:
            case VOp::DivSqrt:
                return 4;
            case VOp::Rsq:
                return 3;
            case VOp::Dp3:
                return 2;
            case VOp::Mul:
            case VOp::Mad:
            case VOp::Mov:
            case VOp::Min:
            case VOp::Max:
            case VOp::Sge:
                return 1;
            case VOp::Add:
                return 4;
            }
            return 1;
        };

        const auto sourceBankPressure = [&](const VInstr& vi) {
            if (profile_ == GeneralProfile::Fragment)
                return std::pair<int, int>{0, 0};
            const int bankA = 0;
            const int bankB = vi.dst.output ? 1 : 0;
            return std::pair<int, int>{bankA, bankB};
        };

        const size_t n = program_.instrs.size();
        std::vector<int> indegree(n, 0);
        std::vector<std::vector<size_t>> consumers(n);
        std::unordered_map<int, size_t> tempProducer;
        tempProducer.reserve(n * 2);

        for (size_t i = 0; i < n; ++i) {
            const VInstr& vi = program_.instrs[i];
            for (const VSrc& src : vi.srcs) {
                if (src.kind != VSrcKind::Temp)
                    continue;
                const auto prodIt = tempProducer.find(src.index);
                if (prodIt == tempProducer.end())
                    continue;
                const size_t producer = prodIt->second;
                auto& deps = consumers[producer];
                if (std::find(deps.begin(), deps.end(), i) == deps.end()) {
                    deps.push_back(i);
                    ++indegree[i];
                }
            }
            if (!vi.dst.none && !vi.dst.output)
                tempProducer[vi.dst.index] = i;
        }

        std::vector<int> readyTime(n, 0);
        std::vector<bool> scheduled(n, false);
        std::vector<Node> ready;
        ready.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (indegree[i] == 0) {
                Node node;
                node.index = i;
                node.readyTime = 0;
                node.sourceIndex = profile_ == GeneralProfile::Fragment
                    ? static_cast<int>(i)
                    : program_.instrs[i].sourceIndex;
                const auto [bankA, bankB] = sourceBankPressure(program_.instrs[i]);
                node.bankA = bankA;
                node.bankB = bankB;
                ready.push_back(node);
            }
        }

        std::vector<VInstr> ordered;
        ordered.reserve(n);
        int curCycle = 0;
        while (ordered.size() < n) {
            auto bestIt = ready.end();
            Node bestNode;
            bool found = false;
            int nextCycle = std::numeric_limits<int>::max();

            for (auto it = ready.begin(); it != ready.end(); ++it) {
                if (scheduled[it->index])
                    continue;
                if (it->readyTime > curCycle) {
                    nextCycle = std::min(nextCycle, it->readyTime);
                    continue;
                }

                Node cand = *it;
                cand.schedulableNow = true;
                cand.slack = std::max(0, curCycle - cand.readyTime);

                if (!found || better(cand, bestNode)) {
                    bestNode = cand;
                    bestIt = it;
                    found = true;
                }
            }

            if (!found) {
                if (nextCycle == std::numeric_limits<int>::max()) {
                    ++curCycle;
                    continue;
                }
                curCycle = nextCycle;
                continue;
            }

            const size_t idx = bestNode.index;
            scheduled[idx] = true;
            ordered.push_back(program_.instrs[idx]);
            ready.erase(bestIt);

            const int completeCycle = curCycle + latencyFor(program_.instrs[idx]);
            for (size_t consumer : consumers[idx]) {
                readyTime[consumer] = std::max(readyTime[consumer], completeCycle);
                if (--indegree[consumer] == 0) {
                    Node node;
                    node.index = consumer;
                    node.readyTime = readyTime[consumer];
                    node.sourceIndex = profile_ == GeneralProfile::Fragment
                        ? static_cast<int>(consumer)
                        : program_.instrs[consumer].sourceIndex;
                    const auto [bankA, bankB] = sourceBankPressure(program_.instrs[consumer]);
                    node.bankA = bankA;
                    node.bankB = bankB;
                    ready.push_back(node);
                }
            }

            ++curCycle;
        }

        program_.instrs = std::move(ordered);
    }

    void countUses()
    {
        for (const auto& block : entry_.blocks) {
            if (!block) continue;
            for (const auto& instPtr : block->instructions) {
                if (!instPtr) continue;
                for (IRValueID id : instPtr->operands)
                    ++useCount_[id];
            }
        }
    }

    void seedParameters()
    {
        int nextVpMatrixConst = 256;
        int nextVpUniformConst = 467;
        std::unordered_set<std::string> seenUniformNames;
        const bool dumpOrder = std::getenv("RSX_DUMP_ORDER") != nullptr;
        struct PendingMatrix {
            IRValueID valueId;
            std::string name;
            int rows = 0;
        };
        std::vector<PendingMatrix> pendingMatrices;
        for (size_t pi = 0; pi < entry_.parameters.size(); ++pi) {
            const auto& p = entry_.parameters[pi];
            const std::string sem = toUpper(p.semanticName);
            if (p.storage == StorageQualifier::Uniform &&
                !seenUniformNames.insert(p.name).second) {
                continue;
            }
            if (profile_ == GeneralProfile::Vertex &&
                (p.storage == StorageQualifier::In ||
                 p.storage == StorageQualifier::None)) {
                const int idx = vertexInputIndex(sem, p.semanticIndex);
                if (idx >= 0)
                    program_.valueToSource[p.valueId] = inputSrc(idx);
            } else if (profile_ == GeneralProfile::Fragment &&
                       (p.storage == StorageQualifier::In ||
                        p.storage == StorageQualifier::None)) {
                const int idx = fragmentInputSrc(sem, p.semanticIndex);
                if (idx >= 0)
                    program_.valueToSource[p.valueId] = inputSrc(idx);
            } else if (profile_ == GeneralProfile::Vertex &&
                       p.storage == StorageQualifier::Uniform &&
                       p.type.isMatrix()) {
                pendingMatrices.push_back(PendingMatrix{p.valueId, p.name, p.type.matrixRows});
            } else if (profile_ == GeneralProfile::Vertex &&
                       p.storage == StorageQualifier::Uniform) {
                program_.valueToSource[p.valueId] =
                    uniformSrc(nextVpUniformConst--, false);
            } else if (profile_ == GeneralProfile::Fragment &&
                       p.storage == StorageQualifier::Uniform &&
                       p.type.baseType != IRType::Sampler2D &&
                       p.type.baseType != IRType::SamplerRect &&
                       p.type.baseType != IRType::SamplerCube) {
                program_.valueToSource[p.valueId] =
                    uniformSrc(static_cast<int>(pi), true);
            }
        }

        for (const auto& g : module_.globals) {
            if (g.storage != StorageQualifier::Uniform)
                continue;
            if (!seenUniformNames.insert(g.name).second)
                continue;
            if (profile_ == GeneralProfile::Vertex && g.type.isMatrix()) {
                pendingMatrices.push_back(PendingMatrix{g.valueId, g.name, g.type.matrixRows});
            } else if (profile_ == GeneralProfile::Vertex) {
                program_.valueToSource[g.valueId] =
                    uniformSrc(nextVpUniformConst--, false);
            }
        }
        for (auto it = pendingMatrices.begin(); it != pendingMatrices.end(); ++it) {
            matrixUniformBase_[it->valueId] = nextVpMatrixConst;
            if (dumpOrder) {
                std::fprintf(stderr, "matrix %s -> c[%d]\n",
                             it->name.c_str(), nextVpMatrixConst);
            }
            nextVpMatrixConst += it->rows;
        }
        program_.nextVpLiteralConst = nextVpUniformConst;
    }

    VSrc resolve(IRValueID id)
    {
        const auto srcIt = program_.valueToSource.find(id);
        if (srcIt != program_.valueToSource.end())
            return srcIt->second;
        const auto regIt = program_.valueToVReg.find(id);
        if (regIt != program_.valueToVReg.end()) {
            VSrc src = tempSrc(regIt->second);
            const auto fp16It = program_.vregToFp16.find(regIt->second);
            src.fp16 = fp16It != program_.vregToFp16.end() && fp16It->second;
            return src;
        }

        const IRValue* value = entry_.getValue(id);
        if (auto* constant = dynamic_cast<const IRConstant*>(value))
            return literalSrc(*constant);

        return noneSrc();
    }

    int define(IRValueID id)
    {
        const auto it = program_.valueToVReg.find(id);
        if (it != program_.valueToVReg.end())
            return it->second;
        const int vreg = nextVReg_++;
        program_.valueToVReg[id] = vreg;
        program_.vregToFp16[vreg] = false;
        return vreg;
    }

    int newVReg()
    {
        const int vreg = nextVReg_++;
        program_.vregToFp16[vreg] = false;
        return vreg;
    }

    void lowerInstruction(const IRInstruction& inst)
    {
        switch (inst.op) {
        case IROp::LoadAttribute:
        case IROp::LoadVarying:
            lowerInputLoad(inst);
            return;
        case IROp::VecShuffle:
            lowerVecShuffle(inst);
            return;
        case IROp::VecExtract:
            lowerVecExtract(inst);
            return;
        case IROp::VecInsert:
            lowerVecInsert(inst);
            return;
        case IROp::VecConstruct:
            lowerVecConstruct(inst);
            return;
        case IROp::Saturate:
            lowerUnary(inst, VOp::Mov, true);
            return;
        case IROp::Add:
            lowerBinary(inst, VOp::Add);
            return;
        case IROp::Sub:
            lowerBinary(inst, VOp::Add, true);
            return;
        case IROp::Mul:
            lowerBinary(inst, VOp::Mul);
            return;
        case IROp::Div:
            lowerDiv(inst);
            return;
        case IROp::Mad:
            lowerTernary(inst, VOp::Mad);
            return;
        case IROp::MatVecMul:
            lowerMatVecMul(inst);
            return;
        case IROp::Sin:
            lowerUnary(inst, VOp::Sin, false);
            return;
        case IROp::Cos:
            lowerUnary(inst, VOp::Cos, false);
            return;
        case IROp::RSqrt:
            lowerUnary(inst, VOp::Rsq, false);
            return;
        case IROp::Length:
            lowerLength(inst);
            return;
        case IROp::Normalize:
            lowerNormalize(inst);
            return;
        case IROp::Pow:
            lowerPow(inst);
            return;
        case IROp::Dot:
            lowerBinary(inst, inst.resultType.componentCount() >= 4
                              ? VOp::Dp4 : VOp::Dp3);
            return;
        case IROp::Min:
            lowerBinary(inst, VOp::Min);
            return;
        case IROp::Max:
            lowerBinary(inst, VOp::Max);
            return;
        case IROp::Clamp:
            lowerClamp(inst);
            return;
        case IROp::Lerp:
            lowerLerp(inst);
            return;
        case IROp::Step:
            lowerStep(inst);
            return;
        case IROp::Distance:
            lowerDistance(inst);
            return;
        case IROp::Reflect:
            lowerReflect(inst);
            return;
        case IROp::CmpLe:
            lowerCmpLE(inst);
            return;
        case IROp::Select:
            lowerSelect(inst);
            return;
        case IROp::TexSample:
            lowerTex(inst);
            return;
        case IROp::StoreOutput:
            lowerStoreOutput(inst);
            return;
        case IROp::Return:
        case IROp::Comment:
        case IROp::Nop:
            return;
        default:
            program_.diagnostics.push_back(
                std::string("nv40-general: unsupported IR op ") +
                irOpToString(inst.op));
            return;
        }
    }

    void lowerInputLoad(const IRInstruction& inst)
    {
        const std::string sem = toUpper(inst.semanticName);
        const int idx = profile_ == GeneralProfile::Vertex
            ? vertexInputIndex(sem, inst.semanticIndex)
            : fragmentInputSrc(sem, inst.semanticIndex);
        if (idx < 0) {
            program_.diagnostics.push_back(
                "nv40-general: unsupported input semantic " +
                inst.semanticName);
            return;
        }
        program_.valueToSource[inst.result] = inputSrc(idx);
    }

    void lowerVecShuffle(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        VSrc src = resolve(inst.operands[0]);
        assignSwizzle(src, inst.swizzleMask, inst.resultType.componentCount());
        program_.valueToSource[inst.result] = src;
    }

    void lowerVecExtract(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        const int lane = std::max(0, std::min(3, inst.componentIndex));
        VSrc src = resolve(inst.operands[0]);
        src.swizzle = {static_cast<uint8_t>(lane),
                       static_cast<uint8_t>(lane),
                       static_cast<uint8_t>(lane),
                       static_cast<uint8_t>(lane)};
        program_.valueToSource[inst.result] = src;
    }

    void lowerVecInsert(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        const auto baseIt = program_.valueToVReg.find(inst.operands[0]);
        const int lane = std::max(0, std::min(3, inst.componentIndex));
        const int laneMask = 1 << lane;

        const auto isOutputParam = [&](IRValueID id) {
            for (const auto& p : entry_.parameters) {
                if (p.valueId == id &&
                    (p.storage == StorageQualifier::Out ||
                     p.storage == StorageQualifier::InOut))
                    return true;
            }
            return false;
        };

        if (baseIt == program_.valueToVReg.end()) {
            if (!isOutputParam(inst.operands[0]))
                return;

            const int resultReg = define(inst.result);
            program_.valueToVReg[inst.result] = resultReg;

            VInstr vi;
            vi.op = VOp::Mov;
            vi.dst.index = resultReg;
            vi.dst.writemask = laneMask;
            vi.srcs[0] = resolve(inst.operands[1]);
            vi.srcs[0].swizzle = {0, 0, 0, 0};
            program_.instrs.push_back(vi);
            return;
        }

        if (!program_.instrs.empty()) {
            VInstr& producer = program_.instrs.back();
            if (!producer.dst.output &&
                producer.dst.index == baseIt->second &&
                useCount_[inst.operands[0]] == 1)
                producer.dst.writemask &= ~laneMask;
        }

        program_.valueToVReg[inst.result] = baseIt->second;
        VInstr vi;
        vi.op = VOp::Mov;
        vi.dst.index = baseIt->second;
        vi.dst.writemask = laneMask;
        vi.srcs[0] = resolve(inst.operands[1]);
        vi.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(vi);
    }

    void lowerVecConstruct(const IRInstruction& inst)
    {
        if (tryFoldPowDotVecConstruct(inst))
            return;

        if (inst.operands.size() != 2 || inst.result == InvalidIRValue)
            return;
        const auto baseIt = program_.valueToVReg.find(inst.operands[0]);
        if (baseIt == program_.valueToVReg.end())
            return;
        if (inst.resultType.componentCount() != 4)
            return;

        program_.valueToVReg[inst.result] = baseIt->second;
        VInstr vi;
        vi.op = VOp::Mov;
        vi.dst.index = baseIt->second;
        vi.dst.writemask = 0x8;
        vi.srcs[0] = resolve(inst.operands[1]);
        vi.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(vi);
    }

    bool tryFoldPowDotVecConstruct(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment ||
            inst.result == InvalidIRValue ||
            inst.operands.size() != 4 ||
            inst.operands[0] != inst.operands[1] ||
            inst.operands[0] != inst.operands[2] ||
            !isLiteralOne(inst.operands[3]))
            return false;

        const auto powRegIt = program_.valueToVReg.find(inst.operands[0]);
        if (powRegIt == program_.valueToVReg.end() ||
            program_.instrs.size() < 5)
            return false;

        const int powReg = powRegIt->second;
        const size_t ex2Idx = program_.instrs.size() - 1;
        const size_t mulIdx = ex2Idx - 1;
        const size_t lg2Idx = ex2Idx - 2;
        const VInstr ex2 = program_.instrs[ex2Idx];
        const VInstr mul = program_.instrs[mulIdx];
        const VInstr lg2 = program_.instrs[lg2Idx];
        if (ex2.op != VOp::Ex2 || ex2.dst.index != powReg ||
            mul.op != VOp::Mul || mul.dst.index != powReg ||
            lg2.op != VOp::Lg2 || lg2.dst.index != powReg ||
            lg2.srcs[0].kind != VSrcKind::Temp)
            return false;

        const int maxReg = lg2.srcs[0].index;
        size_t maxIdx = 0;
        bool foundMax = false;
        for (size_t i = 0; i < lg2Idx; ++i) {
            const VInstr& vi = program_.instrs[i];
            if (vi.op == VOp::Max && !vi.dst.output && vi.dst.index == maxReg) {
                maxIdx = i;
                foundMax = true;
            }
        }
        if (!foundMax || program_.instrs[maxIdx].srcs[0].kind != VSrcKind::Temp)
            return false;

        const VInstr maxInstr = program_.instrs[maxIdx];
        const int dotReg = maxInstr.srcs[0].index;
        size_t dotIdx = 0;
        bool foundDot = false;
        for (size_t i = 0; i < maxIdx; ++i) {
            const VInstr& vi = program_.instrs[i];
            if (vi.op == VOp::Dp3 && !vi.dst.output && vi.dst.index == dotReg) {
                dotIdx = i;
                foundDot = true;
            }
        }
        if (!foundDot)
            return false;

        const VInstr dot = program_.instrs[dotIdx];
        if (dot.srcs[0].kind != VSrcKind::Input ||
            dot.srcs[1].kind != VSrcKind::Input)
            return false;

        std::vector<VInstr> rewritten;
        rewritten.reserve(program_.instrs.size() + 1);
        rewritten.insert(rewritten.end(), program_.instrs.begin(),
                         program_.instrs.begin() + static_cast<std::ptrdiff_t>(dotIdx));

        VInstr loadRhs;
        loadRhs.op = VOp::Mov;
        loadRhs.dst.index = powReg;
        loadRhs.dst.writemask = 0x7;
        loadRhs.srcs[0] = dot.srcs[1];
        rewritten.push_back(loadRhs);

        VInstr alpha;
        alpha.op = VOp::Mov;
        alpha.dst.index = powReg;
        alpha.dst.writemask = 0x8;
        alpha.srcs[0] = resolve(inst.operands[3]);
        alpha.srcs[0].swizzle = {0, 0, 0, 0};
        rewritten.push_back(alpha);

        VInstr foldedDot;
        foldedDot.op = VOp::Dp3;
        foldedDot.dst.index = powReg;
        foldedDot.dst.writemask = 0x1;
        foldedDot.srcs[0] = dot.srcs[0];
        foldedDot.srcs[1] = tempSrc(powReg);
        foldedDot.stubFenceBefore = true;
        rewritten.push_back(foldedDot);

        VInstr foldedMax = maxInstr;
        foldedMax.dst.index = powReg;
        foldedMax.dst.writemask = 0x1;
        foldedMax.srcs[0] = tempSrc(powReg);
        if (foldedMax.srcs[1].kind == VSrcKind::Literal ||
            foldedMax.srcs[1].kind == VSrcKind::Uniform)
            foldedMax.srcs[1].swizzle = {0, 0, 0, 0};
        rewritten.push_back(foldedMax);

        VInstr foldedLg2 = lg2;
        foldedLg2.dst.index = powReg;
        foldedLg2.dst.writemask = 0x1;
        foldedLg2.srcs[0] = tempSrc(powReg);
        rewritten.push_back(foldedLg2);

        VInstr foldedMul = mul;
        foldedMul.dst.index = powReg;
        foldedMul.dst.writemask = 0x1;
        foldedMul.srcs[0] = tempSrc(powReg);
        rewritten.push_back(foldedMul);

        VInstr foldedEx2 = ex2;
        foldedEx2.dst.index = powReg;
        foldedEx2.dst.writemask = 0x7;
        foldedEx2.srcs[0] = tempSrc(powReg);
        foldedEx2.preservePartialOutputMask = true;
        rewritten.push_back(foldedEx2);

        program_.instrs = std::move(rewritten);
        program_.valueToVReg[inst.result] = powReg;
        return true;
    }

    void lowerUnary(const IRInstruction& inst, VOp op, bool sat)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment &&
            (op == VOp::Rcp || op == VOp::Rsq ||
             op == VOp::Sin || op == VOp::Cos)) {
            program_.diagnostics.push_back(
                "nv40-general: VP scalar intrinsic lowering deferred");
            return;
        }
        VInstr vi;
        vi.op = op;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = resolve(inst.operands[0]);
        vi.sat = sat && !isPreclampedFragmentColor(vi.srcs[0]);
        program_.instrs.push_back(vi);
    }

    void lowerDiv(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment ||
            !isLiteralOne(inst.operands[0])) {
            program_.diagnostics.push_back(
                "nv40-general: only reciprocal div lowering is supported");
            return;
        }
        VInstr vi;
        vi.op = VOp::Rcp;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = resolve(inst.operands[1]);
        program_.instrs.push_back(vi);
    }

    void lowerBinary(const IRInstruction& inst, VOp op, bool negateRhs = false)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        if (profile_ == GeneralProfile::Fragment &&
            op == VOp::Max && tryFoldDotMax(inst))
            return;
        if (profile_ == GeneralProfile::Fragment &&
            op == VOp::Add && !negateRhs && tryFuseAddWithMul(inst))
            return;
        VInstr vi;
        vi.op = op;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = resolve(inst.operands[0]);
        vi.srcs[1] = resolve(inst.operands[1]);
        vi.srcs[1].neg = vi.srcs[1].neg != negateRhs;
        program_.instrs.push_back(vi);
    }

    bool tryFoldDotMax(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || !isLiteralZero(inst.operands[1]))
            return false;
        for (const auto& block : entry_.blocks) {
            if (!block) continue;
            for (const auto& instPtr : block->instructions) {
                if (!instPtr || instPtr->op != IROp::Pow)
                    continue;
                if (!instPtr->operands.empty() &&
                    instPtr->operands[0] == inst.result)
                    return false;
            }
        }
        const auto dotRegIt = program_.valueToVReg.find(inst.operands[0]);
        if (dotRegIt == program_.valueToVReg.end() ||
            program_.instrs.empty())
            return false;

        const int dotReg = dotRegIt->second;
        VInstr dot = program_.instrs.back();
        if (dot.op != VOp::Dp3 || dot.dst.output ||
            dot.dst.index != dotReg ||
            dot.srcs[0].kind != VSrcKind::Input ||
            dot.srcs[1].kind != VSrcKind::Input)
            return false;

        program_.instrs.pop_back();

        VInstr loadRhs;
        loadRhs.op = VOp::Mov;
        loadRhs.dst.index = dotReg;
        loadRhs.dst.writemask = 0x7;
        loadRhs.srcs[0] = dot.srcs[1];
        program_.instrs.push_back(loadRhs);

        VInstr foldedDot;
        foldedDot.op = VOp::Dp3;
        foldedDot.dst.index = dotReg;
        foldedDot.dst.writemask = 0x1;
        foldedDot.srcs[0] = dot.srcs[0];
        foldedDot.srcs[1] = tempSrc(dotReg);
        program_.instrs.push_back(foldedDot);

        VInstr maxv;
        maxv.op = VOp::Max;
        maxv.dst.index = define(inst.result);
        maxv.dst.writemask = componentMask(inst.resultType);
        maxv.srcs[0] = tempSrc(dotReg);
        maxv.srcs[0].swizzle = {0, 0, 0, 0};
        maxv.srcs[1] = resolve(inst.operands[1]);
        maxv.srcs[1].swizzle = {0, 0, 0, 0};
        maxv.stubFenceBrBefore = true;
        program_.instrs.push_back(maxv);
        return true;
    }

    bool tryFuseAddWithMul(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || program_.instrs.empty())
            return false;

        for (int mulOperand = 0; mulOperand < 2; ++mulOperand) {
            const IRValueID mulValue = inst.operands[mulOperand];
            const auto regIt = program_.valueToVReg.find(mulValue);
            if (regIt == program_.valueToVReg.end() || useCount_[mulValue] != 1)
                continue;

            VInstr& producer = program_.instrs.back();
            if (producer.op != VOp::Mul ||
                producer.dst.output ||
                producer.dst.index != regIt->second)
                continue;

            producer.op = VOp::Mad;
            producer.srcs[2] = resolve(inst.operands[1 - mulOperand]);
            producer.dst.index = define(inst.result);
            producer.dst.writemask = componentMask(inst.resultType);
            return true;
        }

        return false;
    }

    void lowerTernary(const IRInstruction& inst, VOp op)
    {
        if (inst.operands.size() < 3 || inst.result == InvalidIRValue) return;
        VInstr vi;
        vi.op = op;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = resolve(inst.operands[0]);
        vi.srcs[1] = resolve(inst.operands[1]);
        vi.srcs[2] = resolve(inst.operands[2]);
        program_.instrs.push_back(vi);
    }

    void lowerMatVecMul(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Vertex ||
            inst.operands.size() < 2 ||
            inst.result == InvalidIRValue) {
            program_.diagnostics.push_back(
                "nv40-general: only VP matvecmul lowering is supported");
            return;
        }
        const auto matIt = matrixUniformBase_.find(inst.operands[0]);
        if (matIt == matrixUniformBase_.end()) {
            program_.diagnostics.push_back(
                "nv40-general: matvecmul matrix source is not a uniform matrix");
            return;
        }

        const int result = define(inst.result);
        VSrc vec = resolve(inst.operands[1]);
        static const int masks[4] = {0x8, 0x4, 0x2, 0x1};
        static const int rows[4] = {3, 2, 1, 0};
        for (int i = 0; i < 4; ++i) {
            VInstr dp;
            dp.op = VOp::Dp4;
            dp.dst.index = result;
            dp.dst.writemask = masks[i];
            dp.srcs[0] = vec;
            dp.srcs[1] = uniformSrc(matIt->second + rows[i], false);
            program_.instrs.push_back(dp);
        }
    }

    void lowerLength(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment) {
            program_.diagnostics.push_back(
                "nv40-general: VP length lowering deferred");
            return;
        }

        const int lenSq = newVReg();
        VInstr load;
        load.op = VOp::Mov;
        load.dst.index = lenSq;
        load.dst.writemask = valueComponentMask(inst.operands[0]) & 0x7;
        load.srcs[0] = resolve(inst.operands[0]);
        program_.instrs.push_back(load);

        VInstr dp;
        dp.op = VOp::Dp3;
        dp.dst.index = lenSq;
        dp.dst.writemask = 0x1;
        dp.srcs[0] = tempSrc(lenSq);
        dp.srcs[1] = tempSrc(lenSq);
        program_.instrs.push_back(dp);

        VInstr sqrt;
        sqrt.op = VOp::DivSqrt;
        sqrt.dst.index = lenSq;
        sqrt.dst.writemask = 0x1;
        sqrt.srcs[0] = tempSrc(lenSq);
        sqrt.srcs[0].abs = true;
        sqrt.srcs[1] = tempSrc(lenSq);
        program_.instrs.push_back(sqrt);

        const int result = define(inst.result);
        VInstr broadcast;
        broadcast.op = VOp::Mov;
        broadcast.dst.index = result;
        broadcast.dst.writemask = componentMask(inst.resultType);
        broadcast.srcs[0] = tempSrc(lenSq);
        broadcast.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(broadcast);
    }

    void lowerNormalize(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment) {
            lowerVertexNormalize(inst);
            return;
        }

        const int result = define(inst.result);
        VSrc src = resolve(inst.operands[0]);

        VInstr dp;
        dp.op = VOp::Dp3;
        dp.dst.index = result;
        dp.dst.writemask = 0x8;
        dp.srcs[0] = src;
        dp.srcs[1] = src;
        dp.disablePc = true;
        program_.instrs.push_back(dp);

        VInstr load;
        load.op = VOp::Mov;
        load.dst.index = result;
        load.dst.writemask = 0x7;
        load.srcs[0] = src;
        load.disablePc = true;
        program_.instrs.push_back(load);

        VInstr norm;
        norm.op = VOp::DivSqrt;
        norm.dst.index = result;
        norm.dst.writemask = 0x7;
        norm.srcs[0] = tempSrc(result);
        norm.srcs[1] = tempSrc(result);
        norm.srcs[1].swizzle = {3, 3, 3, 3};
        program_.instrs.push_back(norm);
    }

    void lowerVertexNormalize(const IRInstruction& inst)
    {
        VInstr delayedPositionMov;
        bool hasDelayedPositionMov = false;
        if (!program_.instrs.empty()) {
            const VInstr& prev = program_.instrs.back();
            if (prev.op == VOp::Mov && prev.dst.output &&
                prev.dst.index == NV40_VP_INST_DEST_POS) {
                delayedPositionMov = prev;
                hasDelayedPositionMov = true;
                program_.instrs.pop_back();
            }
        }

        const int result = define(inst.result);
        VSrc src = resolve(inst.operands[0]);
        applyDp3Swizzle(src);

        VInstr dp;
        dp.op = VOp::Dp3;
        dp.dst.index = result;
        dp.dst.writemask = 0x1;
        dp.srcs[0] = src;
        dp.srcs[1] = src;
        program_.instrs.push_back(dp);

        VInstr rsq;
        rsq.op = VOp::Rsq;
        rsq.dst.index = result;
        rsq.dst.writemask = 0x1;
        rsq.srcs[0] = tempSrc(result);
        rsq.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(rsq);

        if (hasDelayedPositionMov)
            program_.instrs.push_back(delayedPositionMov);

        VInstr mul;
        mul.op = VOp::Mul;
        mul.dst.index = result;
        mul.dst.writemask = componentMask(inst.resultType);
        mul.srcs[0] = tempSrc(result);
        mul.srcs[0].swizzle = {0, 0, 0, 0};
        mul.srcs[1] = src;
        program_.instrs.push_back(mul);
    }

    void lowerPow(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        VInstr delayedPositionMov;
        bool hasDelayedPositionMov = false;
        if (profile_ == GeneralProfile::Vertex && !program_.instrs.empty()) {
            const VInstr& prev = program_.instrs.back();
            if (prev.op == VOp::Mov && prev.dst.output &&
                prev.dst.index == NV40_VP_INST_DEST_POS) {
                delayedPositionMov = prev;
                hasDelayedPositionMov = true;
                program_.instrs.pop_back();
            }
        }

        const int temp = define(inst.result);
        VSrc base = resolve(inst.operands[0]);
        if (profile_ == GeneralProfile::Vertex)
            base.swizzle = {0, 0, 0, 0};
        const auto baseRegIt = program_.valueToVReg.find(inst.operands[0]);
        if (baseRegIt != program_.valueToVReg.end() &&
            useCount_[inst.operands[0]] == 1 &&
            !program_.instrs.empty()) {
            VInstr& producer = program_.instrs.back();
            if (!producer.dst.output &&
                producer.op == VOp::Mov &&
                producer.dst.index == baseRegIt->second &&
                producer.dst.writemask == 0x1) {
                base = producer.srcs[0];
                program_.instrs.pop_back();
            }
        }
        VInstr lg2;
        lg2.op = VOp::Lg2;
        lg2.dst.index = temp;
        lg2.dst.writemask = componentMask(inst.resultType);
        lg2.srcs[0] = base;
        program_.instrs.push_back(lg2);

        if (hasDelayedPositionMov)
            program_.instrs.push_back(delayedPositionMov);

        VInstr mul;
        mul.op = VOp::Mul;
        mul.dst.index = temp;
        mul.dst.writemask = componentMask(inst.resultType);
        mul.srcs[0] = tempSrc(temp);
        if (profile_ == GeneralProfile::Vertex)
            mul.srcs[0].swizzle = {0, 0, 0, 0};
        mul.srcs[1] = resolve(inst.operands[1]);
        if (mul.srcs[1].kind == VSrcKind::Literal ||
            mul.srcs[1].kind == VSrcKind::Uniform)
            mul.srcs[1].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(mul);

        VInstr ex2;
        ex2.op = VOp::Ex2;
        ex2.dst.index = temp;
        ex2.dst.writemask = componentMask(inst.resultType);
        ex2.srcs[0] = tempSrc(temp);
        if (profile_ == GeneralProfile::Vertex)
            ex2.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(ex2);
    }

    void lowerClamp(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment ||
            inst.operands.size() < 3 || inst.result == InvalidIRValue) {
            program_.diagnostics.push_back(
                "nv40-general: only FP clamp lowering is supported");
            return;
        }

        const int result = define(inst.result);
        VInstr load;
        load.op = VOp::Mov;
        load.dst.index = result;
        load.dst.preferredPhys = 1;
        load.dst.writemask = componentMask(inst.resultType);
        load.srcs[0] = resolve(inst.operands[0]);
        program_.instrs.push_back(load);

        VInstr minv;
        minv.op = VOp::Min;
        minv.dst.index = result;
        minv.dst.writemask = componentMask(inst.resultType);
        minv.srcs[0] = resolve(inst.operands[2]);
        minv.srcs[1] = tempSrc(result);
        minv.stubFenceBefore = true;
        program_.instrs.push_back(minv);

        VInstr lo;
        lo.op = VOp::Mov;
        lo.dst.index = newVReg();
        lo.dst.preferredPhys = 0;
        lo.dst.writemask = componentMask(inst.resultType);
        lo.srcs[0] = resolve(inst.operands[1]);
        program_.instrs.push_back(lo);

        VInstr maxv;
        maxv.op = VOp::Max;
        maxv.dst.index = result;
        maxv.dst.writemask = componentMask(inst.resultType);
        maxv.srcs[0] = tempSrc(lo.dst.index);
        maxv.srcs[1] = tempSrc(result);
        program_.instrs.push_back(maxv);
    }

    void lowerLerp(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment ||
            inst.operands.size() < 3 || inst.result == InvalidIRValue) {
            program_.diagnostics.push_back(
                "nv40-general: only FP lerp lowering is supported");
            return;
        }

        const int tReg = newVReg();
        VInstr loadT;
        loadT.op = VOp::Mov;
        loadT.dst.index = tReg;
        loadT.dst.writemask = componentMask(inst.resultType);
        loadT.srcs[0] = resolve(inst.operands[2]);
        program_.instrs.push_back(loadT);

        const int result = define(inst.result);
        VInstr loadA;
        loadA.op = VOp::Mov;
        loadA.dst.index = result;
        loadA.dst.writemask = componentMask(inst.resultType);
        loadA.srcs[0] = resolve(inst.operands[0]);
        program_.instrs.push_back(loadA);

        const int deltaReg = newVReg();
        VInstr delta;
        delta.op = VOp::Add;
        delta.dst.index = deltaReg;
        delta.dst.writemask = componentMask(inst.resultType);
        delta.srcs[0] = resolve(inst.operands[1]);
        delta.srcs[1] = tempSrc(result);
        delta.srcs[1].neg = true;
        delta.stubFenceBefore = true;
        program_.instrs.push_back(delta);

        VInstr mad;
        mad.op = VOp::Mad;
        mad.dst.index = result;
        mad.dst.writemask = componentMask(inst.resultType);
        mad.srcs[0] = tempSrc(tReg);
        mad.srcs[1] = tempSrc(deltaReg);
        mad.srcs[2] = tempSrc(result);
        program_.instrs.push_back(mad);
    }

    void lowerStep(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment ||
            inst.operands.size() < 2 || inst.result == InvalidIRValue) {
            program_.diagnostics.push_back(
                "nv40-general: only FP step lowering is supported");
            return;
        }

        const int edgeReg = newVReg();
        VInstr loadEdge;
        loadEdge.op = VOp::Mov;
        loadEdge.dst.index = edgeReg;
        loadEdge.dst.writemask = componentMask(inst.resultType);
        loadEdge.srcs[0] = resolve(inst.operands[0]);
        program_.instrs.push_back(loadEdge);

        const int cmpReg = define(inst.result);
        VInstr cmp;
        cmp.op = VOp::Sge;
        cmp.dst.index = cmpReg;
        cmp.dst.preferredPhys = 0;
        cmp.dst.writemask = componentMask(inst.resultType);
        cmp.dst.fp16 = true;
        cmp.fpPrecisionOverride = FLOAT32;
        cmp.srcs[0] = resolve(inst.operands[1]);
        cmp.srcs[1] = tempSrc(edgeReg);
        program_.instrs.push_back(cmp);

        VInstr mov;
        mov.op = VOp::Mov;
        mov.dst.index = cmpReg;
        mov.dst.writemask = componentMask(inst.resultType);
        mov.srcs[0] = tempSrc(cmpReg);
        mov.srcs[0].fp16 = true;
        mov.fpPrecisionOverride = FIXED12;
        mov.stubFenceBrBefore = true;
        program_.instrs.push_back(mov);
    }

    void lowerDistance(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment ||
            inst.operands.size() < 2 || inst.result == InvalidIRValue) {
            program_.diagnostics.push_back(
                "nv40-general: only FP distance lowering is supported");
            return;
        }

        const int result = define(inst.result);
        VInstr loadB;
        loadB.op = VOp::Mov;
        loadB.dst.index = result;
        loadB.dst.writemask = valueComponentMask(inst.operands[1]) & 0x7;
        loadB.srcs[0] = resolve(inst.operands[1]);
        program_.instrs.push_back(loadB);

        VInstr delta;
        delta.op = VOp::Add;
        delta.dst.index = result;
        delta.dst.writemask = valueComponentMask(inst.operands[0]) & 0x7;
        delta.srcs[0] = resolve(inst.operands[0]);
        delta.srcs[0].neg = true;
        delta.srcs[1] = tempSrc(result);
        delta.stubFenceBefore = true;
        program_.instrs.push_back(delta);

        VInstr dp;
        dp.op = VOp::Dp3;
        dp.dst.index = result;
        dp.dst.writemask = 0x1;
        dp.srcs[0] = tempSrc(result);
        dp.srcs[1] = tempSrc(result);
        program_.instrs.push_back(dp);

        VInstr sqrt;
        sqrt.op = VOp::DivSqrt;
        sqrt.dst.index = result;
        sqrt.dst.writemask = 0x1;
        sqrt.srcs[0] = tempSrc(result);
        sqrt.srcs[0].abs = true;
        sqrt.srcs[1] = tempSrc(result);
        program_.instrs.push_back(sqrt);

        const int broadcast = newVReg();
        VInstr mov;
        mov.op = VOp::Mov;
        mov.dst.index = broadcast;
        mov.dst.writemask = componentMask(inst.resultType);
        mov.srcs[0] = tempSrc(result);
        mov.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(mov);
        program_.valueToVReg[inst.result] = broadcast;
    }

    void lowerReflect(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment ||
            inst.operands.size() < 2 || inst.result == InvalidIRValue) {
            program_.diagnostics.push_back(
                "nv40-general: only FP reflect lowering is supported");
            return;
        }

        const int result = define(inst.result);
        VInstr loadN;
        loadN.op = VOp::Mov;
        loadN.dst.index = result;
        loadN.dst.preferredPhys = 0;
        loadN.dst.writemask = valueComponentMask(inst.operands[1]) & 0x7;
        loadN.srcs[0] = resolve(inst.operands[1]);
        program_.instrs.push_back(loadN);

        const int iReg = newVReg();
        VInstr loadI;
        loadI.op = VOp::Mov;
        loadI.dst.index = iReg;
        loadI.dst.preferredPhys = 1;
        loadI.dst.writemask = valueComponentMask(inst.operands[0]) & 0x7;
        loadI.srcs[0] = resolve(inst.operands[0]);
        program_.instrs.push_back(loadI);

        VInstr dot;
        dot.op = VOp::Dp3;
        dot.dst.index = result;
        dot.dst.writemask = 0x8;
        dot.srcs[0] = tempSrc(result);
        dot.srcs[1] = tempSrc(iReg);
        dot.fpScale = NVFX_FP_OP_DST_SCALE_2X;
        program_.instrs.push_back(dot);

        VInstr mad;
        mad.op = VOp::Mad;
        mad.dst.index = result;
        mad.dst.writemask = 0x7;
        mad.srcs[0] = tempSrc(result);
        mad.srcs[0].neg = true;
        mad.srcs[1] = tempSrc(result);
        mad.srcs[1].swizzle = {3, 3, 3, 3};
        mad.srcs[2] = tempSrc(iReg);
        mad.stubFenceBrBefore = true;
        program_.instrs.push_back(mad);
    }

    void lowerTex(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        VInstr vi;
        vi.op = VOp::Tex;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = resolve(inst.operands[1]);
        vi.texUnit = 0;
        program_.instrs.push_back(vi);
    }

    bool isLiteralZero(IRValueID id) const
    {
        const IRValue* value = entry_.getValue(id);
        auto* constant = dynamic_cast<const IRConstant*>(value);
        if (!constant) return false;
        if (std::holds_alternative<float>(constant->value))
            return std::get<float>(constant->value) == 0.0f;
        if (std::holds_alternative<int32_t>(constant->value))
            return std::get<int32_t>(constant->value) == 0;
        if (std::holds_alternative<uint32_t>(constant->value))
            return std::get<uint32_t>(constant->value) == 0u;
        if (std::holds_alternative<bool>(constant->value))
            return !std::get<bool>(constant->value);
        return false;
    }

    void lowerCmpLE(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue)
            return;
        if (profile_ != GeneralProfile::Vertex || !isLiteralZero(inst.operands[1])) {
            program_.diagnostics.push_back(
                "nv40-general: only VP cmple(x, 0) lowering is supported");
            return;
        }
        VSrc src = resolve(inst.operands[0]);
        src.swizzle = {0, 0, 0, 0};
        conditionToSource_[inst.result] = src;
    }

    void lowerSelect(const IRInstruction& inst)
    {
        if (inst.operands.size() < 3 || inst.result == InvalidIRValue)
            return;
        if (profile_ != GeneralProfile::Vertex ||
            !isLiteralZero(inst.operands[1]) ||
            conditionToSource_.find(inst.operands[0]) == conditionToSource_.end()) {
            program_.diagnostics.push_back(
                "nv40-general: only VP select(cmple(x,0), 0, value) lowering is supported");
            return;
        }

        const int result = define(inst.result);
        const int mask = componentMask(inst.resultType);

        VInstr zero;
        zero.op = VOp::Mov;
        zero.dst.index = result;
        zero.dst.writemask = mask;
        zero.srcs[0] = resolve(inst.operands[1]);
        zero.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(zero);

        VInstr movc;
        movc.op = VOp::Mov;
        movc.dst.none = true;
        movc.dst.writemask = 0x1;
        movc.srcs[0] = conditionToSource_[inst.operands[0]];
        movc.srcs[0].swizzle = {0, 0, 0, 0};
        movc.ccUpdate = true;
        program_.instrs.push_back(movc);

        VInstr pred;
        pred.op = VOp::Mov;
        pred.dst.index = result;
        pred.dst.writemask = mask;
        pred.srcs[0] = resolve(inst.operands[2]);
        pred.srcs[0].swizzle = {0, 0, 0, 0};
        pred.predicate = NVFX_COND_GT;
        program_.instrs.push_back(pred);
    }

    void lowerStoreOutput(const IRInstruction& inst)
    {
        if (inst.operands.empty()) return;
        const std::string sem = toUpper(inst.semanticName);
        const int outIndex = profile_ == GeneralProfile::Vertex
            ? vertexOutputIndex(sem, inst.semanticIndex)
            : fragmentOutputIndex(sem);
        if (outIndex < 0) {
            program_.diagnostics.push_back(
                "nv40-general: unsupported output semantic " +
                inst.semanticName);
            return;
        }
        const IRValueID value = inst.operands[0];
        const bool dumpOrder = std::getenv("RSX_DUMP_ORDER") != nullptr;
        if (dumpOrder) {
            const VSrc dbg = resolve(value);
            std::fprintf(stderr,
                         "store sem=%s outIdx=%d value=%u kind=%d idx=%d phys=%d fp16=%d mask=%x\n",
                         inst.semanticName.c_str(),
                         outIndex,
                         static_cast<unsigned>(value),
                         static_cast<int>(dbg.kind),
                         dbg.index,
                         dbg.phys,
                         dbg.fp16 ? 1 : 0,
                         dbg.kind == VSrcKind::None ? 0 : dbg.swizzle[0]);
        }
        const int outMask = storeOutputMask(inst, value);
        const auto regIt = program_.valueToVReg.find(value);
        if (profile_ == GeneralProfile::Vertex &&
            regIt != program_.valueToVReg.end()) {
            int producerDefs = 0;
            int producerMask = 0;
            bool disjointProducerMasks = true;
            for (const VInstr& vi : program_.instrs) {
                if (!vi.dst.output && vi.dst.index == regIt->second) {
                    ++producerDefs;
                    if (producerMask & vi.dst.writemask)
                        disjointProducerMasks = false;
                    producerMask |= vi.dst.writemask;
                }
            }
            if (producerDefs > 1 && disjointProducerMasks &&
                useCount_[value] == 1) {
                for (VInstr& vi : program_.instrs) {
                    if (!vi.dst.output && vi.dst.index == regIt->second) {
                        vi.dst.output = true;
                        vi.dst.index = outIndex;
                        vi.dst.phys = -1;
                    }
                }
                return;
            }
        }
        if (profile_ == GeneralProfile::Fragment && outIndex == 0 &&
            regIt != program_.valueToVReg.end() &&
            !program_.instrs.empty()) {
            VInstr& producer = program_.instrs.back();
            const int producerDefs = static_cast<int>(std::count_if(
                program_.instrs.begin(), program_.instrs.end(),
                [&](const VInstr& vi) {
                    return !vi.dst.output && vi.dst.index == regIt->second;
                }));
            if (!producer.dst.output &&
                producer.op == VOp::Mov &&
                producer.dst.index == regIt->second &&
                producer.dst.writemask != outMask &&
                producerDefs > 1) {
                for (VInstr& vi : program_.instrs) {
                    if (!vi.dst.output && vi.dst.index == regIt->second)
                        vi.dst.preferredPhys = 0;
                }
                return;
            }
            if (!producer.dst.output &&
                producer.op != VOp::Mov &&
                producer.dst.index == regIt->second &&
                producer.dst.writemask != outMask &&
                producerDefs > 1 &&
                producer.preservePartialOutputMask) {
                for (VInstr& vi : program_.instrs) {
                    if (!vi.dst.output && vi.dst.index == regIt->second)
                        vi.dst.preferredPhys = 0;
                }
                producer.dst.output = true;
                producer.dst.index = outIndex;
                producer.dst.phys = -1;
                return;
            }
        }
        if (regIt != program_.valueToVReg.end() &&
            useCount_[value] == 1 &&
            !program_.instrs.empty()) {
            VInstr& producer = program_.instrs.back();
            if (!producer.dst.output && producer.dst.index == regIt->second) {
                producer.dst.output = true;
                producer.dst.index = outIndex;
                producer.dst.phys = -1;
                producer.dst.writemask = outMask;
                return;
            }
        }
        VInstr vi;
        vi.op = VOp::Mov;
        vi.dst.output = true;
        vi.dst.index = outIndex;
        vi.dst.writemask = outMask;
        vi.srcs[0] = resolve(value);
        int sourceWidth = -1;
        for (const auto& p : entry_.parameters) {
            if (p.valueId == value) {
                sourceWidth = p.type.componentCount();
                break;
            }
        }
        if (sourceWidth < 0) {
            const IRValue* irValue = entry_.getValue(value);
            if (irValue)
                sourceWidth = irValue->type.componentCount();
        }
        if (sourceWidth < 0)
            sourceWidth = inst.resultType.componentCount();
        if (sourceWidth == 2 && outMask == 0x3) {
            vi.srcs[0].swizzle = {0, 1, 0, 0};
        } else if (sourceWidth == 3 && outMask == 0x7) {
            vi.srcs[0].swizzle = {0, 1, 2, 0};
        } else if (sourceWidth == 1 && outMask == 0xf) {
            vi.srcs[0].swizzle = {0, 0, 0, 0};
        }
        program_.instrs.push_back(vi);
    }

    int storeOutputMask(const IRInstruction& inst, IRValueID value) const
    {
        const std::string sem = toUpper(inst.semanticName);
        for (const auto& p : entry_.parameters) {
            if (p.storage != StorageQualifier::Out &&
                p.storage != StorageQualifier::InOut)
                continue;
            if (toUpper(p.semanticName) == sem &&
                p.semanticIndex == inst.semanticIndex)
                return componentMask(p.type);
        }

        const IRValue* irValue = entry_.getValue(value);
        return componentMask(irValue ? irValue->type : inst.resultType);
    }

    static bool isArithmeticOp(VOp op)
    {
        switch (op) {
        case VOp::Add:
        case VOp::Mul:
        case VOp::Mad:
        case VOp::Dp3:
        case VOp::Dp4:
        case VOp::Min:
        case VOp::Max:
            return true;
        default:
            return false;
        }
    }

    bool isPreclampedFragmentColor(const VSrc& src) const
    {
        return profile_ == GeneralProfile::Fragment &&
               src.kind == VSrcKind::Input &&
               (src.index == NVFX_FP_OP_INPUT_SRC_COL0 ||
                src.index == NVFX_FP_OP_INPUT_SRC_COL1);
    }

    bool isHalfPrecisionFragmentInput(const VSrc& src) const
    {
        return isPreclampedFragmentColor(src);
    }

    static int requiredSourceMask(const VInstr& vi)
    {
        switch (vi.op) {
        case VOp::Dp3: return 0x7;
        case VOp::Dp4: return 0xf;
        case VOp::Rcp:
        case VOp::Rsq:
        case VOp::Sin:
        case VOp::Cos:
        case VOp::Lg2:
        case VOp::Ex2:
            return 0x1;
        default:       return vi.dst.writemask;
        }
    }

    bool isLiteralOne(IRValueID id) const
    {
        const IRValue* value = entry_.getValue(id);
        auto* constant = dynamic_cast<const IRConstant*>(value);
        if (!constant) return false;
        if (std::holds_alternative<float>(constant->value))
            return std::get<float>(constant->value) == 1.0f;
        if (std::holds_alternative<int32_t>(constant->value))
            return std::get<int32_t>(constant->value) == 1;
        if (std::holds_alternative<uint32_t>(constant->value))
            return std::get<uint32_t>(constant->value) == 1u;
        return false;
    }

    int valueComponentMask(IRValueID id) const
    {
        const IRValue* value = entry_.getValue(id);
        return componentMask(value ? value->type : IRTypeInfo::Float4());
    }

    static void applyDp3Swizzle(VSrc& src)
    {
        if (src.kind != VSrcKind::None)
            src.swizzle = {0, 1, 2, 0};
    }

    void legalizeInputOperands()
    {
        std::vector<VInstr> shaped;
        shaped.reserve(program_.instrs.size());

        for (VInstr vi : program_.instrs) {
            if (!isArithmeticOp(vi.op)) {
                shaped.push_back(vi);
                continue;
            }

            std::vector<int> inputs;
            std::vector<size_t> inlineConstPositions;
            for (size_t srcIndex = 0; srcIndex < vi.srcs.size(); ++srcIndex) {
                const VSrc& src = vi.srcs[srcIndex];
                if (src.kind == VSrcKind::Input &&
                    std::find(inputs.begin(), inputs.end(), src.index) == inputs.end())
                    inputs.push_back(src.index);
                if (src.kind == VSrcKind::Uniform ||
                    src.kind == VSrcKind::Literal)
                    inlineConstPositions.push_back(srcIndex);
            }
            const bool forceFpInputPreload =
                profile_ == GeneralProfile::Fragment && vi.op == VOp::Mad &&
                !inputs.empty();
            const bool needsInlineConstPreload = inlineConstPositions.size() > 1;
            if (!forceFpInputPreload && !needsInlineConstPreload) {
                shaped.push_back(vi);
                continue;
            }

            std::set<int> directInputs;
            if (profile_ == GeneralProfile::Vertex && !inputs.empty())
                directInputs.insert(inputs.front());

            struct PendingPreload
            {
                size_t srcIndex = 0;
                VInstr mov;
            };
            std::vector<PendingPreload> fullPreloads;
            std::vector<PendingPreload> halfPreloads;

            for (size_t srcIndex = 0; srcIndex < vi.srcs.size(); ++srcIndex) {
                VSrc& src = vi.srcs[srcIndex];
                if (src.kind != VSrcKind::Input)
                    continue;
                if (directInputs.find(src.index) != directInputs.end())
                    continue;
                if (profile_ == GeneralProfile::Fragment &&
                    vi.op == VOp::Mad &&
                    isHalfPrecisionFragmentInput(src)) {
                    continue;
                }

                VInstr mov;
                mov.op = VOp::Mov;
                mov.dst.index = newVReg();
                mov.dst.writemask = requiredSourceMask(vi);
                mov.dst.fp16 = profile_ == GeneralProfile::Fragment &&
                                isHalfPrecisionFragmentInput(src);
                program_.vregToFp16[mov.dst.index] = mov.dst.fp16;
                mov.srcs[0] = src;
                if (vi.op == VOp::Dp3 && profile_ != GeneralProfile::Fragment)
                    applyDp3Swizzle(mov.srcs[0]);
                if (vi.op == VOp::Dp3 && profile_ == GeneralProfile::Fragment &&
                    srcIndex == 1) {
                    mov.dst.writemask = 0xb; // the reference compiler uses Rn.xyw for FP DP3 rhs.
                    mov.srcs[0].swizzle = {0, 1, 2, 2};
                }
                mov.srcs[0].neg = false;
                mov.srcs[0].abs = false;

                PendingPreload pending{srcIndex, mov};
                if (mov.dst.fp16)
                    halfPreloads.push_back(pending);
                else
                    fullPreloads.push_back(pending);
            }

            const int fullCount = static_cast<int>(fullPreloads.size());
            if (profile_ == GeneralProfile::Fragment &&
                halfPreloads.size() == 1 && fullCount > 0) {
                halfPreloads[0].mov.dst.preferredPhys = fullCount == 1 ? 0 : 2;
                int nextFullPhys = 0;
                for (PendingPreload& pending : fullPreloads) {
                    if (nextFullPhys == (halfPreloads[0].mov.dst.preferredPhys >> 1))
                        ++nextFullPhys;
                    pending.mov.dst.preferredPhys = nextFullPhys++;
                }
            }

            auto appendPreload = [&](PendingPreload& pending) {
                shaped.push_back(pending.mov);

                VSrc& src = vi.srcs[pending.srcIndex];
                const bool neg = src.neg;
                const bool abs = src.abs;
                src = tempSrc(pending.mov.dst.index);
                src.fp16 = pending.mov.dst.fp16;
                if (vi.op == VOp::Dp3 && profile_ != GeneralProfile::Fragment)
                    applyDp3Swizzle(src);
                if (vi.op == VOp::Dp3 && profile_ == GeneralProfile::Fragment &&
                    pending.srcIndex == 1)
                    src.swizzle = {0, 1, 3, 2};
                src.neg = neg;
                src.abs = abs;
            };

            if (profile_ == GeneralProfile::Fragment && !halfPreloads.empty()) {
                for (PendingPreload& pending : fullPreloads)
                    appendPreload(pending);
                for (PendingPreload& pending : halfPreloads)
                    appendPreload(pending);
            } else {
                std::vector<PendingPreload> ordered;
                ordered.reserve(fullPreloads.size() + halfPreloads.size());
                ordered.insert(ordered.end(), fullPreloads.begin(), fullPreloads.end());
                ordered.insert(ordered.end(), halfPreloads.begin(), halfPreloads.end());
                std::sort(ordered.begin(), ordered.end(),
                          [](const PendingPreload& a, const PendingPreload& b) {
                              return a.srcIndex < b.srcIndex;
                          });
                for (PendingPreload& pending : ordered)
                    appendPreload(pending);
            }

            int keepInlinePosition = -1;
            for (size_t pos : inlineConstPositions) {
                if (vi.srcs[pos].kind == VSrcKind::Literal)
                    keepInlinePosition = static_cast<int>(pos);
            }
            if (keepInlinePosition < 0 && !inlineConstPositions.empty())
                keepInlinePosition = static_cast<int>(inlineConstPositions.front());

            for (size_t srcIndex = 0; srcIndex < vi.srcs.size(); ++srcIndex) {
                VSrc& src = vi.srcs[srcIndex];
                if (src.kind != VSrcKind::Uniform &&
                    src.kind != VSrcKind::Literal)
                    continue;
                if (static_cast<int>(srcIndex) == keepInlinePosition)
                    continue;

                VInstr mov;
                mov.op = VOp::Mov;
                mov.dst.index = newVReg();
                mov.dst.writemask = requiredSourceMask(vi);
                mov.srcs[0] = src;
                shaped.push_back(mov);

                src = tempSrc(mov.dst.index);
            }

            if (vi.op == VOp::Dp3 && profile_ != GeneralProfile::Fragment) {
                applyDp3Swizzle(vi.srcs[0]);
                applyDp3Swizzle(vi.srcs[1]);
            }

            shaped.push_back(vi);
        }

        program_.instrs = std::move(shaped);
    }

    void allocatePhysicalTemps()
    {
        std::unordered_map<int, size_t> lastUse;
        std::set<int> defs;
        const bool dumpOrder = std::getenv("RSX_DUMP_ORDER") != nullptr;
        for (size_t i = 0; i < program_.instrs.size(); ++i) {
            const VInstr& vi = program_.instrs[i];
            if (!vi.dst.none && !vi.dst.output)
                defs.insert(vi.dst.index);
            for (const VSrc& src : vi.srcs) {
                if (src.kind == VSrcKind::Temp)
                    lastUse[src.index] = i;
            }
        }

        std::vector<int> freeList;
        int nextPhys = 0;
        // FP uses the same def-order identity as VP (reverse virtual-order
        // assignment, just across the 4-register fragment bank).
        if (profile_ == GeneralProfile::Fragment)
            nextPhys = static_cast<int>(defs.size()) - 1;
        for (size_t i = 0; i < program_.instrs.size(); ++i) {
            VInstr& vi = program_.instrs[i];
            for (VSrc& src : vi.srcs) {
                if (src.kind == VSrcKind::Temp) {
                    const auto physIt = program_.vregToPhys.find(src.index);
                    if (physIt != program_.vregToPhys.end())
                        src.phys = physIt->second;
                    const auto fp16It = program_.vregToFp16.find(src.index);
                    src.fp16 = fp16It != program_.vregToFp16.end() && fp16It->second;
                }
            }
            if (!vi.dst.none &&
                !vi.dst.output &&
                program_.vregToPhys.find(vi.dst.index) == program_.vregToPhys.end()) {
                int phys;
                if (vi.dst.preferredPhys >= 0) {
                    phys = vi.dst.preferredPhys;
                } else {
                    auto reusableSrc = std::find_if(
                        vi.srcs.begin(), vi.srcs.end(),
                        [&](const VSrc& src) {
                            return src.kind == VSrcKind::Temp &&
                                   !src.fp16 &&
                                   !vi.dst.fp16 &&
                                   lastUse[src.index] == i &&
                       program_.vregToPhys.find(src.index) != program_.vregToPhys.end();
                        });
                    if (reusableSrc != vi.srcs.end()) {
                        phys = program_.vregToPhys[reusableSrc->index];
                    } else if (!vi.dst.fp16 && !freeList.empty()) {
                        phys = freeList.back();
                        freeList.pop_back();
                    } else {
                        // FP H registers have their own index space but alias
                        // full R slots in pairs: H0/H1 -> R0, H2/H3 -> R1.
                        if (profile_ == GeneralProfile::Fragment) {
                            phys = vi.dst.fp16 ? (nextPhys-- << 1) : nextPhys--;
                        } else {
                            phys = vi.dst.fp16 ? (nextPhys++ << 1) : nextPhys++;
                        }
                    }
                }
                program_.vregToPhys[vi.dst.index] = phys;
            }
            if (!vi.dst.none && !vi.dst.output) {
                vi.dst.phys = program_.vregToPhys[vi.dst.index];
                program_.vregToFp16[vi.dst.index] = vi.dst.fp16;
            }
            if (dumpOrder) {
                std::fprintf(stderr, "alloc[%zu] op=%d dstOut=%d dstIdx=%d dstPhys=%d dstFp16=%d\n",
                             i,
                             static_cast<int>(vi.op),
                             vi.dst.output ? 1 : 0,
                             vi.dst.index,
                             vi.dst.phys,
                             vi.dst.fp16 ? 1 : 0);
                for (size_t s = 0; s < vi.srcs.size(); ++s) {
                    const VSrc& src = vi.srcs[s];
                    std::fprintf(stderr,
                                 "  src%zu kind=%d idx=%d phys=%d fp16=%d swz=%d%d%d%d neg=%d abs=%d\n",
                                 s,
                                 static_cast<int>(src.kind),
                                 src.index,
                                 src.phys,
                                 src.fp16 ? 1 : 0,
                                 src.swizzle[0], src.swizzle[1], src.swizzle[2], src.swizzle[3],
                                 src.neg ? 1 : 0,
                                 src.abs ? 1 : 0);
                }
            }
            for (const VSrc& src : vi.srcs) {
                if (src.kind == VSrcKind::Temp && !src.fp16 && lastUse[src.index] == i) {
                    const int freed = program_.vregToPhys[src.index];
                    if (vi.dst.none || vi.dst.output || vi.dst.phys != freed)
                        freeList.push_back(freed);
                }
            }
        }
    }
};

static struct nvfx_reg regFromSource(const VSrc& src)
{
    switch (src.kind) {
    case VSrcKind::Temp:
    {
        struct nvfx_reg r = nvfx_reg(NVFXSR_TEMP,
                                     src.phys >= 0 ? src.phys : src.index);
        r.is_fp16 = src.fp16 ? 1 : 0;
        return r;
    }
    case VSrcKind::Input:
        return nvfx_reg(NVFXSR_INPUT, src.index);
    case VSrcKind::Uniform:
        return nvfx_reg(NVFXSR_CONST, src.embeddedUniform ? 0 : src.index);
    case VSrcKind::Literal:
        return nvfx_reg(NVFXSR_CONST, src.index);
    case VSrcKind::None:
    default:
        return nvfx_reg(NVFXSR_NONE, 0);
    }
}

static struct nvfx_src nvfxSource(const VSrc& src)
{
    struct nvfx_reg r = regFromSource(src);
    struct nvfx_src s = nvfx_src(r);
    s = nvfx_src_swz(s, src.swizzle[0], src.swizzle[1],
                     src.swizzle[2], src.swizzle[3]);
    if (src.neg)
        s = nvfx_src_neg(s);
    if (src.abs)
        s = nvfx_src_abs(s);
    return s;
}

static uint8_t fpOpcode(VOp op)
{
    switch (op) {
    case VOp::Mov: return NVFX_FP_OP_OPCODE_MOV;
    case VOp::Add: return NVFX_FP_OP_OPCODE_ADD;
    case VOp::Mul: return NVFX_FP_OP_OPCODE_MUL;
    case VOp::Mad: return NVFX_FP_OP_OPCODE_MAD;
    case VOp::Dp3: return NVFX_FP_OP_OPCODE_DP3;
    case VOp::Dp4: return NVFX_FP_OP_OPCODE_DP4;
    case VOp::Max: return NVFX_FP_OP_OPCODE_MAX;
    case VOp::Min: return NVFX_FP_OP_OPCODE_MIN;
    case VOp::Rcp: return NVFX_FP_OP_OPCODE_RCP;
    case VOp::Rsq: return NVFX_FP_OP_OPCODE_RSQ;
    case VOp::Sin: return NVFX_FP_OP_OPCODE_SIN;
    case VOp::Cos: return NVFX_FP_OP_OPCODE_COS;
    case VOp::Lg2: return NVFX_FP_OP_OPCODE_LG2;
    case VOp::Ex2: return NVFX_FP_OP_OPCODE_EX2;
    case VOp::DivSqrt: return NVFX_FP_OP_OPCODE_DIVRSQ_NV40RSX;
    case VOp::Sge: return NVFX_FP_OP_OPCODE_SGE;
    case VOp::Tex: return NVFX_FP_OP_OPCODE_TEX;
    }
    return NVFX_FP_OP_OPCODE_MOV;
}

#define VP_OP(NAME) ((NVFX_VP_INST_SLOT_VEC << 7) | NVFX_VP_INST_VEC_OP_##NAME)
#define VP_SCA_OP(NAME) ((NVFX_VP_INST_SLOT_SCA << 7) | NVFX_VP_INST_SCA_OP_##NAME)

static uint8_t vpOpcode(VOp op)
{
    switch (op) {
    case VOp::Mov: return VP_OP(MOV);
    case VOp::Add: return VP_OP(ADD);
    case VOp::Mul: return VP_OP(MUL);
    case VOp::Mad: return VP_OP(MAD);
    case VOp::Dp3: return VP_OP(DP3);
    case VOp::Dp4: return VP_OP(DP4);
    case VOp::Max: return VP_OP(MAX);
    case VOp::Min: return VP_OP(MIN);
    case VOp::Rcp: return VP_SCA_OP(RCP);
    case VOp::Rsq: return VP_SCA_OP(RSQ);
    case VOp::Sin: return VP_SCA_OP(SIN);
    case VOp::Cos: return VP_SCA_OP(COS);
    case VOp::Lg2: return VP_SCA_OP(LG2);
    case VOp::Ex2: return VP_SCA_OP(EX2);
    case VOp::DivSqrt:
    case VOp::Sge:
    case VOp::Tex: return VP_OP(MOV); // VP texture fetch is intentionally unsupported.
    }
    return VP_OP(MOV);
}

static bool hasUnsupportedSource(const VInstr& vi, std::string& why)
{
    (void)vi;
    (void)why;
    return false;
}

static bool isVpScalarOp(VOp op)
{
    switch (op) {
    case VOp::Rcp:
    case VOp::Rsq:
    case VOp::Sin:
    case VOp::Cos:
    case VOp::Lg2:
    case VOp::Ex2:
        return true;
    default:
        return false;
    }
}

static bool isVpVectorOp(VOp op)
{
    return !isVpScalarOp(op) && op != VOp::Tex;
}

static bool sameTempRegister(const VSrc& src, const VDst& dst)
{
    return src.kind == VSrcKind::Temp && !dst.output &&
           src.phys >= 0 && dst.phys >= 0 && src.phys == dst.phys;
}

static bool sameDestinationRegister(const VDst& a, const VDst& b)
{
    if (a.output != b.output)
        return false;
    return a.output ? a.index == b.index
                    : a.phys >= 0 && b.phys >= 0 && a.phys == b.phys;
}

static int singleInputSourceIndex(const VInstr& instr)
{
    int seen = -1;
    for (const VSrc& src : instr.srcs) {
        if (src.kind != VSrcKind::Input)
            continue;
        if (seen >= 0 && seen != src.index)
            return -2;
        seen = src.index;
    }
    return seen;
}

static bool canCoissueVp(const VInstr& sca, const VInstr& vec)
{
    if (!isVpScalarOp(sca.op) || !isVpVectorOp(vec.op))
        return false;
    if (sca.srcs[1].kind != VSrcKind::None ||
        sca.srcs[2].kind != VSrcKind::None)
        return false;
    const int scaInput = singleInputSourceIndex(sca);
    const int vecInput = singleInputSourceIndex(vec);
    if (scaInput == -2 || vecInput == -2)
        return false;
    if (scaInput >= 0 && vecInput >= 0 && scaInput != vecInput)
        return false;
    if (sameDestinationRegister(sca.dst, vec.dst))
        return false;
    for (const VSrc& src : vec.srcs) {
        if (sameTempRegister(src, sca.dst))
            return false;
    }
    if (sameTempRegister(sca.srcs[0], vec.dst))
        return false;
    return true;
}

static bool floatBitsEqual(float a, float b)
{
    uint32_t pa = 0;
    uint32_t pb = 0;
    std::memcpy(&pa, &a, sizeof(pa));
    std::memcpy(&pb, &b, sizeof(pb));
    return pa == pb;
}

static VSrc assignVpLiteralSource(const VSrc& literal,
                                  VpAttributes& attrs,
                                  int& nextLiteralReg)
{
    VSrc out = literal;
    out.kind = VSrcKind::Uniform;
    out.embeddedUniform = false;

    const float value = literal.literal[0];
    for (const auto& slot : attrs.literalPool) {
        for (uint32_t lane = 0; lane < slot.usedLanes; ++lane) {
            if (floatBitsEqual(slot.values[lane], value)) {
                out.index = static_cast<int>(slot.constReg);
                out.swizzle = {static_cast<uint8_t>(lane),
                               static_cast<uint8_t>(lane),
                               static_cast<uint8_t>(lane),
                               static_cast<uint8_t>(lane)};
                return out;
            }
        }
    }

    if (!attrs.literalPool.empty()) {
        auto& slot = attrs.literalPool.back();
        if (slot.usedLanes < 4) {
            const uint32_t lane = slot.usedLanes++;
            slot.values[lane] = value;
            out.index = static_cast<int>(slot.constReg);
            out.swizzle = {static_cast<uint8_t>(lane),
                           static_cast<uint8_t>(lane),
                           static_cast<uint8_t>(lane),
                           static_cast<uint8_t>(lane)};
            return out;
        }
    }

    VpLiteralPoolSlot slot;
    slot.constReg = static_cast<uint32_t>(nextLiteralReg--);
    slot.usedLanes = 1;
    slot.values[0] = value;
    attrs.literalPool.push_back(slot);

    out.index = static_cast<int>(slot.constReg);
    out.swizzle = {0, 0, 0, 0};
    return out;
}

static void populateReferencedParams(const IRFunction& entry, FpAttributes& attrs)
{
    std::unordered_set<IRValueID> usedValueIds;
    for (const auto& blockPtr : entry.blocks) {
        if (!blockPtr) continue;
        for (const auto& instPtr : blockPtr->instructions) {
            if (!instPtr) continue;
            for (IRValueID id : instPtr->operands)
                usedValueIds.insert(id);
        }
    }

    for (size_t i = 0; i < entry.parameters.size(); ++i) {
        const auto& p = entry.parameters[i];
        const bool isOut = (p.storage == StorageQualifier::Out ||
                            p.storage == StorageQualifier::InOut);
        if (isOut || usedValueIds.count(p.valueId))
            attrs.referencedParamIndices.insert(static_cast<unsigned>(i));
    }
}

static void seedFpEmbeddedUniforms(const IRFunction& entry, FpAttributes& attrs)
{
    for (size_t i = 0; i < entry.parameters.size(); ++i) {
        const auto& p = entry.parameters[i];
        if (p.storage != StorageQualifier::Uniform)
            continue;
        if (p.type.baseType == IRType::Sampler2D ||
            p.type.baseType == IRType::SamplerRect ||
            p.type.baseType == IRType::SamplerCube)
            continue;
        attrs.embeddedUniforms.push_back({static_cast<unsigned>(i), {}});
    }
}

static void recordFpUniformOffset(FpAttributes& attrs,
                                  unsigned paramIndex,
                                  uint32_t ucodeByteOffset)
{
    for (auto& uniform : attrs.embeddedUniforms) {
        if (uniform.entryParamIndex == paramIndex) {
            uniform.ucodeByteOffsets.push_back(ucodeByteOffset);
            return;
        }
    }
}

static bool fpProducerNeedsFenctr(VOp op)
{
    switch (op) {
    case VOp::Tex:
    case VOp::Rcp:
    case VOp::Rsq:
    case VOp::Sin:
    case VOp::Cos:
    case VOp::Lg2:
    case VOp::Ex2:
    case VOp::DivSqrt:
        return true;
    default:
        return false;
    }
}

static UcodeOutput emitFragmentVirtual(VirtualProgram& program,
                                       const IRFunction& entry,
                                       FpAttributes* attrsOut)
{
    UcodeOutput out;
    FpAssembler asm_;
    FpAttributes attrs;
    populateReferencedParams(entry, attrs);
    seedFpEmbeddedUniforms(entry, attrs);
    std::unordered_map<int, VOp> tempProducerOp;
    for (const VInstr& vi : program.instrs) {
        if (!vi.dst.none && !vi.dst.output)
            tempProducerOp[vi.dst.index] = vi.op;
    }

    const auto emitFenceForSources = [&](const std::array<VSrc, 3>& srcs) {
        const bool needsCtr = std::any_of(srcs.begin(), srcs.end(),
            [&](const VSrc& src) {
                if (src.kind != VSrcKind::Temp)
                    return false;
                const auto it = tempProducerOp.find(src.index);
                return it != tempProducerOp.end() &&
                       fpProducerNeedsFenctr(it->second);
            });
        if (needsCtr)
            asm_.emitFenctr();
        else
            asm_.emitFencbr();
    };

    bool emittedInstruction = false;
    for (const VInstr& vi : program.instrs) {
        std::string why;
        if (hasUnsupportedSource(vi, why)) {
            out.diagnostics.push_back("nv40-general-fp: " + why);
            return out;
        }
        if (vi.op == VOp::Tex)
            attrs.partialTexType &= ~(3u << (vi.texUnit * 2));
        for (const VSrc& src : vi.srcs) {
            if (src.kind == VSrcKind::Input) {
                attrs.attributeInputMask |= fpAttrMaskBitForInputSrc(src.index);
                if (src.index >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                    src.index <= NVFX_FP_OP_INPUT_SRC_TC(7)) {
                    const uint16_t bit =
                        uint16_t{1} << (src.index - NVFX_FP_OP_INPUT_SRC_TC(0));
                    attrs.texCoordsInputMask |= bit;
                    const bool scalarYRead =
                        src.swizzle[0] == 1 && src.swizzle[1] == 1 &&
                        src.swizzle[2] == 1 && src.swizzle[3] == 1;
                    if (vi.op != VOp::Tex && !scalarYRead)
                        attrs.texCoords2D &= ~bit;
                }
            }
        }
        struct nvfx_reg dst = vi.dst.output
            ? nvfx_reg(NVFXSR_OUTPUT, vi.dst.index)
            : nvfx_reg(NVFXSR_TEMP, vi.dst.phys);
        dst.is_fp16 = vi.dst.fp16 ? 1 : 0;
        std::array<VSrc, 3> srcs = vi.srcs;
        // Match the reference compiler's inline-constant canonicalization:
        // MOV keeps const at operand 0, ADD/MUL prefer operand 1, and
        // MAD keeps the addend at operand 2.  Fence selection depends on
        // the producer class of the live temp sources, so keep these together.
        if (vi.op == VOp::Add &&
            srcs[1].kind == VSrcKind::Uniform && srcs[1].neg)
            std::swap(srcs[0], srcs[1]);
        if (vi.op == VOp::Add &&
            srcs[0].kind == VSrcKind::Temp && srcs[0].fp16 &&
            srcs[1].kind != VSrcKind::None && !srcs[1].fp16)
            std::swap(srcs[0], srcs[1]);
        if (vi.op == VOp::Mul &&
            srcs[0].kind == VSrcKind::Uniform &&
            srcs[1].kind == VSrcKind::Temp)
            std::swap(srcs[0], srcs[1]);
        auto isInlineConst = [](const VSrc& src) {
            return src.kind == VSrcKind::Uniform ||
                   src.kind == VSrcKind::Literal;
        };
        // Locked const-FENC rule: direct MOV-to-output folds an inline const
        // into the prologue fence carrier (RE-confirmed).  The fence type is
        // chosen from the producer class of the live temp sources.
        if (vi.op == VOp::Mov && vi.dst.output &&
            std::any_of(srcs.begin(), srcs.end(), isInlineConst))
            emitFenceForSources(srcs);
        if (emittedInstruction && vi.op == VOp::Mad &&
            std::any_of(srcs.begin(), srcs.end(), isInlineConst))
            emitFenceForSources(srcs);
        if (vi.stubFenceBefore)
            asm_.emitFenctr();
        if (vi.stubFenceBrBefore)
            asm_.emitFencbr();
        struct nvfx_insn insn = nvfx_insn(
            vi.sat, 0, vi.texUnit, 0,
            dst,
            vi.dst.writemask,
            nvfxSource(srcs[0]),
            nvfxSource(srcs[1]),
            nvfxSource(srcs[2]));
        if (vi.dst.fp16)
            insn.precision = FLOAT16;
        if (vi.fpPrecisionOverride >= 0)
            insn.precision = static_cast<uint8_t>(vi.fpPrecisionOverride);
        if (vi.fpScale)
            insn.scale = vi.fpScale;
        if (vi.disablePc)
            insn.disable_pc = 1;
        asm_.emit(insn, fpOpcode(vi.op));
        emittedInstruction = true;
        for (const VSrc& src : srcs) {
            if (src.kind != VSrcKind::Uniform &&
                src.kind != VSrcKind::Literal)
                continue;
            const uint32_t offset = asm_.currentByteSize();
            if (src.kind == VSrcKind::Uniform) {
                recordFpUniformOffset(attrs, static_cast<unsigned>(src.index), offset);
                static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                asm_.appendConstBlock(zeros);
            } else {
                asm_.appendConstBlock(src.literal.data());
            }
        }
    }
    if (asm_.empty()) {
        out.diagnostics.push_back("nv40-general-fp: no instructions emitted");
        return out;
    }
    asm_.markEnd();
    out.words = asm_.words();
    out.ok = true;
    attrs.registerCount = static_cast<uint8_t>(std::max(2, asm_.numTempRegs()));
    if (attrsOut)
        *attrsOut = attrs;
    return out;
}

static UcodeOutput emitVertexVirtual(VirtualProgram& program,
                                     VpAttributes* attrsOut)
{
    UcodeOutput out;
    VpAssembler asm_;
    VpAttributes attrs;
    int nextLiteralReg = program.nextVpLiteralConst;
    for (VInstr& vi : program.instrs) {
        for (VSrc& src : vi.srcs) {
            if (src.kind == VSrcKind::Literal)
                src = assignVpLiteralSource(src, attrs, nextLiteralReg);
        }
    }

    auto makeInsn = [](const VInstr& vi) {
        const struct nvfx_reg dst = vi.dst.none
            ? nvfx_reg(NVFXSR_NONE, 0)
            : vi.dst.output
            ? nvfx_reg(NVFXSR_OUTPUT, vi.dst.index)
            : nvfx_reg(NVFXSR_TEMP, vi.dst.phys);
        const int dstMask = vpMaskFromComponentMask(vi.dst.writemask);
        std::array<VSrc, 3> srcs = vi.srcs;
        if (vi.op == VOp::Add)
            std::swap(srcs[1], srcs[2]);
        struct nvfx_insn insn = nvfx_insn(
            vi.sat, 0, 0, 0,
            const_cast<struct nvfx_reg&>(dst),
            dstMask,
            nvfxSource(srcs[0]),
            nvfxSource(srcs[1]),
            nvfxSource(srcs[2]));
        if (vi.ccUpdate)
            insn.cc_update = 1;
        if (vi.predicate) {
            insn.cc_test = 1;
            insn.cc_cond = vi.predicate;
            insn.cc_swz[0] = insn.cc_swz[1] =
                insn.cc_swz[2] = insn.cc_swz[3] = 0;
        }
        return insn;
    };

    for (size_t i = 0; i < program.instrs.size(); ++i) {
        const VInstr& vi = program.instrs[i];
        std::string why;
        if (hasUnsupportedSource(vi, why)) {
            out.diagnostics.push_back("nv40-general-vp: " + why);
            return out;
        }
        if (vi.op == VOp::Tex) {
            out.diagnostics.push_back("nv40-general-vp: texture fetch unsupported in VP");
            return out;
        }
        if (i + 1 < program.instrs.size()) {
            const VInstr& next = program.instrs[i + 1];
            if (canCoissueVp(vi, next)) {
                asm_.emitCoIssued(makeInsn(next), vpOpcode(next.op),
                                  makeInsn(vi), vpOpcode(vi.op));
                ++i;
                continue;
            }
        }
        asm_.emit(makeInsn(vi), vpOpcode(vi.op));
    }
    if (asm_.empty()) {
        out.diagnostics.push_back("nv40-general-vp: no instructions emitted");
        return out;
    }
    asm_.markLast();
    out.words = asm_.words();
    out.ok = true;
    attrs.registerCount = static_cast<uint32_t>(std::max(1, asm_.numTempRegs()));
    attrs.attributeInputMask = asm_.inputMask();
    attrs.attributeOutputMask = asm_.outputMask();
    if (attrsOut)
        *attrsOut = attrs;
    return out;
}

static void appendBuilderDiagnostics(const VirtualProgram& program, UcodeOutput& out)
{
    out.diagnostics.insert(out.diagnostics.end(),
                           program.diagnostics.begin(),
                           program.diagnostics.end());
    if (!out.ok && out.diagnostics.empty())
        out.diagnostics.push_back("nv40-general: unsupported shader shape");
}

}  // namespace

UcodeOutput lowerFragmentProgramGeneral(const IRModule& module,
                                        const IRFunction& entry,
                                        const rsx_cg::CompileOptions&,
                                        FpAttributes* attrsOut)
{
    GeneralBuilder builder(GeneralProfile::Fragment, entry, module);
    VirtualProgram program = builder.run();
    UcodeOutput out = emitFragmentVirtual(program, entry, attrsOut);
    appendBuilderDiagnostics(program, out);
    return out;
}

UcodeOutput lowerVertexProgramGeneral(const IRModule& module,
                                      const IRFunction& entry,
                                      const rsx_cg::CompileOptions&,
                                      VpAttributes* attrsOut)
{
    GeneralBuilder builder(GeneralProfile::Vertex, entry, module);
    VirtualProgram program = builder.run();
    UcodeOutput out = emitVertexVirtual(program, attrsOut);
    appendBuilderDiagnostics(program, out);
    return out;
}

}  // namespace nv40::detail
