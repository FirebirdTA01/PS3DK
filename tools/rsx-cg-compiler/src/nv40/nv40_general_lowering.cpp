/*
 * The general NV40 lowering pipeline - the default since D1.
 *
 * The shape matcher this replaced is still reachable for one release
 * as `--legacy-lowering` (RSXCG_GENERAL=0), so a divergence can be
 * bisected against it rather than argued about.
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

// Before the nv40 headers: nvfx_shader.h defines a function-like
// `abs` macro that poisons <cmath> if the standard header is included
// after it (std::isfinite is used by the CF-1a finite-arm check).
#include <cmath>

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

const char* unsupportedProfileOperator(IROp op)
{
    switch (op)
    {
    case IROp::And: return "&";
    case IROp::Or:  return "|";
    case IROp::Xor: return "^";
    case IROp::Not: return "~";
    case IROp::Shl: return "<<";
    case IROp::Shr: return ">>";
    default:        return nullptr;
    }
}

std::string profileUnsupportedDiagnostic(const IRInstruction& inst)
{
    const char* op = unsupportedProfileOperator(inst.op);
    if (!op) return {};
    std::string msg;
    if (!inst.loc.filename.empty())
        msg = inst.loc.toString() + ": ";
    msg += std::string("error C5508: the operator \"") + op +
           "\" is not supported by this profile";
    return msg;
}

enum class GeneralProfile { Fragment, Vertex };

enum class VOp
{
    Mov,
    Add,
    Mul,
    Mad,
    Dp2,
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
    Frc,
    Flr,
    Sge,
    Slt,
    Sgt,
    Sle,
    Seq,
    Sne,
    Tex,
    // CF-1b pseudo-op: predicated select, one VInstr carrying
    // (cond, thenVal, elseVal) that the FP emitter expands to three
    // hardware instructions (MOV default; CC-set from cond; CC-gated
    // commit).  A single node on purpose: the ordering pass tracks
    // only temp-read dependencies — no CC hazards, no same-register
    // write-after-write — so a three-VInstr sequence could be torn
    // apart or reordered against another CC writer.  One node makes
    // the sequence atomic by construction, the same shape as the
    // default path's PredCarry (one IR op, several hw instructions).
    SelPred,
    // CF-2 pseudo-op: a fragment kill.  ONE VInstr carrying the
    // instruction that computes the guard, expanded by the FP emitter
    // into two hardware instructions - the guard's producer retargeted
    // to the condition register with cc_update, then KIL testing that
    // register.  Atomic for exactly SelPred's reason: the ordering pass
    // tracks temp reads and knows nothing about the condition register,
    // so a separate CC writer and KIL could be torn apart, or another
    // CC writer scheduled between them.
    Kil
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
    // Meaningful lanes of `literal`.  A VECTOR literal has to keep all of
    // them: the vertex literal pool used to read literal[0] and nothing
    // else, so float4(a,b,c,d) shipped as a broadcast of `a` and every
    // store of it painted one value four times (t_3e342903).
    uint8_t  literalLanes = 1;
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
    bool outputPin = false;  // FP-only: preferredPhys IS the output slot, not a preference
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
    // VOp::Kil only: the operation that computes the guard (fused into
    // the kill and emitted with cc_update to the condition register),
    // and whether the kill fires where that guard is FALSE.  The
    // reference folds a negated guard into the KIL's condition-code test
    // rather than inverting the comparison.
    VOp  killFused = VOp::Mov;
    bool killTestEq = false;
};

struct VirtualProgram
{
    std::vector<VInstr> instrs;
    std::unordered_map<IRValueID, int> valueToVReg;
    std::unordered_map<IRValueID, VSrc> valueToSource;
    // Const-slot indices for FILE-SCOPE fragment uniforms, in the order
    // cg_container_fp.cpp walks module.globals.  Emission needs them to
    // create the embedded-offset entries, and it does not see the module.
    std::vector<unsigned> fpGlobalUniformSlots;
    // Slot -> (compiled default, component count) for a file-scope uniform
    // declared with an initialiser.  Emission writes the default into every
    // const block the parameter lists (t_3bf3ce95); it does not see the
    // module, so the lowering pass carries it here.
    std::unordered_map<unsigned, std::pair<std::vector<float>, unsigned>>
        fpUniformDefaults;
    // TEXCOORDn -> the component count it was DECLARED with.  texCoords2D
    // is a declaration property; emission sees consumed lanes, so it
    // cannot derive it without this.
    std::unordered_map<int, int> texcoordDeclaredWidth;
    std::unordered_map<int, int> vregToPhys;
    std::unordered_map<int, bool> vregToFp16;
    int nextVpLiteralConst = 467;
    // Lowest const register the literal pool may take.  Uniforms grow DOWN
    // from 467 and matrices grow UP from 256, and the literal pool
    // continues downward after the uniforms - so the matrix watermark is
    // the floor.  Carried here because emission allocates the pool and
    // does not see the uniform walk (t_3e342903 made vector literals take
    // a register each, which is what made this reachable).
    int vpConstFloor = 256;
    std::vector<std::string> diagnostics;
    // Set when ANY lowering could not complete: an unresolved operand, or an
    // IR op this path does not implement.  Emission must not succeed after
    // it.  Both used to continue quietly - an unresolved operand became an
    // empty source (vertex attribute ZERO), and an unimplemented op pushed a
    // diagnostic and emitted the rest of the program anyway, exiting 0 with a
    // shader silently missing whatever that op was supposed to do.
    bool loweringFailed = false;
    // What the pre-2026-09-02 numbering (fragment bank seeded at the
    // definition count) would have DECLARED for this program: the key of
    // the transitional capacity gate in emitFragmentVirtual.  -1 = not
    // probed (vertex, or an allocation that refused).
    int legacyDeclaredTemps = -1;
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
    // Struct-flattened loads from unbound fields carry an EMPTY semantic
    // and a positional index - the frontend's inferred-ATTR convention,
    // which the container writer already maps as empty-at-N = ATTRN.
    // The lowering must agree or those inputs are unresolvable (this was
    // one of the two general-vs-default regressions).
    if (semanticUpper.empty() && semanticIndex >= 0 && semanticIndex < 16)
        return semanticIndex;
    if (semanticUpper == "POSITION") return NVFX_VP_INST_IN_POS;
    if (semanticUpper == "NORMAL")   return NVFX_VP_INST_IN_NORMAL;
    if (semanticUpper == "COLOR" || semanticUpper == "COL")
        return semanticIndex == 1 ? NVFX_VP_INST_IN_COL1
                                  : NVFX_VP_INST_IN_COL0;
    // DIFFUSE/SPECULAR are index-less aliases of COLOR0/COLOR1; an
    // indexed DIFFUSE1 is not a thing and must stay unsupported (-1)
    // rather than silently landing on COL0.
    if (semanticUpper == "DIFFUSE"  && semanticIndex == 0) return NVFX_VP_INST_IN_COL0;
    if (semanticUpper == "SPECULAR" && semanticIndex == 0) return NVFX_VP_INST_IN_COL1;
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
    if (semanticUpper == "DIFFUSE"  && semanticIndex == 0) return NV40_VP_INST_DEST_COL0;
    if (semanticUpper == "SPECULAR" && semanticIndex == 0) return NV40_VP_INST_DEST_COL1;
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
    if (semanticUpper == "DIFFUSE"  && semanticIndex == 0) return NVFX_FP_OP_INPUT_SRC_COL0;
    if (semanticUpper == "SPECULAR" && semanticIndex == 0) return NVFX_FP_OP_INPUT_SRC_COL1;
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
    // TEXCOORD8 and TEXCOORD9 are real fragment inputs, but the reference
    // container does NOT continue the TC0..7 attribute bits at 22/23.
    // Measured on vpcov_tc8/vpcov_tc9: TC8 -> bit 12 and TC9 -> bit 13.
    if (inputSrc == NVFX_FP_OP_INPUT_SRC_TC(8) ||
        inputSrc == NVFX_FP_OP_INPUT_SRC_TC(9))
        return 1u << (4 + (inputSrc - NVFX_FP_OP_INPUT_SRC_TC(0)));
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
    // MATERIALISES THE CONSTANT BLOCK ONLY.  It deliberately does NOT set
    // the scalar broadcast swizzle: a scalar literal lands here as
    // {c,0,0,0}, and under the identity swizzle every lane past x would
    // read ZERO (`MUL R.xy, R, {6,0,0,0}` multiplies y by zero, compiles
    // clean and renders wrong - t_b6f2a2a4).  The broadcast is applied by
    // resolve(), which is the only place that knows the VALUE's width and
    // therefore the only place that can apply the same rule to a scalar
    // uniform and a scalar temp as well.  Do not reintroduce it here: a
    // per-source-kind fix is what left the uniform and computed cases
    // broken after the first attempt.
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
        const size_t n = std::min<size_t>(4, values.size());
        for (size_t i = 0; i < n; ++i)
            s.literal[i] = values[i];
        s.literalLanes = static_cast<uint8_t>(std::max<size_t>(1, n));
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
        std::vector<const IRBasicBlock*> order;
        if (!flattenBlockOrder(order))
            return program_;
        for (const IRBasicBlock* block : order) {
            for (const auto& instPtr : block->instructions) {
                if (!instPtr) continue;
                // Flattened programs execute both arms unconditionally
                // (joins arrive from the frontend as selects), so the
                // branch terminators are dropped rather than lowered.
                // Single-block programs keep the exact old behaviour: a
                // stray branch still hits the unsupported-op refusal.
                if (flattened_ &&
                    (instPtr->op == IROp::Branch ||
                     instPtr->op == IROp::CondBranch))
                    continue;
                lowerInstruction(*instPtr);
            }
        }
        legalizeInputOperands();
        renumberSourceIndices();
        applyOrderingPass();
        // TRANSITIONAL CAPACITY GATE, first half (director 2026-09-02,
        // option C): number the program the way 6ece362 did - bank seeded
        // at the definition count, spill bank above it - on a COPY, and
        // remember the register count that numbering would have declared.
        // The emitter refuses above the old capacity by that number, so
        // the seed fix ships no shape the old build did not, until each
        // shape above the old capacity has been judged on pixels.
        if (profile_ == GeneralProfile::Fragment) {
            const VirtualProgram saved = program_;
            legacyCapacityProbe_ = true;
            allocatePhysicalTemps();
            legacyCapacityProbe_ = false;
            int highest = -1;
            if (!program_.loweringFailed)
                for (const VInstr& vi : program_.instrs)
                    if (!vi.dst.none && vi.dst.phys >= 0)
                        highest = std::max(highest, vi.dst.fp16 ? (vi.dst.phys >> 1)
                                                                : vi.dst.phys);
            program_ = saved;
            program_.legacyDeclaredTemps = highest < 0 ? -1 : std::max(2, highest + 1);
        }
        allocatePhysicalTemps();
        return program_;
    }

private:
    GeneralProfile profile_;
    const IRFunction& entry_;
    const IRModule& module_;
    VirtualProgram program_;
    // True only during the capacity gate's dry run: seed the fragment bank
    // at the definition count, as before 7243bd9.
    bool legacyCapacityProbe_ = false;
    int nextVReg_ = 0;
    std::unordered_map<IRValueID, unsigned> useCount_;
    std::unordered_map<IRValueID, unsigned> nonTermUseCount_;
    // Every value some instruction PRODUCES.  A value that is neither
    // produced nor a parameter is an uninitialised declaration - `half4 c;`
    // - and an insert into it has nothing to copy.
    std::unordered_set<IRValueID> definedValues_;
    std::unordered_map<IRValueID, int> matrixUniformBase_;
    // Rows the matrix at that base actually OWNS.  A row index is
    // bounded against this, not against 4: `m[3]` on a float3x3 would
    // otherwise resolve to c[base + 3], a register belonging to
    // whatever was allocated next (review finding, codex).  The
    // reference rejects that source - "array index out of bounds".
    std::unordered_map<IRValueID, int> matrixUniformRows_;
    // Which texture unit each sampler value names.  lowerTex used to hard-
    // code unit 0, so every sampler in a program sampled the FIRST texture
    // while the container correctly described the bindings - a two-texture
    // shader silently read one texture twice.
    std::unordered_map<IRValueID, int> samplerUnit_;
    std::unordered_map<IRValueID, VSrc> conditionToSource_;
    std::unordered_map<IRValueID, int> valueWidth_;
    // CF-1a flatten state (t_91bbd575).  Set only when the entry
    // function has more than one basic block; single-block programs
    // never engage the flatten and lower exactly as before.
    bool flattened_ = false;
    std::unordered_map<IRValueID, const IRInstruction*> defMap_;

    // CF-1a (design note docs/design/shader-compiler-control-flow.md):
    // a forward-only structured CFG runs UNCONDITIONALLY — both arms
    // always execute, joins are already select instructions inserted
    // by the frontend at the merge blocks — so flattening is a
    // topological order over the blocks with the branch terminators
    // dropped.  Everything predication cannot yet express refuses
    // loudly here rather than lowering wrong:
    //   - back-edges (loops) — no honest unconditional schedule exists;
    //   - output stores off the exit block — an off-path store would
    //     commit a value the branch was supposed to suppress;
    //   - multiple (or zero) return blocks, unterminated blocks,
    //     branches to unknown blocks, unreachable non-empty blocks —
    //     shapes the frontend does not emit, refused rather than
    //     assumed away.
    // Successor edges are derived from the branch instructions
    // themselves, not from IRBasicBlock's successor vectors: the
    // instruction stream is what gets emitted, so it is what gets
    // trusted.  Returns false with the refusal recorded.
    bool flattenBlockOrder(std::vector<const IRBasicBlock*>& order)
    {
        std::vector<const IRBasicBlock*> blocks;
        for (const auto& b : entry_.blocks)
            if (b) blocks.push_back(b.get());
        if (blocks.size() <= 1) {
            order = blocks;
            return true;
        }
        flattened_ = true;

        auto refuse = [&](const std::string& what) {
            program_.diagnostics.push_back("nv40-general: " + what);
            program_.loweringFailed = true;
            return false;
        };

        std::unordered_map<std::string, const IRBasicBlock*> byName;
        for (const IRBasicBlock* b : blocks)
            byName[b->name] = b;

        std::unordered_map<const IRBasicBlock*,
                           std::vector<const IRBasicBlock*>> succs;
        const IRBasicBlock* exitBlock = nullptr;
        for (const IRBasicBlock* b : blocks) {
            bool terminated = false;
            for (const auto& instPtr : b->instructions) {
                if (!instPtr) continue;
                const IRInstruction& inst = *instPtr;
                if (terminated)
                    return refuse("block '" + b->name +
                                  "' has instructions past its terminator; refusing");
                if (inst.result != InvalidIRValue)
                    defMap_[inst.result] = &inst;
                switch (inst.op) {
                case IROp::Branch:
                case IROp::CondBranch: {
                    terminated = true;
                    // targetName is "then,else" for brc, "target" for br.
                    const std::string& t = inst.targetName;
                    size_t start = 0;
                    while (true) {
                        const size_t comma = t.find(',', start);
                        const std::string name =
                            comma == std::string::npos
                                ? t.substr(start)
                                : t.substr(start, comma - start);
                        auto it = byName.find(name);
                        if (it == byName.end())
                            return refuse("branch to unknown block '" +
                                          name + "'; refusing");
                        succs[b].push_back(it->second);
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                    break;
                }
                case IROp::Return:
                    terminated = true;
                    if (exitBlock)
                        return refuse(
                            "more than one return block; refusing");
                    exitBlock = b;
                    break;
                default:
                    break;
                }
            }
            if (!terminated)
                return refuse("block '" + b->name +
                              "' has no terminator; refusing");
        }
        if (!exitBlock)
            return refuse("control flow has no return block; refusing");

        // Off-exit stores.  In a flattened program every block executes,
        // so a store that the original control flow could SKIP commits a
        // value the branch was meant to suppress.  Refusing all of them
        // was the first slice's bound; a store the flow cannot skip is
        // safe, and that is a reachability question with an answer:
        //
        //   b is on EVERY path from entry to exit
        //     <=>  removing b disconnects the exit from the entry.
        //
        // Every such block executes on every path in the original
        // program too, so the flattened order commits the same value.
        // And when several store blocks each dominate the exit they are
        // totally ordered by dominance - dominators of a node form a
        // chain - and reverse post-order respects dominance, so the LAST
        // store the flattened program runs is the last one the original
        // would have run, on every path.  `o = c; if (..) discard;
        // o = o*d; if (..) discard; o = o+d;` is that shape.
        //
        // Stated as reachability rather than as a dominator computation
        // on purpose: one graph search per store block, on functions of a
        // handful of blocks, and nothing subtle to get wrong.
        // Blocks that KILL the fragment.  A discard's guard is the
        // reachability condition of its own block (the guard pass proves
        // that), so a fragment that took a path through one of these is
        // killed - which is what makes the second rule below sound.
        std::unordered_set<const IRBasicBlock*> discardBlocks;
        for (const IRBasicBlock* b : blocks)
            for (const auto& instPtr : b->instructions)
                if (instPtr && instPtr->op == IROp::Discard)
                    discardBlocks.insert(b);

        const auto reachesExitWithout =
            [&](const IRBasicBlock* removed, bool alsoSkipKills) {
                std::unordered_set<const IRBasicBlock*> seen;
                std::vector<const IRBasicBlock*> work;
                const auto blocked = [&](const IRBasicBlock* b) {
                    return b == removed ||
                           (alsoSkipKills && discardBlocks.count(b) != 0);
                };
                if (!blocked(blocks.front())) {
                    work.push_back(blocks.front());
                    seen.insert(blocks.front());
                }
                while (!work.empty()) {
                    const IRBasicBlock* cur = work.back();
                    work.pop_back();
                    if (cur == exitBlock) return true;
                    for (const IRBasicBlock* nb : succs[cur]) {
                        if (blocked(nb)) continue;
                        if (seen.insert(nb).second) work.push_back(nb);
                    }
                }
                return false;
            };

        for (const IRBasicBlock* b : blocks) {
            if (b == exitBlock) continue;
            bool stores = false;
            for (const auto& instPtr : b->instructions) {
                if (!instPtr) continue;
                if (instPtr->op == IROp::StoreOutput ||
                    instPtr->op == IROp::StoreVarying)
                    stores = true;
            }
            if (!stores) continue;
            if (!reachesExitWithout(b, false))
                continue;   // unskippable: on every path to the exit
            // Skippable, and still safe when every path that MISSES it is
            // killed: such a fragment never reaches the framebuffer, so
            // the value the flattened program commits to it is not
            // observed, and every SURVIVING fragment took the store.
            // `if (a) { o = ..; } else { discard; }` is that shape.
            //
            // It generalises to several stores without extra argument:
            // when every store block for the program is either
            // unskippable or kill-covered, every survivor visited all of
            // them, and reverse post-order restricted to those blocks is
            // the order the survivor visited them in - so the last store
            // the flattened program runs is the last one the original
            // would have run for that fragment.
            if (!reachesExitWithout(b, true))
                continue;
            return refuse(
                "output store in block '" + b->name +
                "' the control flow can skip on a path that is not "
                "killed; refusing");
        }

        // Iterative DFS from the entry block (blocks[0] by contract).
        // Reverse post-order of a DAG is a topological order, and a
        // grey-node revisit is a back-edge, i.e. a loop.  Definitions
        // dominate uses in this IR, and a dominator precedes its
        // dominatee in every topological order, so emission in this
        // order never reads an unresolved value.
        std::unordered_map<const IRBasicBlock*, int> color;  // 0 white, 1 grey, 2 black
        std::vector<const IRBasicBlock*> post;
        struct Frame { const IRBasicBlock* b; size_t next; };
        std::vector<Frame> stack;
        stack.push_back({blocks.front(), 0});
        color[blocks.front()] = 1;
        while (!stack.empty()) {
            const Frame f = stack.back();
            const auto& ss = succs[f.b];
            if (f.next < ss.size()) {
                stack.back().next = f.next + 1;
                const IRBasicBlock* nb = ss[f.next];
                int& c = color[nb];
                if (c == 1)
                    return refuse(
                        "control flow has a back-edge (loop); refusing");
                if (c == 0) {
                    c = 1;
                    stack.push_back({nb, 0});
                }
            } else {
                color[f.b] = 2;
                post.push_back(f.b);
                stack.pop_back();
            }
        }
        for (const IRBasicBlock* b : blocks) {
            if (color[b] == 2) continue;
            for (const auto& instPtr : b->instructions)
                if (instPtr)
                    return refuse("unreachable block '" + b->name +
                                  "' has instructions; refusing");
        }
        order.assign(post.rbegin(), post.rend());
        if (order.back() != exitBlock)
            return refuse(
                "return block is not the control-flow sink; refusing");
        return true;
    }

    // A select arm is provably finite when no execution can make it
    // inf/NaN: a finite literal, a direct varying/attribute read, or a
    // select over provably finite arms.  Everything computed (div,
    // rsq, pow, ...) is not provable and must wait for CF-1b's
    // predicated write.  Conservative by design: a false negative
    // refuses a shader, a false positive silently corrupts its joins.
    bool provablyFinite(IRValueID id, int depth = 0) const
    {
        if (depth > 64)
            return false;
        if (const auto* c =
                dynamic_cast<const IRConstant*>(entry_.getValue(id))) {
            if (std::holds_alternative<float>(c->value))
                return std::isfinite(std::get<float>(c->value));
            if (std::holds_alternative<std::vector<float>>(c->value)) {
                for (float f : std::get<std::vector<float>>(c->value))
                    if (!std::isfinite(f)) return false;
                return true;
            }
            return true;  // bool / integer literals
        }
        const auto it = defMap_.find(id);
        if (it == defMap_.end())
            return false;
        const IRInstruction& def = *it->second;
        switch (def.op) {
        case IROp::LoadVarying:
        case IROp::LoadAttribute:
            return true;
        case IROp::Select:
            return def.operands.size() >= 3 &&
                   provablyFinite(def.operands[1], depth + 1) &&
                   provablyFinite(def.operands[2], depth + 1);
        default:
            return false;
        }
    }

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
            case VOp::Dp2:
            case VOp::Dp3:
                return 2;
            case VOp::Mul:
            case VOp::Mad:
            case VOp::Mov:
            case VOp::Min:
            case VOp::Max:
            case VOp::Frc:
            case VOp::Flr:
            case VOp::Sge:
            case VOp::Slt:
            case VOp::Sgt:
            case VOp::Sle:
            case VOp::Seq:
            case VOp::Sne:
            case VOp::SelPred:
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

        // Dependency edges, per register, over ALL writers and readers.
        //
        // The previous form kept ONE producer per temp register
        // (`tempProducer[reg] = i`) and gave a consumer a RAW edge to that
        // single most-recent write.  A vector built lane by lane is SEVERAL
        // partial-writemask writes to the SAME register - exactly what
        // lowerVecConstruct emits - so a consumer was constrained only by
        // the LAST lane write, and every earlier lane was free to be
        // scheduled BELOW the instruction that reads it.
        //
        // Measured on test_02_add (t_0a4e0ed4): `float3 result = col1 +
        // col2` had its ADD emitted at instruction 11 while col2.y was
        // written at 15 and col1.x at the last instruction, so the sum read
        // stale registers and the shader rendered a near-constant colour.
        // On the corpus this repaired 9 shaders to pixel-identical against
        // the reference and moved none of the 27 that already matched.
        //
        // It also explains WHICH shaders were wrong: an op that writes its
        // whole result in one instruction has a single producer and cannot
        // lose an edge, while anything that computes a SCALAR and places it
        // into a lane (dot products, sin/cos/pow) is a partial write.
        // Vertex outputs live in their own register file; give them a key
        // space that cannot collide with a temp index.
        constexpr int kOutputKeyBase = 1 << 16;
        std::unordered_map<int, std::vector<size_t>> writers, readers;
        const auto link = [&](size_t from, size_t to) {
            // A self-edge is FATAL, not merely redundant: indegree never
            // reaches zero, the node is never ready, and the scheduling
            // loop below can make no progress.  It arises whenever an
            // instruction reads and writes the same register (`ADD R0.x,
            // R0, R1` - most of the sin/cos and dot-product lowerings),
            // because the read is recorded before the WAR walk below.
            if (from == to)
                return;
            auto& deps = consumers[from];
            if (std::find(deps.begin(), deps.end(), to) == deps.end()) {
                deps.push_back(to);
                ++indegree[to];
            }
        };

        for (size_t i = 0; i < n; ++i) {
            const VInstr& vi = program_.instrs[i];
            for (const VSrc& src : vi.srcs) {
                if (src.kind != VSrcKind::Temp)
                    continue;
                for (size_t w : writers[src.index])      // RAW
                    link(w, i);
                readers[src.index].push_back(i);
            }
            if (!vi.dst.none) {
                // OUTPUT destinations belong in this graph too, and used
                // to be excluded from it: two stores to the same output
                // were two writes to one register with no edge between
                // them, and the scheduler was free to commit them in
                // either order.  Measured on fp_discard_two_f
                // (t_becbfa69): `o = c; if (..) discard; o = o*d; if (..)
                // discard; o = o+d;` emitted all three stores and put the
                // FIRST one last, so every surviving pixel read `o = c`.
                //
                // Outputs get a key space DISJOINT from the temps', in
                // both profiles.  It is tempting to share it on the
                // fragment side, where a destination of OUTPUT n occupies
                // TEMP n on the hardware - but this pass runs BEFORE
                // allocatePhysicalTemps, so `dst.index` here is a VIRTUAL
                // register id and virtual 0 has nothing to do with R0.
                // Sharing the key ordered stores against unrelated
                // virtual registers that happened to be numbered 0, and
                // moved six fragment schedules to constrain nothing.  The
                // physical aliasing is the allocator's question, not the
                // scheduler's.
                const int key = vi.dst.output
                    ? (kOutputKeyBase + vi.dst.index)
                    : vi.dst.index;
                for (size_t w : writers[key]) {
                    // WAW.  For an OUTPUT destination the edge is added
                    // only when the two masks share a lane: a program
                    // assembles an output lane by lane (`oColor.xyz =
                    // ...; oColor.w = 1;`) and those writes commute, so
                    // ordering them would move thirty vertex schedules to
                    // constrain nothing.  Temps keep the unconditional
                    // edge they have always had - tightening that rule is
                    // its own change with its own fence.
                    if (vi.dst.output &&
                        (program_.instrs[w].dst.writemask &
                         vi.dst.writemask) == 0)
                        continue;
                    link(w, i);
                }
                for (size_t r : readers[key])   // WAR
                    link(r, i);
                writers[key].push_back(i);
            }
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
                    // Nothing is ready and no future cycle can change that:
                    // the graph cannot be drained.  This used to spin
                    // forever, which turned a dependency-graph defect into
                    // a HANG - the rig's staging timeout then presented it
                    // as "those shaders are missing from the log", which is
                    // the worst possible way to meet a compiler bug.
                    // Refuse loudly instead; a graph that cannot be
                    // scheduled is a defect in this pass either way.
                    // Unconditional: this sits inside
                    // `while (ordered.size() < n)`, so there is always at
                    // least one instruction left to blame.  An `if` here
                    // would send the next reader hunting for the case that
                    // falls through to a spin, and there isn't one.
                    program_.diagnostics.push_back(
                        "nv40-general: instruction scheduler could not drain its "
                        "dependency graph (" + std::to_string(n - ordered.size()) +
                        " of " + std::to_string(n) +
                        " instructions unschedulable); refusing");
                    program_.loweringFailed = true;
                    return;
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
                const bool terminator =
                    instPtr->op == IROp::Branch ||
                    instPtr->op == IROp::CondBranch;
                if (instPtr->result != InvalidIRValue)
                    definedValues_.insert(instPtr->result);
                for (IRValueID id : instPtr->operands) {
                    ++useCount_[id];
                    // A flattened program drops the branch terminators,
                    // so a condition read only by its own brc and by a
                    // discard has ONE real consumer.  Counting the brc
                    // would refuse the fusion the reference performs on
                    // every simple `if (a < b) discard;`.
                    if (!terminator)
                        ++nonTermUseCount_[id];
                }
            }
        }
    }

    void seedParameters()
    {
        int nextVpMatrixConst = 256;
        int nextVpUniformConst = 467;
        // Entry-parameter samplers take the low texture units and file-scope
        // sampler globals continue from there, which is exactly how
        // cg_container_fp.cpp numbers them; the two must agree or binding a
        // texture by name reaches a different unit than the ucode samples.
        int nextFpTexUnit = 0;
        unsigned nextFpGlobalSlot =
            static_cast<unsigned>(entry_.parameters.size());
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
            // Parameters have no IRValue in the entry table; without
            // this their widths are unobservable downstream and every
            // reader used to silently assume Float4.
            if (!p.type.isMatrix())
                valueWidth_[p.valueId] = p.type.componentCount();
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
                noteTexcoordWidth(idx, p.type.componentCount());
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
                       isSamplerIRType(p.type.baseType)) {
                samplerUnit_[p.valueId] = nextFpTexUnit++;
            } else if (profile_ == GeneralProfile::Fragment &&
                       p.storage == StorageQualifier::Uniform) {
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
            } else if (profile_ == GeneralProfile::Fragment &&
                       isSamplerIRType(g.type.baseType)) {
                samplerUnit_[g.valueId] = nextFpTexUnit++;
            } else if (profile_ == GeneralProfile::Fragment) {
                // File-scope uniforms are numbered after every entry
                // parameter, in declaration order - the numbering
                // cg_container_fp.cpp reverses when it walks
                // module.globals.  This branch did not exist, so an FP
                // file-scope uniform had no source at all and the shader
                // refused (or, before the id spaces were separated, read
                // whatever function value shared its id).
                const unsigned slot = nextFpGlobalSlot++;
                program_.valueToSource[g.valueId] = uniformSrc(
                    static_cast<int>(slot), true);
                program_.fpGlobalUniformSlots.push_back(slot);
                if (!g.initialValue.empty())
                    program_.fpUniformDefaults[slot] = {
                        g.initialValue,
                        static_cast<unsigned>(g.type.componentCount())};
            }
        }
        for (auto it = pendingMatrices.begin(); it != pendingMatrices.end(); ++it) {
            matrixUniformBase_[it->valueId] = nextVpMatrixConst;
            matrixUniformRows_[it->valueId] = std::max(1, it->rows);
            if (dumpOrder) {
                std::fprintf(stderr, "matrix %s -> c[%d]\n",
                             it->name.c_str(), nextVpMatrixConst);
            }
            nextVpMatrixConst += it->rows;
        }
        program_.nextVpLiteralConst = nextVpUniformConst;
        program_.vpConstFloor = nextVpMatrixConst;
    }

    // A SCALAR operand must REPLICATE its own component across all four
    // lanes.  The property belongs to the VALUE, not to the instruction
    // reading it or to the kind of source it came from, so it is applied
    // here - the one place every operand is resolved - rather than at each
    // emit site.  Fixing emit sites one at a time leaves the same bug
    // waiting in the next opcode; the vita-cg-compiler team needed a
    // scalar-value set consulted at 13 read sites for exactly this reason,
    // and this lowering is lucky enough to have a single choke point.
    //
    // Each source kind fails differently without it, which is what the
    // three channels of sd_scalar_broadcast.fcg measured on hardware:
    //   LITERAL   {c,0,0,0} under identity -> lanes past x read ZERO
    //   UNIFORM   the slot holds four real values -> lanes past x read the
    //             WRONG COMPONENT (measured: `uv * u_scale.x` read
    //             u_scale.y on lane y, a ratio of 1.58 against the
    //             reference rather than a zero)
    //   TEMP      a scalar lives in .x only -> lanes past x read a lane
    //             that was never written (measured: pinned at 0)
    //
    // REPLICATE swizzle[0], do not force lane 0: a width-1 value can be a
    // LANE EXTRACT whose component is already selected (`uv.y` carries
    // swizzle[0] == 1), and forcing {0,0,0,0} there would silently read the
    // wrong lane.  For a width-1 value only swizzle[0] is meaningful, so
    // replicating it is always the identity-preserving choice.
    static void broadcastScalar(VSrc& src)
    {
        if (src.kind == VSrcKind::None)
            return;
        const uint8_t c = src.swizzle[0];
        src.swizzle = {c, c, c, c};
    }

    VSrc resolve(IRValueID id)
    {
        const bool isScalar = (valueWidthOf(id) == 1);
        const auto srcIt = program_.valueToSource.find(id);
        if (srcIt != program_.valueToSource.end()) {
            VSrc src = srcIt->second;
            if (isScalar) broadcastScalar(src);
            return src;
        }
        const auto regIt = program_.valueToVReg.find(id);
        if (regIt != program_.valueToVReg.end()) {
            VSrc src = tempSrc(regIt->second);
            const auto fp16It = program_.vregToFp16.find(regIt->second);
            src.fp16 = fp16It != program_.vregToFp16.end() && fp16It->second;
            if (isScalar) broadcastScalar(src);
            return src;
        }

        const IRValue* value = entry_.getValue(id);
        if (auto* constant = dynamic_cast<const IRConstant*>(value)) {
            VSrc src = literalSrc(*constant);
            if (isScalar) broadcastScalar(src);
            return src;
        }

        // REFUSE, do not paper over it.  Returning an empty source here used
        // to encode as SRC_REG_TYPE_INPUT with no index - i.e. a read of
        // vertex attribute 0 - so a lowering that quietly dropped a value
        // produced a shader that computed against the wrong register with no
        // diagnostic and no input-mask bit.  23 of 27 vertex shaders were
        // miscompiled that way.  Name the value and what produced it so the
        // next dropped lowering hands its author the culprit.
        std::string what = "nv40-general: operand %" + std::to_string(id) +
                           " could not be resolved";
        if (value)
            what += " (defined by " + value->toString() + ")";
        what += "; refusing rather than emitting an empty source";
        program_.diagnostics.push_back(what);
        program_.loweringFailed = true;
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
        // The entry's value table only holds constants and named
        // values - getValue() returns null for ordinary instruction
        // results, so their widths were unobservable downstream and
        // every reader silently assumed Float4 (measured: the
        // conservative vec-construct guard exists because of it).
        // Every instruction knows its own result type right here;
        // record it once, centrally, before dispatch.
        if (inst.result != InvalidIRValue && !inst.resultType.isMatrix())
            valueWidth_[inst.result] = inst.resultType.componentCount();
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
        case IROp::LoadUniform:
            lowerLoadUniform(inst);
            return;
        case IROp::MatVecMul:
            lowerMatVecMul(inst);
            return;
        case IROp::VecMatMul:
            lowerVecMatMul(inst);
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
            // The width that picks DP4 over DP3 is the OPERANDS', not the
            // result's.  A dot product's result is a SCALAR, always, so
            // `resultType.componentCount() >= 4` was never true and every
            // dot lowered to DP3 - a 4D dot silently dropping its w term.
            // test_42_dot4 was the only shader in the corpus with one, and
            // it was the last general-path mismatch in the set
            // (t_856689b2).
            {
                // ... and a 2-wide dot is DP2, not DP3 over a register
                // whose third lane nothing wrote (t_a30159bf): the
                // operands are written with a two-lane mask, so DP3
                // reads x*x + y*y + whatever the allocator left in z.
                // The reference emits DP2 here.  Width 1 keeps its old
                // spelling until someone measures the reference on a
                // scalar dot; guessing it into this commit would be the
                // same mistake in the other direction.
                const int w =
                    std::max(valueWidthOf(inst.operands[0]),
                             inst.operands.size() > 1
                                 ? valueWidthOf(inst.operands[1]) : 0);
                // FRAGMENT only: NV40's vertex unit has no DP2 opcode,
                // and vpOpcode's fallthrough is MOV - a silent wrong
                // answer.  The vertex side of the same stale-lane
                // question is its own item, unmeasured here.
                const bool dp2 = (w == 2 &&
                                  profile_ == GeneralProfile::Fragment);
                lowerBinary(inst, w >= 4 ? VOp::Dp4
                                : dp2    ? VOp::Dp2
                                         : VOp::Dp3);
            }
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
        case IROp::Refract:
            lowerRefract(inst);
            return;
        case IROp::CmpLe:
            lowerCmpLE(inst);
            return;
        case IROp::CmpLt:
            lowerBinary(inst, VOp::Slt);
            return;
        case IROp::CmpGt:
            lowerBinary(inst, VOp::Sgt);
            return;
        case IROp::CmpGe:
            lowerBinary(inst, VOp::Sge);
            return;
        case IROp::CmpEq:
            lowerBinary(inst, VOp::Seq);
            return;
        case IROp::CmpNe:
            lowerBinary(inst, VOp::Sne);
            return;
        case IROp::Neg:
            lowerMovWithModifier(inst, true, false);
            return;
        // Logical ops act on the 0/1 booleans the comparison
        // lowerings produce (the frontend types them bool/bvec):
        // AND is multiplication, OR is max, NOT is 1-x.  All
        // component-wise, so bvec operands work unchanged.
        case IROp::LogicalAnd:
            lowerBinary(inst, VOp::Mul);
            return;
        case IROp::LogicalOr:
            lowerBinary(inst, VOp::Max);
            return;
        case IROp::LogicalNot:
            lowerLogicalNot(inst);
            return;
        case IROp::Abs:
            lowerMovWithModifier(inst, false, true);
            return;
        case IROp::Frac:
            lowerUnary(inst, VOp::Frc, false);
            return;
        case IROp::Floor:
            lowerUnary(inst, VOp::Flr, false);
            return;
        case IROp::Ceil:
            lowerCeil(inst);
            return;
        case IROp::Round:
            lowerRound(inst);
            return;
        case IROp::Sign:
            lowerSign(inst);
            return;
        case IROp::Trunc:
            lowerTrunc(inst);
            return;
        case IROp::Sqrt:
            lowerSqrt(inst);
            return;
        case IROp::Mod:
            lowerMod(inst);
            return;
        case IROp::FloatToHalf:
            lowerPrecisionCast(inst, true);
            return;
        case IROp::HalfToFloat:
            lowerPrecisionCast(inst, false);
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
        case IROp::Discard:
            lowerDiscard(inst);
            return;
        case IROp::Return:
        case IROp::Comment:
        case IROp::Nop:
            return;
        default:
            if (std::string msg = profileUnsupportedDiagnostic(inst);
                !msg.empty())
            {
                program_.diagnostics.push_back(msg);
                program_.loweringFailed = true;
                return;
            }
            // An op we do not implement is a REFUSAL, not a note.  This used
            // to push a diagnostic and return while the caller emitted the
            // remaining instructions and exited 0, so a shader using discard,
            // sqrt, floor or a comparison silently lost it.  Three shaders
            // still exited 0 this way after resolve() began refusing, because
            // an unimplemented op whose result nothing reads leaves nothing
            // unresolved downstream - discard has no result at all.
            program_.diagnostics.push_back(
                std::string("nv40-general: unsupported IR op ") +
                irOpToString(inst.op));
            program_.loweringFailed = true;
            return;
        }
    }

    // Record a TEXCOORD's DECLARED width, keyed by the input-source code
    // fragmentInputSrc already resolved, so the spellings this accepts
    // cannot drift from the ones that populate the read mask (TEX0 is
    // TEXCOORD0).
    void noteTexcoordWidth(int inputSrcCode, int components)
    {
        if (inputSrcCode < NVFX_FP_OP_INPUT_SRC_TC(0) ||
            inputSrcCode > NVFX_FP_OP_INPUT_SRC_TC(9))
            return;
        int& w = program_.texcoordDeclaredWidth[
            inputSrcCode - NVFX_FP_OP_INPUT_SRC_TC(0)];
        w = std::max(w, components);
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
        if (profile_ == GeneralProfile::Fragment)
            noteTexcoordWidth(idx, inst.resultType.componentCount());
        // Inputs have no IRValue in the entry table, so their width is
        // otherwise unobservable downstream (the old default silently
        // read as Float4).  Record the declared width here; every width
        // consumer checks this map before falling back.
        valueWidth_[inst.result] = inst.resultType.componentCount();
    }

    void lowerVecShuffle(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        VSrc src = resolve(inst.operands[0]);
        assignSwizzle(src, inst.swizzleMask, inst.resultType.componentCount());
        program_.valueToSource[inst.result] = src;
    }

    // The integer value of a constant operand, for an index rather than a
    // number: an array or matrix subscript arrives as int32 here.
    bool constantIndex(IRValueID id, int& out) const
    {
        const IRValue* value = entry_.getValue(id);
        auto* constant = dynamic_cast<const IRConstant*>(value);
        if (!constant) return false;
        if (std::holds_alternative<int32_t>(constant->value))
            out = std::get<int32_t>(constant->value);
        else if (std::holds_alternative<uint32_t>(constant->value))
            out = static_cast<int>(std::get<uint32_t>(constant->value));
        else if (std::holds_alternative<float>(constant->value)) {
            const float f = std::get<float>(constant->value);
            if (static_cast<float>(static_cast<int>(f)) != f) return false;
            out = static_cast<int>(f);
        } else {
            return false;
        }
        return true;
    }

    void lowerVecExtract(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;

        // `m[r]` on a matrix uniform is the const register of row r, whole
        // - not a lane of something.  Without this the operand resolved to
        // nothing and the program refused to lower (t_9da20b33).  The row
        // is OPERAND 1: the IR for m[0] and m[2] differs only there, and
        // both carry componentIndex 0, so reading the field would compile
        // every row as row 0.
        if (inst.operands.size() >= 2 && !inst.resultType.isMatrix()) {
            const auto matIt = matrixUniformBase_.find(inst.operands[0]);
            int row = 0;
            const auto rowsIt = matrixUniformRows_.find(inst.operands[0]);
            const int rows =
                rowsIt == matrixUniformRows_.end() ? 0 : rowsIt->second;
            if (matIt != matrixUniformBase_.end() &&
                constantIndex(inst.operands[1], row) &&
                row >= 0 && row < rows) {
                program_.valueToSource[inst.result] =
                    uniformSrc(matIt->second + row, false);
                return;
            }
        }

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
            // The base is a value that lives in a SOURCE register rather
            // than a temp: a varying passed straight through with one lane
            // overridden, `float4 c = color; c.x = 0.5;`.  Returning here
            // left the insert's result undefined and the consuming store
            // refused with "operand could not be resolved" - the general
            // path's half of t_afb4af65, filed as t_be578e74.
            //
            // Materialise the base into a temp, masked to the lanes the
            // insert does NOT write, then write the lane.  That is the
            // reference's shape for the same source (`MOV R0.yzw, f[TEX0]`
            // then `MOV R0.x, {0.5,0,0,0}.x`), and it avoids copying a lane
            // that is about to be overwritten.
            // NOTHING TO COPY.  Two bases carry no value to preserve: an
            // OUT parameter, and an UNINITIALISED declaration - `half4 c;
            // c.xyz = ...; c.a = ...;` builds its whole value by inserts,
            // and the base %n has no producer anywhere in the function.
            // Both want the same emission: define the result and write
            // the lane, copying nothing.  th06_add is the second
            // (t_b8bb521f); the reference agrees, never materialising `c`
            // at all and writing R0.w and R0.xyz from the two chains.
            //
            // "No producer" is checked against every instruction's result,
            // NOT against "the lowering has not resolved it yet": a base
            // whose own lowering failed must still fall through to the
            // refusal below, or this turns a dropped computation into a
            // partial value.
            const bool nothingToCopy =
                isOutputParam(inst.operands[0]) ||
                (!program_.valueToSource.count(inst.operands[0]) &&
                 !definedValues_.count(inst.operands[0]));
            if (!nothingToCopy) {
                if (!program_.valueToSource.count(inst.operands[0]))
                    return;
                const int baseReg = define(inst.result);
                program_.valueToVReg[inst.result] = baseReg;

                VInstr base;
                base.op = VOp::Mov;
                base.dst.index = baseReg;
                base.dst.writemask = 0xf & ~laneMask;
                base.srcs[0] = resolve(inst.operands[0]);
                program_.instrs.push_back(base);

                VInstr lane_;
                lane_.op = VOp::Mov;
                lane_.dst.index = baseReg;
                lane_.dst.writemask = laneMask;
                lane_.srcs[0] = resolve(inst.operands[1]);
                // NOT forced to {0,0,0,0}: resolve() has already
                // replicated swizzle[0] for a width-1 value, which for a
                // LANE EXTRACT is the lane it selected.  Forcing lane 0
                // here made `color.y = lit.y` emit `MOV R0.y, R16.x` -
                // the red channel broadcast into green and blue
                // (t_856689b2's remaining four).  broadcastScalar's own
                // comment warns against exactly this.
                program_.instrs.push_back(lane_);
                return;
            }

            const int resultReg = define(inst.result);
            program_.valueToVReg[inst.result] = resultReg;

            VInstr vi;
            vi.op = VOp::Mov;
            vi.dst.index = resultReg;
            vi.dst.writemask = laneMask;
            vi.srcs[0] = resolve(inst.operands[1]);
            // NOT forced to {0,0,0,0}: resolve() has already
            // replicated swizzle[0] for a width-1 value, which for a
            // LANE EXTRACT is the lane it selected.  Forcing lane 0
            // here made `color.y = lit.y` emit `MOV R0.y, R16.x` -
            // the red channel broadcast into green and blue
            // (t_856689b2's remaining four).  broadcastScalar's own
            // comment warns against exactly this.
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
        // NOT forced to {0,0,0,0}: resolve() has already
        // replicated swizzle[0] for a width-1 value, which for a
        // LANE EXTRACT is the lane it selected.  Forcing lane 0
        // here made `color.y = lit.y` emit `MOV R0.y, R16.x` -
        // the red channel broadcast into green and blue
        // (t_856689b2's remaining four).  broadcastScalar's own
        // comment warns against exactly this.
        program_.instrs.push_back(vi);
    }

    void lowerVecConstruct(const IRInstruction& inst)
    {
        if (tryFoldPowDotVecConstruct(inst))
            return;

        if (inst.result == InvalidIRValue || inst.operands.empty() ||
            inst.operands.size() > 4)
            return;

        const int resultWidth = inst.resultType.componentCount();

        // Fast path, preserved from the original lowering: float4(vec3
        // already in a vreg, scalar) aliases the base register and writes
        // only .w - one instruction, at the cost of mutating the base
        // vreg in place (pre-existing behavior; safe while the IR never
        // reads the vec3 after widening it, which is the shape's only
        // known use).  The scalar-partner check is PROOF the shape fits:
        // an unresolved value's component mask defaults to Float4, so a
        // wide reading proves nothing, but a scalar reading proves a
        // narrow partner.  Everything else takes the general packer
        // below, which is where float4(float2, float2) - the silent
        // wrong-lane miscompile found in review - now emits correctly
        // instead of refusing.
        if (inst.operands.size() == 2 && resultWidth == 4) {
            const int tailMask = valueComponentMask(inst.operands[1]);
            const bool tailIsScalar =
                tailMask != 0 && (tailMask & (tailMask - 1)) == 0;
            const auto baseIt = program_.valueToVReg.find(inst.operands[0]);
            if (tailIsScalar && baseIt != program_.valueToVReg.end()) {
                program_.valueToVReg[inst.result] = baseIt->second;
                VInstr vi;
                vi.op = VOp::Mov;
                vi.dst.index = baseIt->second;
                vi.dst.writemask = 0x8;
                vi.srcs[0] = resolve(inst.operands[1]);
                // NOT forced to {0,0,0,0}: resolve() has already
                // replicated swizzle[0] for a width-1 value, which for a
                // LANE EXTRACT is the lane it selected.  Forcing lane 0
                // here made `color.y = lit.y` emit `MOV R0.y, R16.x` -
                // the red channel broadcast into green and blue
                // (t_856689b2's remaining four).  broadcastScalar's own
                // comment warns against exactly this.
                program_.instrs.push_back(vi);
                return;
            }
        }

        // General packer: one MOV per operand into contiguous lanes.
        // Requires every operand's width to be KNOWN from the IR value
        // table and the widths to sum to the result width; anything less
        // refuses loudly - never a plausible container with wrong lanes.
        int widths[4] = {0, 0, 0, 0};
        int total = 0;
        bool known = true;
        for (size_t i = 0; i < inst.operands.size(); i++) {
            const int w = valueWidthOf(inst.operands[i]);
            if (w < 1 || w > 4) {
                known = false;
                break;
            }
            widths[i] = w;
            total += w;
        }
        if (!known || total != resultWidth) {
            std::string what = "nv40-general: vec construction with operand widths (";
            for (size_t i = 0; i < inst.operands.size(); i++) {
                if (i) what += ",";
                const int w = valueWidthOf(inst.operands[i]);
                what += w ? std::to_string(w) : std::string("?");
            }
            what += ") for a " + std::to_string(resultWidth) +
                    "-wide result; refusing rather than packing the wrong lanes";
            program_.diagnostics.push_back(what);
            program_.loweringFailed = true;
            return;
        }

        const int dstReg = define(inst.result);
        int off = 0;
        for (size_t i = 0; i < inst.operands.size(); i++) {
            const int w = widths[i];
            VInstr vi;
            vi.op = VOp::Mov;
            vi.dst.index = dstReg;
            vi.dst.writemask = ((1 << w) - 1) << off;
            vi.srcs[0] = resolve(inst.operands[i]);
            // Compose swizzles: dest lane off+j reads the operand's j-th
            // logical component, which sits at the source's swizzle[j].
            const std::array<uint8_t, 4> orig = vi.srcs[0].swizzle;
            std::array<uint8_t, 4> sw = {orig[0], orig[0], orig[0], orig[0]};
            for (int j = 0; j < w; j++)
                sw[off + j] = orig[j];
            vi.srcs[0].swizzle = sw;
            program_.instrs.push_back(vi);
            off += w;
        }
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

    // float -> half and half -> float.  NV40 has no conversion opcode:
    // the conversion IS the register file the value lands in, so both are
    // a MOV whose destination is an H or an R register, and the precision
    // of every later instruction follows from its operands.  MEASURED on
    // the reference compiling th06_mod, which never emits a conversion at
    // all - it writes `MOVH H0, f[TEX2]` and then reads H0.w in a MADH,
    // so the ftoh in the IR is absorbed by the register the value was
    // already loaded into.
    //
    // Ours emits the MOV.  That is one instruction more than the
    // reference wherever the source is already in the right file, and it
    // is the honest first slice: the fold is a peephole over this, not a
    // different lowering (t_b8bb521f).
    void lowerPrecisionCast(const IRInstruction& inst, bool toHalf)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment) {
            program_.diagnostics.push_back(
                "nv40-general: half precision is fragment-only");
            program_.loweringFailed = true;
            return;
        }
        VInstr vi;
        vi.op = VOp::Mov;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.dst.fp16 = toHalf;
        program_.vregToFp16[vi.dst.index] = toHalf;
        vi.srcs[0] = resolve(inst.operands[0]);
        program_.instrs.push_back(vi);
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

    // abs()/neg() are SOURCE MODIFIERS on NV40, not instructions: emit
    // a MOV whose source carries the modifier.  One instruction today;
    // the optimization-level work can later fold the modifier into the
    // consumer.  Order note: the hardware applies negate AFTER abs when
    // both are set, so Abs must CLEAR an inherited negate
    // (abs(-x) == abs(x)) while Neg toggles it.
    void lowerMovWithModifier(const IRInstruction& inst, bool neg, bool abs)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        VInstr vi;
        vi.op = VOp::Mov;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = resolve(inst.operands[0]);
        if (abs) {
            vi.srcs[0].abs = true;
            vi.srcs[0].neg = false;
        }
        if (neg)
            vi.srcs[0].neg = !vi.srcs[0].neg;
        program_.instrs.push_back(vi);
    }

    static VSrc floatLit(float v)
    {
        VSrc s;
        s.kind = VSrcKind::Literal;
        s.literal = {v, v, v, v};
        s.swizzle = {0, 0, 0, 0};
        return s;
    }

    // CF-2 (t_91bbd575): a fragment kill.
    //
    // The guard arrives as an operand from materialiseDiscardGuards -
    // the conjunction of the branch conditions on the path that reaches
    // this discard - and `guardIsNegated` says the kill fires where that
    // guard is FALSE.  Everything the reference does here was measured
    // before it was written; the shapes are in the CF-2 note.
    void lowerDiscard(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment) {
            program_.diagnostics.push_back(
                "nv40-general: discard is fragment-only");
            program_.loweringFailed = true;
            return;
        }

        VInstr kil;
        kil.op = VOp::Kil;
        kil.dst.none = true;
        kil.dst.writemask = 0x1;
        kil.killTestEq = inst.guardIsNegated;

        if (inst.operands.empty()) {
            // Unconditional.  The reference does NOT encode an
            // always-kill: it MATERIALISES a true condition (a MOV of
            // 1.0 into the condition register with cc_update) and then
            // uses the same KIL.  There is exactly one KIL spelling, so
            // there is exactly one here too.
            kil.killFused = VOp::Mov;
            kil.srcs[0] = floatLit(1.0f);
            kil.fpPrecisionOverride = NVFX_FP_PRECISION_FX12;
            program_.instrs.push_back(kil);
            return;
        }

        const IRValueID guard = inst.operands[0];
        const auto vregIt = program_.valueToVReg.find(guard);
        const auto useIt = nonTermUseCount_.find(guard);
        const unsigned uses =
            useIt == nonTermUseCount_.end() ? 0u : useIt->second;
        const bool soleConsumer = (uses <= 1);

        // Fuse the guard's producer into the kill when it is the last
        // instruction emitted and nothing else reads its result: the
        // reference retargets that instruction to the condition register
        // and never writes the general register at all.
        bool fused = false;
        if (!program_.instrs.empty() && vregIt != program_.valueToVReg.end()) {
            const VInstr& last = program_.instrs.back();
            const bool fusable =
                !last.dst.none && !last.dst.output &&
                last.dst.index == vregIt->second &&
                last.op != VOp::SelPred && last.op != VOp::Kil &&
                last.op != VOp::Tex && last.fpScale == 0 &&
                !last.stubFenceBefore && !last.stubFenceBrBefore;
            if (fusable && soleConsumer) {
                kil.killFused = last.op;
                kil.srcs = last.srcs;
                kil.sat = last.sat;
                kil.fpPrecisionOverride =
                    fusedPrecision(guard, last.op, kil.killTestEq, true);
                program_.instrs.pop_back();
                fused = true;
            }
        }

        if (!fused) {
            // Not the last instruction, or read by something else: keep
            // the guard where it is and move a copy of it into the
            // condition register.  One instruction longer than the
            // reference, which reuses the CC in that case - measured,
            // and deliberately out of CF-2's first slice.
            VSrc src = resolve(guard);
            if (src.kind == VSrcKind::None) {
                program_.diagnostics.push_back(
                    "nv40-general: discard guard is unresolved");
                program_.loweringFailed = true;
                return;
            }
            kil.killFused = VOp::Mov;
            kil.srcs[0] = src;
            kil.fpPrecisionOverride = NVFX_FP_PRECISION_FX12;
        }

        program_.instrs.push_back(kil);
    }

    // Precision of the instruction fused into a kill, as MEASURED - not
    // as derived, because nobody in this room has a mechanism for the
    // SGE case and a tidy rule would be a rule the reference does not
    // follow.  The reference:
    //
    //   - demotes a LONE comparison feeding a kill to fx12, when the
    //     test is NE and the condition register has no other consumer;
    //   - does NOT demote SGE, ever, in that position;
    //   - does NOT demote a NEGATED lone comparison (test EQ);
    //   - DOES keep fx12 for a `&&` / `||` chain link whatever the test,
    //     because those combiners are fx12 in their own right;
    //   - keeps fp32 whenever the condition register drives something
    //     else as well (a predicated write after the kill).
    //
    // Everything else keeps the default.  In particular an arbitrary
    // arithmetic guard is NOT demoted: fx12 is s1.10 and would clamp a
    // value the shader may legitimately compute outside [-2, 2).
    int fusedPrecision(IRValueID guard, VOp fusedOp, bool testEq,
                       bool soleConsumer) const
    {
        const auto defIt = defMap_.find(guard);
        const IROp defOp =
            defIt == defMap_.end() ? IROp::Nop : defIt->second->op;

        const bool combiner =
            (defOp == IROp::LogicalAnd || defOp == IROp::LogicalOr) &&
            (fusedOp == VOp::Mul || fusedOp == VOp::Max ||
             fusedOp == VOp::Add);
        if (combiner)
            return NVFX_FP_PRECISION_FX12;

        const bool comparison =
            fusedOp == VOp::Slt || fusedOp == VOp::Sgt ||
            fusedOp == VOp::Sle || fusedOp == VOp::Seq ||
            fusedOp == VOp::Sne;
        if (comparison && !testEq && soleConsumer)
            return NVFX_FP_PRECISION_FX12;
        return -1;
    }

    void lowerLogicalNot(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        VInstr vi;
        vi.op = VOp::Add;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = floatLit(1.0f);
        vi.srcs[1] = resolve(inst.operands[0]);
        vi.srcs[1].neg = !vi.srcs[1].neg;
        program_.instrs.push_back(vi);
    }

    // ceil(x) = -floor(-x): FLR into a temp with the source negated,
    // then a negated MOV into the result.
    void lowerCeil(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        const int mask = componentMask(inst.resultType);
        const int t = newVReg();
        VInstr flr;
        flr.op = VOp::Flr;
        flr.dst.index = t;
        flr.dst.writemask = mask;
        flr.srcs[0] = resolve(inst.operands[0]);
        flr.srcs[0].neg = !flr.srcs[0].neg;
        program_.instrs.push_back(flr);

        VInstr mov;
        mov.op = VOp::Mov;
        mov.dst.index = define(inst.result);
        mov.dst.writemask = mask;
        mov.srcs[0] = tempSrc(t);
        mov.srcs[0].neg = true;
        program_.instrs.push_back(mov);
    }

    // round(x) = floor(x + 0.5), the Cg stdlib expansion.
    void lowerRound(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        const int mask = componentMask(inst.resultType);
        const int t = newVReg();
        VInstr add;
        add.op = VOp::Add;
        add.dst.index = t;
        add.dst.writemask = mask;
        add.srcs[0] = resolve(inst.operands[0]);
        add.srcs[1] = floatLit(0.5f);
        program_.instrs.push_back(add);

        VInstr flr;
        flr.op = VOp::Flr;
        flr.dst.index = define(inst.result);
        flr.dst.writemask = mask;
        flr.srcs[0] = tempSrc(t);
        program_.instrs.push_back(flr);
    }

    // sign(x) = (x > 0) - (x < 0): 1/0/-1 with sign(0) == 0.
    void lowerSign(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        const int mask = componentMask(inst.resultType);
        const int gt = newVReg();
        const int lt = newVReg();

        VInstr a;
        a.op = VOp::Sgt;
        a.dst.index = gt;
        a.dst.writemask = mask;
        a.srcs[0] = resolve(inst.operands[0]);
        a.srcs[1] = floatLit(0.0f);
        program_.instrs.push_back(a);

        VInstr b;
        b.op = VOp::Slt;
        b.dst.index = lt;
        b.dst.writemask = mask;
        b.srcs[0] = resolve(inst.operands[0]);
        b.srcs[1] = floatLit(0.0f);
        program_.instrs.push_back(b);

        VInstr sub;
        sub.op = VOp::Add;
        sub.dst.index = define(inst.result);
        sub.dst.writemask = mask;
        sub.srcs[0] = tempSrc(gt);
        sub.srcs[1] = tempSrc(lt);
        sub.srcs[1].neg = true;
        program_.instrs.push_back(sub);
    }

    // trunc(x) = floor(|x|) * sign(x).
    void lowerTrunc(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        const int mask = componentMask(inst.resultType);
        const int mag = newVReg();
        VInstr flr;
        flr.op = VOp::Flr;
        flr.dst.index = mag;
        flr.dst.writemask = mask;
        flr.srcs[0] = resolve(inst.operands[0]);
        flr.srcs[0].abs = true;
        flr.srcs[0].neg = false;
        program_.instrs.push_back(flr);

        const int gt = newVReg();
        const int lt = newVReg();
        VInstr a;
        a.op = VOp::Sgt;
        a.dst.index = gt;
        a.dst.writemask = mask;
        a.srcs[0] = resolve(inst.operands[0]);
        a.srcs[1] = floatLit(0.0f);
        program_.instrs.push_back(a);

        VInstr b;
        b.op = VOp::Slt;
        b.dst.index = lt;
        b.dst.writemask = mask;
        b.srcs[0] = resolve(inst.operands[0]);
        b.srcs[1] = floatLit(0.0f);
        program_.instrs.push_back(b);

        VInstr sgn;
        sgn.op = VOp::Add;
        sgn.dst.index = gt;
        sgn.dst.writemask = mask;
        sgn.srcs[0] = tempSrc(gt);
        sgn.srcs[1] = tempSrc(lt);
        sgn.srcs[1].neg = true;
        program_.instrs.push_back(sgn);

        VInstr mul;
        mul.op = VOp::Mul;
        mul.dst.index = define(inst.result);
        mul.dst.writemask = mask;
        mul.srcs[0] = tempSrc(mag);
        mul.srcs[1] = tempSrc(gt);
        program_.instrs.push_back(mul);
    }

    // sqrt(x) = rcp(rsq(x)): two native scalar ops, and the composition
    // gets sqrt(0) right (rsq(0)=+inf, rcp(+inf)=0) where x*rsq(x)
    // would produce 0*inf=NaN.  Fragment only: the VP scalar unit path
    // is still deferred (same guard as rcp/rsq/sin/cos).
    void lowerSqrt(const IRInstruction& inst)
    {
        if (inst.operands.empty() || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment) {
            program_.diagnostics.push_back(
                "nv40-general: VP scalar intrinsic lowering deferred");
            return;
        }
        const int mask = componentMask(inst.resultType);
        const int t = newVReg();
        VInstr rsq;
        rsq.op = VOp::Rsq;
        rsq.dst.index = t;
        rsq.dst.writemask = mask;
        rsq.srcs[0] = resolve(inst.operands[0]);
        program_.instrs.push_back(rsq);

        VInstr rcp;
        rcp.op = VOp::Rcp;
        rcp.dst.index = define(inst.result);
        rcp.dst.writemask = mask;
        rcp.srcs[0] = tempSrc(t);
        program_.instrs.push_back(rcp);
    }

    // mod(x, y) = x - y * floor(x / y), scalar divisor only for now:
    // RCP is a scalar op, so a vector divisor needs per-lane RCPs that
    // belong to a later slice.  Fragment only for the same reason.
    void lowerMod(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment ||
            valueWidthOf(inst.operands[1]) != 1) {
            program_.diagnostics.push_back(
                "nv40-general: mod lowering needs a fragment profile and a "
                "scalar divisor; refusing");
            program_.loweringFailed = true;
            return;
        }
        const int mask = componentMask(inst.resultType);
        const int r = newVReg();
        VInstr rcp;
        rcp.op = VOp::Rcp;
        rcp.dst.index = r;
        rcp.dst.writemask = 0x1;
        rcp.srcs[0] = resolve(inst.operands[1]);
        program_.instrs.push_back(rcp);

        const int q = newVReg();
        VInstr mul;
        mul.op = VOp::Mul;
        mul.dst.index = q;
        mul.dst.writemask = mask;
        mul.srcs[0] = resolve(inst.operands[0]);
        mul.srcs[1] = tempSrc(r);
        mul.srcs[1].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(mul);

        VInstr flr;
        flr.op = VOp::Flr;
        flr.dst.index = q;
        flr.dst.writemask = mask;
        flr.srcs[0] = tempSrc(q);
        program_.instrs.push_back(flr);

        VInstr mad;
        mad.op = VOp::Mad;
        mad.dst.index = define(inst.result);
        mad.dst.writemask = mask;
        mad.srcs[0] = tempSrc(q);
        mad.srcs[0].neg = true;
        mad.srcs[1] = resolve(inst.operands[1]);
        mad.srcs[1].swizzle = {0, 0, 0, 0};
        mad.srcs[2] = resolve(inst.operands[0]);
        program_.instrs.push_back(mad);
    }

    void lowerDiv(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        if (profile_ != GeneralProfile::Fragment) {
            program_.diagnostics.push_back(
                "nv40-general: VP div lowering deferred with the scalar unit");
            return;
        }
        // 1/x keeps its single-instruction form.
        if (isLiteralOne(inst.operands[0])) {
            VInstr vi;
            vi.op = VOp::Rcp;
            vi.dst.index = define(inst.result);
            vi.dst.writemask = componentMask(inst.resultType);
            vi.srcs[0] = resolve(inst.operands[1]);
            program_.instrs.push_back(vi);
            return;
        }
        // General x/y: RCP is a scalar op, so a w-wide divisor takes one
        // RCP per lane into a temp, then a single MUL.  Optimizing the
        // uniform-divisor case is optimization-level work.
        const int divisorWidth = valueWidthOf(inst.operands[1]);
        if (divisorWidth < 1 || divisorWidth > 4) {
            program_.diagnostics.push_back(
                "nv40-general: div by a divisor of unknown width; refusing");
            program_.loweringFailed = true;
            return;
        }
        const int mask = componentMask(inst.resultType);
        const int r = newVReg();
        for (int lane = 0; lane < divisorWidth; lane++) {
            VInstr rcp;
            rcp.op = VOp::Rcp;
            rcp.dst.index = r;
            rcp.dst.writemask = 1 << lane;
            rcp.srcs[0] = resolve(inst.operands[1]);
            const uint8_t comp = rcp.srcs[0].swizzle[lane];
            rcp.srcs[0].swizzle = {comp, comp, comp, comp};
            program_.instrs.push_back(rcp);
        }
        VInstr mul;
        mul.op = VOp::Mul;
        mul.dst.index = define(inst.result);
        mul.dst.writemask = mask;
        mul.srcs[0] = resolve(inst.operands[0]);
        mul.srcs[1] = tempSrc(r);
        if (divisorWidth == 1)
            mul.srcs[1].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(mul);
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

    void lowerLoadUniform(const IRInstruction& inst)
    {
        if (inst.result == InvalidIRValue)
            return;
        for (const auto& g : module_.globals) {
            if (g.name != inst.targetName)
                continue;
            // ARRAY uniforms are registered as a single const source for
            // the whole array, so aliasing an indexed load to it would
            // silently read element zero for every index (found in
            // review: offsets[1] emitted reading the base).  Refuse until
            // the array-uniform slice allocates per-element registers on
            // both the lowering and upload sides.
            if (g.type.isArray() || inst.componentIndex != 0) {
                program_.diagnostics.push_back(
                    "nv40-general: ldunif of array uniform '" +
                    inst.targetName +
                    "' is not implemented; refusing rather than aliasing "
                    "every index to the base");
                program_.loweringFailed = true;
                return;
            }
            const auto samplerIt = samplerUnit_.find(g.valueId);
            if (samplerIt != samplerUnit_.end()) {
                // A sampler is not a value with a source; it names a unit.
                samplerUnit_[inst.result] = samplerIt->second;
                return;
            }
            const auto mIt = matrixUniformBase_.find(g.valueId);
            if (mIt != matrixUniformBase_.end()) {
                matrixUniformBase_[inst.result] = mIt->second;
                const auto rIt = matrixUniformRows_.find(g.valueId);
                if (rIt != matrixUniformRows_.end())
                    matrixUniformRows_[inst.result] = rIt->second;
                return;
            }
            const auto sIt = program_.valueToSource.find(g.valueId);
            if (sIt != program_.valueToSource.end()) {
                program_.valueToSource[inst.result] = sIt->second;
                return;
            }
            break;
        }
        program_.diagnostics.push_back(
            "nv40-general: ldunif of '" + inst.targetName +
            "' has no registered uniform source; refusing");
        program_.loweringFailed = true;
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

    // mul(v, M) - a ROW vector times a matrix.  Component i of the
    // result is dot(v, column_i), but columns are not addressable as
    // single const registers; the same product IS addressable as a
    // linear combination of ROWS:
    //     result = v.x*M[0] + v.y*M[1] + v.z*M[2] + v.w*M[3]
    // which is one MUL and three MADs, fully vectorized, no transpose.
    void lowerVecMatMul(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue)
            return;
        if (profile_ != GeneralProfile::Vertex) {
            program_.diagnostics.push_back(
                "nv40-general: FP vecmatmul lowering is not implemented; refusing");
            program_.loweringFailed = true;
            return;
        }
        const auto matIt = matrixUniformBase_.find(inst.operands[1]);
        if (matIt == matrixUniformBase_.end()) {
            program_.diagnostics.push_back(
                "nv40-general: vecmatmul matrix source is not a uniform "
                "matrix (computed matrices await the matmul slice); refusing");
            program_.loweringFailed = true;
            return;
        }
        if (valueWidthOf(inst.operands[0]) != 4 ||
            inst.resultType.componentCount() != 4) {
            program_.diagnostics.push_back(
                "nv40-general: vecmatmul is lowered for vec4*mat4 only; refusing");
            program_.loweringFailed = true;
            return;
        }

        const int result = define(inst.result);
        const VSrc vec = resolve(inst.operands[0]);
        for (int j = 0; j < 4; ++j) {
            VInstr vi;
            vi.op = (j == 0) ? VOp::Mul : VOp::Mad;
            vi.dst.index = result;
            vi.dst.writemask = 0xf;
            vi.srcs[0] = vec;
            const uint8_t c = vec.swizzle[j];
            vi.srcs[0].swizzle = {c, c, c, c};
            vi.srcs[1] = uniformSrc(matIt->second + j, false);
            if (j != 0)
                vi.srcs[2] = tempSrc(result);
            program_.instrs.push_back(vi);
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

    // refract(I, N, eta):
    //   d = dot(N, I);  k = 1 - eta^2 * (1 - d^2)
    //   result = (k < 0) ? 0 : eta*I - (eta*d + sqrt(k)) * N
    //
    // The k<0 arm is resolved with the ARITHMETIC select - deliberately,
    // as the control-flow note's provably-finite opt-in: sqrt is taken of
    // |k| (abs modifier), so the "untaken" arm's value is finite for every
    // input and 0*finite cannot contaminate the blend the way 0*NaN would.
    // That is the whole reason the |k| is there.
    void lowerRefract(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment ||
            inst.operands.size() < 3 || inst.result == InvalidIRValue) {
            program_.diagnostics.push_back(
                "nv40-general: only FP refract lowering is supported; refusing");
            program_.loweringFailed = true;
            return;
        }
        // The sequence below is hardwired to DP3 and xyz masks.  The
        // symbol table registers refract for vec2/vec3/vec4 too - a vec2
        // would read an uninitialized z lane into the dot, and a vec4
        // result would leave .w unwritten under an xyzw consumer (found
        // in review by a width probe).  Refuse the widths the lowering
        // does not implement rather than emit either of those.
        if (inst.resultType.componentCount() != 3) {
            program_.diagnostics.push_back(
                "nv40-general: refract is lowered for float3 only; refusing");
            program_.loweringFailed = true;
            return;
        }

        const VSrc I   = resolve(inst.operands[0]);
        const VSrc N   = resolve(inst.operands[1]);
        const VSrc eta = resolve(inst.operands[2]);

        // t.x = d = dot(N, I); t.y = 1 - d^2; t.z = eta^2; t.w = k
        const int t = newVReg();
        VInstr dot;
        dot.op = VOp::Dp3;
        dot.dst.index = t;
        dot.dst.writemask = 0x1;
        dot.srcs[0] = N;
        dot.srcs[1] = I;
        program_.instrs.push_back(dot);

        VInstr oneMinusD2;
        oneMinusD2.op = VOp::Mad;
        oneMinusD2.dst.index = t;
        oneMinusD2.dst.writemask = 0x2;
        oneMinusD2.srcs[0] = tempSrc(t);
        oneMinusD2.srcs[0].swizzle = {0, 0, 0, 0};
        oneMinusD2.srcs[0].neg = true;
        oneMinusD2.srcs[1] = tempSrc(t);
        oneMinusD2.srcs[1].swizzle = {0, 0, 0, 0};
        oneMinusD2.srcs[2] = floatLit(1.0f);
        program_.instrs.push_back(oneMinusD2);

        VInstr eta2;
        eta2.op = VOp::Mul;
        eta2.dst.index = t;
        eta2.dst.writemask = 0x4;
        eta2.srcs[0] = eta;
        eta2.srcs[0].swizzle = {0, 0, 0, 0};
        eta2.srcs[1] = eta;
        eta2.srcs[1].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(eta2);

        VInstr k;
        k.op = VOp::Mad;
        k.dst.index = t;
        k.dst.writemask = 0x8;
        k.srcs[0] = tempSrc(t);
        k.srcs[0].swizzle = {2, 2, 2, 2};
        k.srcs[0].neg = true;
        k.srcs[1] = tempSrc(t);
        k.srcs[1].swizzle = {1, 1, 1, 1};
        k.srcs[2] = floatLit(1.0f);
        program_.instrs.push_back(k);

        // s.x = rsq(|k|); s.y = sqrt(|k|); s.z = eta*d + sqrt(|k|)
        const int s = newVReg();
        VInstr rsq;
        rsq.op = VOp::Rsq;
        rsq.dst.index = s;
        rsq.dst.writemask = 0x1;
        rsq.srcs[0] = tempSrc(t);
        rsq.srcs[0].swizzle = {3, 3, 3, 3};
        rsq.srcs[0].abs = true;
        program_.instrs.push_back(rsq);

        VInstr sq;
        sq.op = VOp::Rcp;
        sq.dst.index = s;
        sq.dst.writemask = 0x2;
        sq.srcs[0] = tempSrc(s);
        sq.srcs[0].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(sq);

        VInstr coef;
        coef.op = VOp::Mad;
        coef.dst.index = s;
        coef.dst.writemask = 0x4;
        coef.srcs[0] = eta;
        coef.srcs[0].swizzle = {0, 0, 0, 0};
        coef.srcs[1] = tempSrc(t);
        coef.srcs[1].swizzle = {0, 0, 0, 0};
        coef.srcs[2] = tempSrc(s);
        coef.srcs[2].swizzle = {1, 1, 1, 1};
        program_.instrs.push_back(coef);

        // r = eta*I - coef*N   (finite for all inputs)
        const int result = define(inst.result);
        VInstr etaI;
        etaI.op = VOp::Mul;
        etaI.dst.index = result;
        etaI.dst.writemask = 0x7;
        etaI.srcs[0] = I;
        etaI.srcs[1] = eta;
        etaI.srcs[1].swizzle = {0, 0, 0, 0};
        program_.instrs.push_back(etaI);

        VInstr subN;
        subN.op = VOp::Mad;
        subN.dst.index = result;
        subN.dst.writemask = 0x7;
        subN.srcs[0] = N;
        subN.srcs[0].neg = true;
        subN.srcs[1] = tempSrc(s);
        subN.srcs[1].swizzle = {2, 2, 2, 2};
        subN.srcs[2] = tempSrc(result);
        program_.instrs.push_back(subN);

        // c = (k < 0); result = r - r*c  (arithmetic select, both arms finite)
        VInstr cmp;
        cmp.op = VOp::Slt;
        cmp.dst.index = s;
        cmp.dst.writemask = 0x8;
        cmp.srcs[0] = tempSrc(t);
        cmp.srcs[0].swizzle = {3, 3, 3, 3};
        cmp.srcs[1] = floatLit(0.0f);
        program_.instrs.push_back(cmp);

        VInstr blend;
        blend.op = VOp::Mad;
        blend.dst.index = result;
        blend.dst.writemask = 0x7;
        blend.srcs[0] = tempSrc(result);
        blend.srcs[0].neg = true;
        blend.srcs[1] = tempSrc(s);
        blend.srcs[1].swizzle = {3, 3, 3, 3};
        blend.srcs[2] = tempSrc(result);
        program_.instrs.push_back(blend);
    }

    void lowerTex(const IRInstruction& inst)
    {
        if (inst.operands.size() < 2 || inst.result == InvalidIRValue) return;
        VInstr vi;
        vi.op = VOp::Tex;
        vi.dst.index = define(inst.result);
        vi.dst.writemask = componentMask(inst.resultType);
        vi.srcs[0] = resolve(inst.operands[1]);
        const auto unitIt = samplerUnit_.find(inst.operands[0]);
        if (unitIt == samplerUnit_.end()) {
            // Defaulting to 0 here is what made every sampler read the first
            // texture: the old code could not tell "unit 0" from "no idea".
            program_.diagnostics.push_back(
                "nv40-general: tex fetch whose sampler operand %" +
                std::to_string(inst.operands[0]) +
                " does not name a known sampler; refusing rather than "
                "defaulting to texture unit 0");
            program_.loweringFailed = true;
            return;
        }
        vi.texUnit = unitIt->second;
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
        // VP cmple(x, 0) keeps its predicate-source path: lowerSelect's
        // VP special case consumes conditionToSource_, and that pairing
        // is pinned by its fixture.
        if (profile_ == GeneralProfile::Vertex && isLiteralZero(inst.operands[1])) {
            VSrc src = resolve(inst.operands[0]);
            src.swizzle = {0, 0, 0, 0};
            conditionToSource_[inst.result] = src;
            return;
        }
        // Everything else is an ordinary comparison: SLE is native in
        // both units.  (This retires a diagnose-and-continue bail that
        // printed on dead cmple instructions in otherwise-clean
        // compiles and would have left a consumed one to the resolve()
        // refusal.)
        lowerBinary(inst, VOp::Sle);
    }

    // CF-1b: select(c, a, b) as a predicated write when the arms are
    // not provably finite.  Emitted as ONE SelPred pseudo-VInstr
    // (cond, thenVal, elseVal) that the FP emitter expands to
    //     MOV  dst, b            ; the default commits first
    //     MOVC CC.x, cond        ; OUT_NONE + COND_WRITE_ENABLE
    //     MOV  dst(NE.x), a      ; commits only where cond != 0
    // — the reference compiler's shape for guarded idioms (measured:
    // SGTRC RC.x then RCPR R0.x(NE.x); ours sets CC with a separate
    // MOV rather than folding it into the comparison, a known
    // instruction-count divergence to revisit when branch shaders
    // become byte-comparable).  Fragment-only and scalar-condition
    // only: the VP virtual scheduler reorders on temp dependencies
    // and has no CC model, and no VP witness exists in the corpus;
    // vector conditions want a witness before choosing a CC lane
    // strategy.  Returns false for shapes it does not cover — the
    // caller refuses loudly.
    bool lowerSelectPredicated(const IRInstruction& inst)
    {
        if (profile_ != GeneralProfile::Fragment)
            return false;
        if (inst.operands.size() < 3)
            return false;
        if (valueWidthOf(inst.operands[0]) != 1)
            return false;
        VInstr sel;
        sel.op = VOp::SelPred;
        sel.dst.index = define(inst.result);
        sel.dst.writemask = componentMask(inst.resultType);
        sel.srcs[0] = resolve(inst.operands[0]);
        {
            // CC is written through writemask x; make every lane of
            // the source read the condition lane so the encoding does
            // not depend on where the producer left it.
            const uint8_t c = sel.srcs[0].swizzle[0];
            sel.srcs[0].swizzle = {c, c, c, c};
        }
        sel.srcs[1] = resolve(inst.operands[1]);
        sel.srcs[2] = resolve(inst.operands[2]);
        program_.instrs.push_back(sel);
        return true;
    }

    bool lowerSelectGeneral(const IRInstruction& inst)
    {
        if (inst.operands.size() < 3)
            return false;
        // Operands: (cond, a, b) = cond ? a : b.
        const int mask = componentMask(inst.resultType);
        // A SCALAR condition selecting vector arms is legal and common;
        // the comparison wrote only .x of its register, so reading the
        // condition with an identity swizzle would pull three
        // uninitialized lanes into the blend (found in review by a
        // scalar-cond/vec4-arms probe).  Broadcast width-1 conditions,
        // keep identity when the widths match, refuse anything else.
        const int condWidth = valueWidthOf(inst.operands[0]);
        const int resultWidth = inst.resultType.componentCount();
        if (condWidth != 1 && condWidth != resultWidth)
            return false;
        const int d = newVReg();
        VInstr sub;
        sub.op = VOp::Add;
        sub.dst.index = d;
        sub.dst.writemask = mask;
        sub.srcs[0] = resolve(inst.operands[1]);
        sub.srcs[1] = resolve(inst.operands[2]);
        sub.srcs[1].neg = !sub.srcs[1].neg;
        program_.instrs.push_back(sub);

        VInstr mad;
        mad.op = VOp::Mad;
        mad.dst.index = define(inst.result);
        mad.dst.writemask = mask;
        mad.srcs[0] = resolve(inst.operands[0]);
        if (condWidth == 1) {
            const uint8_t c = mad.srcs[0].swizzle[0];
            mad.srcs[0].swizzle = {c, c, c, c};
        }
        mad.srcs[1] = tempSrc(d);
        mad.srcs[2] = resolve(inst.operands[2]);
        program_.instrs.push_back(mad);
        return true;
    }

    void lowerSelect(const IRInstruction& inst)
    {
        if (inst.operands.size() < 3 || inst.result == InvalidIRValue)
            return;
        if (profile_ != GeneralProfile::Vertex ||
            !isLiteralZero(inst.operands[1]) ||
            conditionToSource_.find(inst.operands[0]) == conditionToSource_.end()) {
            // CF-1a/1b: in a flattened program every select executes
            // with both arms unconditionally live, and the arithmetic
            // blend below is NOT a conditional move — if the untaken
            // arm yields inf/NaN, 0 * NaN poisons the join even though
            // the arm "was not taken".  Arms that are not provably
            // finite take CF-1b's predicated write instead (MOV
            // default, CC-set from cond, CC-gated commit — a true
            // conditional move); shapes predication does not cover yet
            // refuse.  KNOWN GAP, single-block programs: a bare
            // source-level `?:` in a straight-line shader still takes
            // the blend below UNGUARDED — the hazard belongs to the
            // blend, not to flattening, and the single-block path is
            // byte-frozen until the differential rig can judge the
            // switch in pixels (board item; measured witness: a
            // one-block 1.0/x ternary compiles through the blend
            // today).
            if (flattened_ &&
                (!provablyFinite(inst.operands[1]) ||
                 !provablyFinite(inst.operands[2]))) {
                if (lowerSelectPredicated(inst))
                    return;
                program_.diagnostics.push_back(
                    "nv40-general: join select arm is not provably finite "
                    "and predicated lowering does not cover this shape "
                    "(vector condition or VP profile); refusing");
                program_.loweringFailed = true;
                return;
            }
            // General select(c, a, b) with a 0/1 condition (which is what
            // the comparison lowerings produce): d = a - b, then
            // dst = c * d + b.  Component-wise, so vector conditions work.
            // The VP predicate special case above stays for the shape its
            // fixture pins.
            if (lowerSelectGeneral(inst))
                return;
            program_.diagnostics.push_back(
                "nv40-general: select condition could not be lowered; refusing");
            program_.loweringFailed = true;
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
                // OUTPUT PIN, not a preference (t_5dc260b0 fallout).  This
                // branch composes the colour lane by lane into a temp and
                // returns WITHOUT emitting any dst.output instruction, so
                // "this value is the colour" is carried by the pin alone.
                // The allocator is otherwise free to yield a pin whose
                // register is occupied - which is right for the precision
                // pins, and silently wrong here: the colour gets composed
                // in R1, nothing ever writes R0, and the framebuffer reads
                // whatever else landed there.  Measured on
                // hello-ppu-cellgcm-discard-blend once the register
                // numbering came low enough for texA to reach R0.
                for (VInstr& vi : program_.instrs) {
                    if (!vi.dst.output && vi.dst.index == regIt->second) {
                        vi.dst.preferredPhys = 0;
                        vi.dst.outputPin = true;
                    }
                }
                return;
            }
            if (!producer.dst.output &&
                producer.op != VOp::Mov &&
                producer.dst.index == regIt->second &&
                producer.dst.writemask != outMask &&
                producerDefs > 1 &&
                producer.preservePartialOutputMask) {
                // Same contract as the branch above.  This one DOES mark
                // the last producer as an output store, but the earlier
                // lane writes are pinned temps that start before it, so
                // the slot is occupied from the FIRST of them - which is
                // what allocatePhysicalTemps has to reserve.
                for (VInstr& vi : program_.instrs) {
                    if (!vi.dst.output && vi.dst.index == regIt->second) {
                        vi.dst.preferredPhys = 0;
                        vi.dst.outputPin = true;
                    }
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
            // SelPred is excluded from the store fold: on FP the
            // output IS R0, so folding would hand the expansion an
            // output-aliased destination the allocator cannot steer
            // sources away from (it assigns their registers long
            // before this fold runs), recreating the early-read alias
            // through the output file.  The explicit MOV costs one
            // instruction and keeps SelPred's destination a real
            // temp, where the allocator's alias exception holds.
            // (Found by the expansion's alias guard: four corpus
            // shaders folded a SelPred to the output and shipped a
            // TEMP(-1) destination encode at 9e7e84d.)
            if (!producer.dst.output && producer.dst.index == regIt->second &&
                producer.op != VOp::SelPred) {
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
        if (sourceWidth < 0) {
            // The width of the value being STORED, from the central
            // per-instruction record.  The old fallback used the
            // StoreOutput instruction's own resultType, which describes
            // nothing - a store has no result - so an instruction result
            // with no IRValue entry measured as width 1 and took the
            // scalar-broadcast branch below.  That is how `m[0]` came out
            // as `MOV o[n], c[256].x`, the row's x lane four times
            // (t_9da20b33).
            sourceWidth = valueWidthOf(value);
            if (sourceWidth <= 0)
                sourceWidth = inst.resultType.componentCount();
        }
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

    // Ops whose operands need legalising: more than one source, so they
    // can address two distinct input registers (illegal - one input
    // selector) or carry two inline const blocks (illegal - one block per
    // instruction).  The COMPARISONS were missing from this list, which
    // left both defects live on every comparison the general path emits:
    // `a < b` on two varyings compared b with itself, and a comparison of
    // a uniform against a literal appended two const blocks after one
    // instruction, so the second was decoded as an instruction
    // (t_40dd8159).  Everything not listed here is unary and cannot
    // reach either rule.
    static bool isArithmeticOp(VOp op)
    {
        switch (op) {
        case VOp::Add:
        case VOp::Mul:
        case VOp::Mad:
        case VOp::Dp2:
        case VOp::Dp3:
        case VOp::Dp4:
        case VOp::Min:
        case VOp::Max:
        case VOp::Slt:
        case VOp::Sgt:
        case VOp::Sle:
        case VOp::Sge:
        case VOp::Seq:
        case VOp::Sne:
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

    // A kill carries the instruction that computes its guard inline, so
    // every rule about operands applies to THAT op, not to VOp::Kil.
    static VOp effectiveOp(const VInstr& vi)
    {
        return vi.op == VOp::Kil ? vi.killFused : vi.op;
    }

    static int requiredSourceMask(const VInstr& vi)
    {
        switch (effectiveOp(vi)) {
        case VOp::Dp2: return 0x3;
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
        if (value)
            return componentMask(value->type);
        const auto it = valueWidth_.find(id);
        if (it != valueWidth_.end())
            return (1 << it->second) - 1;
        return componentMask(IRTypeInfo::Float4());
    }

    // Width of a value in components, 0 when genuinely unknown.
    int valueWidthOf(IRValueID id) const
    {
        const IRValue* value = entry_.getValue(id);
        if (value)
            return value->type.componentCount();
        const auto it = valueWidth_.find(id);
        return it != valueWidth_.end() ? it->second : 0;
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
            const VOp effOp = effectiveOp(vi);
            if (!isArithmeticOp(effOp)) {
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
            // An NV40 FRAGMENT instruction carries ONE input-source
            // selector, so two operands of register type INPUT read the
            // SAME varying whatever the emitter meant: `a - b` on two
            // varyings emitted as one ADD is `b - b`, silently, and the
            // container's input mask still names both (t_e89cd261).
            // Preload whenever an instruction addresses more than one
            // distinct input register - the reference does the same, and
            // it is the fragment counterpart of the vertex rule that an
            // instruction addressing two distinct input registers is
            // illegal.
            const bool forceFpInputPreload =
                profile_ == GeneralProfile::Fragment && !inputs.empty() &&
                (effOp == VOp::Mad || inputs.size() > 1);
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
                    effOp == VOp::Mad &&
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
                if (effOp == VOp::Dp3 && profile_ != GeneralProfile::Fragment)
                    applyDp3Swizzle(mov.srcs[0]);
                if (effOp == VOp::Dp3 && profile_ == GeneralProfile::Fragment &&
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
                if (effOp == VOp::Dp3 && profile_ != GeneralProfile::Fragment)
                    applyDp3Swizzle(src);
                if (effOp == VOp::Dp3 && profile_ == GeneralProfile::Fragment &&
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
        std::unordered_map<int, size_t> firstDef;
        std::set<int> defs;
        const bool dumpOrder = std::getenv("RSX_DUMP_ORDER") != nullptr;
        for (size_t i = 0; i < program_.instrs.size(); ++i) {
            const VInstr& vi = program_.instrs[i];
            if (!vi.dst.none && !vi.dst.output) {
                defs.insert(vi.dst.index);
                if (!firstDef.count(vi.dst.index))
                    firstDef[vi.dst.index] = i;
            }
            for (const VSrc& src : vi.srcs) {
                if (src.kind == VSrcKind::Temp)
                    lastUse[src.index] = i;
            }
        }

        // PEAK LIVE COUNT - how many virtual temps are in flight at once.
        //
        // This is the number of registers the program actually needs, and
        // it is NOT the number of definitions: a 63-instruction program
        // that sums sixteen terms defines 62 values and holds 18 at a
        // time, because the loop below reuses a register the moment its
        // occupant is last read.  Counted here as interval overlap over
        // the FINAL instruction order, with a value defined and never read
        // living for exactly its own instruction.  Conservative on the
        // high side: the destination is counted before the sources that
        // die at the same instruction are released, so a destination that
        // reuses a dying source's register is counted twice.  Over-
        // counting costs a register number, under-counting would cost a
        // spill - and the spill bank below starts exactly where this range
        // ends, so even an under-count stays compact.
        int peakLiveTemps = 0;
        {
            const size_t n = program_.instrs.size();
            std::vector<int> defsAt(n + 1, 0), diesAt(n + 1, 0);
            for (int v : defs) {
                const auto d = firstDef.find(v);
                if (d == firstDef.end())
                    continue;
                const auto l = lastUse.find(v);
                const size_t end = (l == lastUse.end() || l->second < d->second)
                    ? d->second : l->second;
                ++defsAt[d->second];
                ++diesAt[end];
            }
            int occ = 0;
            for (size_t i = 0; i < n; ++i) {
                occ += defsAt[i];
                if (occ > peakLiveTemps)
                    peakLiveTemps = occ;
                occ -= diesAt[i];
            }
        }

        // FRAGMENT only: an output destination OCCUPIES the temp register
        // of the same index - the colour output is R0 - and it stays live
        // from its store to the END of the program, because that is when
        // the framebuffer reads it.  Temps were allocated with no
        // knowledge of that, so a temp whose live range crossed a store
        // could be given the store's register and clobber it.  Measured on
        // fp_discard_nested_f (t_dabb23e1): `MOVR R0, f[TEX0]` then
        // `SLTR R0.x, ...` - the kill fired on exactly the right pixels
        // and every surviving one was painted with the comparison.
        //
        // Recorded as the EARLIEST store per output, because the register
        // is live from there on.  Nothing is reserved outright: a temp
        // whose last use precedes the store cannot conflict, which is why
        // the overwhelming case - a shader that stores its output last -
        // allocates exactly as before.
        std::unordered_map<int, size_t> outputStorePos;
        std::unordered_map<int, int> outputPinOwner;
        if (profile_ == GeneralProfile::Fragment) {
            for (size_t i = 0; i < program_.instrs.size(); ++i) {
                const VInstr& vi = program_.instrs[i];
                if (vi.dst.none || !vi.dst.output) continue;
                auto it = outputStorePos.find(vi.dst.index);
                if (it == outputStorePos.end())
                    outputStorePos[vi.dst.index] = i;
            }
            // An OUTPUT PIN occupies its slot from its FIRST write, and
            // lowerStoreOutput's lane-by-lane branches emit no store
            // instruction at all - so a guard built only from dst.output
            // instructions was inert on exactly the shape that needs it.
            // Recorded as the earliest, so a slot that has both a pinned
            // write and a later store is reserved from the pin.
            for (size_t i = 0; i < program_.instrs.size(); ++i) {
                const VInstr& vi = program_.instrs[i];
                if (vi.dst.none || vi.dst.output || !vi.dst.outputPin)
                    continue;
                const int slot = vi.dst.fp16 ? (vi.dst.preferredPhys >> 1)
                                             : vi.dst.preferredPhys;
                if (slot < 0)
                    continue;
                auto it = outputStorePos.find(slot);
                if (it == outputStorePos.end() || i < it->second)
                    outputStorePos[slot] = i;
                // WHO the slot is reserved FOR.  Without this the
                // reservation blocks the pinned value against itself: the
                // colour vreg asks for R0, the guard sees R0 reserved from
                // the colour's own first write, and the pin is rejected -
                // which moved fp_pow_computed_literal_f off R0 and split
                // its two composing writes across two registers, dropping
                // the alpha lane.  Caught by the byte-identity column of
                // the corpus sweep, not by any test.
                outputPinOwner[slot] = vi.dst.index;
            }
        }

        std::vector<int> freeList;
        int nextPhys = 0;
        // FP numbers DOWN from the top of the range it needs, so the range
        // has to be the peak live count and not the definition count.
        // Seeding it at defs.size() made the REPORTED register count -
        // which is the highest slot ever written, plus one - track the
        // number of definitions, so a program holding 18 values at a time
        // declared 62 registers and was refused by the fragment budget
        // below at 48.  The registers past the peak were never touched;
        // only their numbers were spent (t_5dc260b0).
        //
        // Fragment fallback counter: the bank above the ordinary
        // descending range, used only when a candidate is rejected.  It
        // starts where the descending range ends, so the two never
        // collide and a rejection costs one number rather than a jump to
        // the top of the definition count.
        const int fpRegBase = legacyCapacityProbe_
            ? std::max(1, static_cast<int>(defs.size()))
            : std::max(1, peakLiveTemps);
        int fpSpill = static_cast<int>(defs.size());
        if (profile_ == GeneralProfile::Fragment) {
            fpSpill = fpRegBase;
            nextPhys = fpRegBase - 1;
        }
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
            // SelPred breaks the read-then-write assumption every other
            // VOp satisfies: it WRITES dst in its first expanded
            // instruction and READS srcs[0]/srcs[1] in later ones
            // (found in review of 9e7e84d as a live miscompile — the
            // condition's dying register was reused as dst, so CC was
            // set from the just-written else value and the then arm
            // never committed).  So for SelPred, dst must not share a
            // register with srcs[0] or srcs[1] on ANY assignment path;
            // srcs[2] is read by the same instruction that writes dst
            // and stays shareable.  H registers alias R slots in
            // pairs, so the comparison is on R slots.
            // A candidate register conflicts with a live output when the
            // temp's live range reaches the output's store: the output is
            // live from there to the end, so any overlap is a clobber.
            // H registers alias R slots in pairs, so the comparison is on
            // R slots, the same rule as aliasesEarlyRead.
            const auto clobbersLiveOutput = [&](int candidate, bool candFp16) {
                if (outputStorePos.empty())
                    return false;
                const auto lu = lastUse.find(vi.dst.index);
                const size_t deadAfter = lu == lastUse.end() ? i : lu->second;
                const int candSlot = candFp16 ? (candidate >> 1) : candidate;
                // STRICT: a temp whose last read IS the store commits
                // no clobber - the store reads it and writes the output in
                // the same instruction, which is the read-then-write the
                // allocator's own reuse path already relies on.  Only a
                // temp still live AFTER the store overlaps the output's
                // live range.
                for (const auto& kv : outputStorePos) {
                    if (kv.first != candSlot || kv.second >= deadAfter)
                        continue;
                    // The value the slot is reserved FOR is not clobbering
                    // it by occupying it - that is the reservation working.
                    const auto owner = outputPinOwner.find(kv.first);
                    if (owner != outputPinOwner.end() &&
                        owner->second == vi.dst.index)
                        continue;
                    return true;
                }
                return false;
            };
            const auto aliasesEarlyRead = [&](int candidate, bool candFp16) {
                if (vi.op != VOp::SelPred)
                    return false;
                const int candSlot = candFp16 ? (candidate >> 1) : candidate;
                for (int s = 0; s < 2; ++s) {
                    const VSrc& src = vi.srcs[s];
                    if (src.kind != VSrcKind::Temp)
                        continue;
                    const auto it = program_.vregToPhys.find(src.index);
                    if (it == program_.vregToPhys.end())
                        continue;
                    const bool sFp16 =
                        program_.vregToFp16.count(src.index) &&
                        program_.vregToFp16[src.index];
                    const int srcSlot = sFp16 ? (it->second >> 1) : it->second;
                    if (srcSlot == candSlot)
                        return true;
                }
                return false;
            };
            if (!vi.dst.none &&
                !vi.dst.output &&
                program_.vregToPhys.find(vi.dst.index) == program_.vregToPhys.end()) {
                // A PIN IS A PREFERENCE, NOT A MANDATE.  It used to be
                // honoured unconditionally - overriding the free list and
                // the reuse path with no check that the register was free
                // - which is fine while at most one pinned value is live
                // at a time and silently wrong otherwise: two pinned
                // results share one register and a consumer reads the same
                // value twice.  lowerStep pins EVERY step() result to
                // phys 0, so `step(a,x) * step(x,b)` computed a*a
                // (t_929c0177; measured as the wrong border columns of
                // test_62_v_address_register).
                //
                // 3aec606 made that REFUSE.  This yields instead: when the
                // pinned register's current occupant is still read after
                // this instruction, fall through to ordinary allocation.
                // The pin is then honoured whenever it can be, which is
                // every case that worked before, and skipped exactly where
                // it used to corrupt.
                //
                // WHY YIELDING IS SAFE FOR THE PRECISION SHAPING the pins
                // exist for: the shaping needs the fp16 destination and the
                // later full-precision read to ALIAS, and that is carried
                // by the vreg (same vreg, fp16 flag on the source), not by
                // the register NUMBER.  The ordinary fp16 path allocates
                // `nextPhys << 1`, which is an H register aliasing its R
                // slot exactly as H0 aliases R0 - so SGE-then-MOVX still
                // converts in place, one slot further along.  If that is
                // wrong for some pin the rig will say so: the prediction
                // on this commit is that the 13 live-clobber shaders which
                // do NOT change state are the evidence it holds.
                //
                // Liveness, not duplication: a pin over a DEAD value is
                // free.  Alias sets, not raw indices - H registers have
                // their own index space and alias R slots in pairs, same
                // rule as aliasesEarlyRead.
                const auto pinClobbersLive = [&]() {
                    const int pinSlot =
                        vi.dst.fp16 ? (vi.dst.preferredPhys >> 1)
                                    : vi.dst.preferredPhys;
                    for (const auto& kv : program_.vregToPhys) {
                        if (kv.first == vi.dst.index)
                            continue;
                        const bool occFp16 =
                            program_.vregToFp16.count(kv.first) &&
                            program_.vregToFp16[kv.first];
                        const int occSlot =
                            occFp16 ? (kv.second >> 1) : kv.second;
                        if (occSlot != pinSlot)
                            continue;
                        const auto lu = lastUse.find(kv.first);
                        if (lu != lastUse.end() && lu->second > i)
                            return true;
                    }
                    return false;
                };

                int phys;
                if (vi.dst.outputPin && vi.dst.preferredPhys >= 0 &&
                    (pinClobbersLive() ||
                     clobbersLiveOutput(vi.dst.preferredPhys, vi.dst.fp16))) {
                    // An output pin is the CONTRACT that this value is the
                    // colour, so yielding it does not cost an optimisation
                    // - it produces a program that computes the right
                    // colour into a register nothing reads.  The
                    // reservation above should make this unreachable; if
                    // it is ever reached the allocator has a defect, and a
                    // refusal names it rather than shipping a picture.
                    program_.diagnostics.push_back(
                        "nv40-general-fp: the colour output's register R" +
                        std::to_string(vi.dst.preferredPhys) +
                        " is held by a value that outlives the store; "
                        "refusing rather than composing the colour off-slot "
                        "(t_5dc260b0)");
                    program_.loweringFailed = true;
                    return;
                }
                if (vi.dst.preferredPhys >= 0 && !pinClobbersLive() &&
                    !clobbersLiveOutput(vi.dst.preferredPhys, vi.dst.fp16)) {
                    phys = vi.dst.preferredPhys;
                } else {
                    auto reusableSrc = std::find_if(
                        vi.srcs.begin(), vi.srcs.end(),
                        [&](const VSrc& src) {
                            // SelPred may only reuse srcs[2] (the else
                            // value) — see aliasesEarlyRead above.
                            if (vi.op == VOp::SelPred &&
                                (&src - vi.srcs.data()) != 2)
                                return false;
                            return src.kind == VSrcKind::Temp &&
                                   !src.fp16 &&
                                   !vi.dst.fp16 &&
                                   lastUse[src.index] == i &&
                       program_.vregToPhys.find(src.index) != program_.vregToPhys.end();
                        });
                    if (reusableSrc != vi.srcs.end() &&
                        !clobbersLiveOutput(
                            program_.vregToPhys[reusableSrc->index],
                            false)) {
                        phys = program_.vregToPhys[reusableSrc->index];
                    } else if (!vi.dst.fp16 && !freeList.empty() &&
                               !aliasesEarlyRead(freeList.back(), false) &&
                               !clobbersLiveOutput(freeList.back(), false)) {
                        // Only the head is tested: an aliasing head
                        // falls through to the fresh counter rather
                        // than scanning deeper.  Deliberate
                        // conservatism - it burns a register under
                        // pressure but keeps this path's behaviour
                        // trivially reasoned about; scan if a real
                        // shader ever exhausts the bank over it.
                        phys = freeList.back();
                        freeList.pop_back();
                    } else {
                        // FP H registers have their own index space but alias
                        // full R slots in pairs: H0/H1 -> R0, H2/H3 -> R1.
                        do {
                            if (profile_ == GeneralProfile::Fragment) {
                                // The fragment counter walks DOWN to 0, so
                                // a rejected candidate can walk it past the
                                // bottom of the bank.  When it does, take a
                                // register ABOVE the ordinary range instead
                                // of a negative one - which is what the
                                // reserved-output case needs, and it leaves
                                // every shader that never rejects a
                                // candidate allocating exactly as before.
                                if (nextPhys < 0) {
                                    phys = vi.dst.fp16 ? (fpSpill << 1)
                                                       : fpSpill;
                                    ++fpSpill;
                                } else {
                                    phys = vi.dst.fp16 ? (nextPhys-- << 1)
                                                       : nextPhys--;
                                }
                            } else {
                                phys = vi.dst.fp16 ? (nextPhys++ << 1) : nextPhys++;
                            }
                        } while (aliasesEarlyRead(phys, vi.dst.fp16) ||
                                 clobbersLiveOutput(phys, vi.dst.fp16));
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
    case VOp::Frc: return NVFX_FP_OP_OPCODE_FRC;
    case VOp::Flr: return NVFX_FP_OP_OPCODE_FLR;
    case VOp::Kil: return NVFX_FP_OP_OPCODE_KIL;
    case VOp::Dp2: return NVFX_FP_OP_OPCODE_DP2;
    case VOp::Sge: return NVFX_FP_OP_OPCODE_SGE;
    case VOp::Slt: return NVFX_FP_OP_OPCODE_SLT;
    case VOp::Sgt: return NVFX_FP_OP_OPCODE_SGT;
    case VOp::Sle: return NVFX_FP_OP_OPCODE_SLE;
    case VOp::Seq: return NVFX_FP_OP_OPCODE_SEQ;
    case VOp::Sne: return NVFX_FP_OP_OPCODE_SNE;
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
    case VOp::Frc: return VP_OP(FRC);
    case VOp::Flr: return VP_OP(FLR);
    case VOp::Sge: return VP_OP(SGE);
    case VOp::Slt: return VP_OP(SLT);
    case VOp::Sgt: return VP_OP(SGT);
    case VOp::Sle: return VP_OP(SLE);
    case VOp::Seq: return VP_OP(SEQ);
    case VOp::Sne: return VP_OP(SNE);
    case VOp::DivSqrt:
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

// Which pool slots hold packed SCALARS and which hold one whole vector
// literal.  The two are allocated differently and must not be matched
// against each other: appending a scalar into a vector's register, or
// broadcasting a scalar out of a lane of one, would both be shapes the
// reference does not produce.
struct VpLiteralAlloc
{
    std::vector<size_t> scalarSlots;
    std::vector<size_t> vectorSlots;
};

static VSrc assignVpLiteralSource(const VSrc& literal,
                                  VpAttributes& attrs,
                                  int& nextLiteralReg,
                                  VpLiteralAlloc& alloc)
{
    VSrc out = literal;
    out.kind = VSrcKind::Uniform;
    out.embeddedUniform = false;

    // A VECTOR literal gets a const register of its own, holding all of
    // its lanes, and keeps whatever swizzle the source already carried.
    // Measured against the reference: `out = float4(a,b,c,d)` is
    // `MOV o[n], c[467]` with C[467] declared float4, and two identical
    // vec4 literals SHARE one register.  Reading literal[0] and
    // broadcasting it - which is what this did for every literal - made
    // every such store paint one value four times (t_3e342903).
    if (literal.literalLanes > 1) {
        const uint8_t lanes = literal.literalLanes;
        for (size_t idx : alloc.vectorSlots) {
            const auto& slot = attrs.literalPool[idx];
            if (slot.usedLanes != lanes) continue;
            bool same = true;
            for (uint32_t lane = 0; lane < slot.usedLanes; ++lane) {
                if (!floatBitsEqual(slot.values[lane], literal.literal[lane])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                out.index = static_cast<int>(slot.constReg);
                return out;
            }
        }

        VpLiteralPoolSlot slot;
        slot.constReg = static_cast<uint32_t>(nextLiteralReg--);
        slot.usedLanes = lanes;
        for (uint32_t lane = 0; lane < lanes; ++lane)
            slot.values[lane] = literal.literal[lane];
        alloc.vectorSlots.push_back(attrs.literalPool.size());
        attrs.literalPool.push_back(slot);
        out.index = static_cast<int>(slot.constReg);
        return out;
    }

    const float value = literal.literal[0];
    for (size_t idx : alloc.scalarSlots) {
        const auto& slot = attrs.literalPool[idx];
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

    if (!alloc.scalarSlots.empty()) {
        auto& slot = attrs.literalPool[alloc.scalarSlots.back()];
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
    alloc.scalarSlots.push_back(attrs.literalPool.size());
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

static void seedFpEmbeddedUniforms(const IRFunction& entry,
                                   const VirtualProgram& program,
                                   FpAttributes& attrs)
{
    // File-scope uniforms first in intent, appended after the parameters
    // below: recordFpUniformOffset matches on the slot index, so an entry
    // must exist for every slot a source can name or the offsets are
    // dropped and the container advertises a parameter nothing can patch.
    for (unsigned slot : program.fpGlobalUniformSlots)
        attrs.embeddedUniforms.push_back({slot, {}});
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
    seedFpEmbeddedUniforms(entry, program, attrs);
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
                    src.index <= NVFX_FP_OP_INPUT_SRC_TC(9)) {
                    const uint16_t bit =
                        uint16_t{1} << (src.index - NVFX_FP_OP_INPUT_SRC_TC(0));
                    // texCoordsInputMask only: texCoords2D is derived
                    // once below, from the DECLARED widths.  This used to
                    // clear on `op != Tex && !scalarYRead`, which is a rule
                    // about consumed lanes and gets the common case
                    // backwards - a float4 TEXCOORD read only as .xy by an
                    // ordinary tex2D must still clear.
                    attrs.texCoordsInputMask |= bit;
                }
            }
        }
        if (vi.op == VOp::Kil) {
            // CF-2: two hardware instructions from one node.  First the
            // guard's producer, retargeted to the condition register -
            // OUT_NONE with the 0x3F sentinel index, so it writes the CC
            // and no general register - then the KIL testing lane x.
            //
            // The KIL's own destination is that same register 63, which
            // is why a kill can never be mistaken for a write to the
            // colour output: the rig's `outw` decode reads the
            // destination field of the same word and sees 63, not 0.
            struct nvfx_reg ccDst = nvfx_reg(NVFXSR_NONE, 0x3F);
            struct nvfx_insn set = nvfx_insn(
                vi.sat, 0, -1, -1, ccDst, NVFX_FP_MASK_X,
                nvfxSource(vi.srcs[0]),
                nvfxSource(vi.srcs[1]),
                nvfxSource(vi.srcs[2]));
            set.cc_update = 1;
            if (vi.fpPrecisionOverride >= 0)
                set.precision = static_cast<uint8_t>(vi.fpPrecisionOverride);
            asm_.emit(set, fpOpcode(vi.killFused));
            for (const VSrc& src : vi.srcs) {
                if (src.kind != VSrcKind::Uniform &&
                    src.kind != VSrcKind::Literal)
                    continue;
                const uint32_t offset = asm_.currentByteSize();
                if (src.kind == VSrcKind::Uniform) {
                    recordFpUniformOffset(
                        attrs, static_cast<unsigned>(src.index), offset);
                    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    asm_.appendConstBlock(zeros);
                } else {
                    asm_.appendConstBlock(src.literal.data());
                }
            }
            const VSrc noneSrc{};
            struct nvfx_insn kil = nvfx_insn(
                0, 0, -1, -1, ccDst,
                NVFX_FP_MASK_X | NVFX_FP_MASK_Y,
                nvfxSource(noneSrc), nvfxSource(noneSrc), nvfxSource(noneSrc));
            kil.precision = 0;
            // A negated guard flips the TEST, and leaves the comparison
            // alone - the reference emits SGT with EQ for `!(a > b)`,
            // never SLE with NE.  The two differ on NaN and on the
            // container, and the container is what the fence reads.
            kil.cc_cond = vi.killTestEq ? NVFX_COND_EQ : NVFX_COND_NE;
            kil.cc_swz[0] = kil.cc_swz[1] = kil.cc_swz[2] = kil.cc_swz[3] = 0;
            asm_.emit(kil, NVFX_FP_OP_OPCODE_KIL);
            // One $kill_NNNN container parameter per discard STATEMENT.
            attrs.pixelKillCount += 1;
            emittedInstruction = true;
            continue;
        }
        if (vi.op == VOp::SelPred) {
            // CF-1b: expand the atomic predicated-select node into its
            // three hardware instructions (see the VOp comment).  Each
            // instruction carries only its own source, so each
            // literal/uniform const block is appended right after the
            // instruction that references it — the same layout the
            // single-instruction path below produces.
            //
            // Alias guard (found in review of 9e7e84d, live miscompile):
            // SelPred WRITES dst in its first instruction and READS
            // srcs[0]/srcs[1] in later ones, breaking the read-then-
            // write assumption every allocator path relies on.  If the
            // allocator ever hands dst a register shared with the
            // condition or the then-value (H registers alias R slots
            // in pairs, so compare R slots), the expansion would
            // silently compute "else everywhere" — refuse instead.
            // srcs[2] is read by the same instruction that writes dst
            // and may share.
            {
                if (vi.dst.phys < 0) {
                    out.diagnostics.push_back(
                        "nv40-general: SelPred destination has no "
                        "physical register; refusing");
                    return out;
                }
                const int dstSlot =
                    vi.dst.fp16 ? (vi.dst.phys >> 1) : vi.dst.phys;
                for (int s = 0; s < 2; ++s) {
                    const VSrc& src = vi.srcs[s];
                    if (src.kind != VSrcKind::Temp)
                        continue;
                    // A Temp source with no physical register would
                    // make regFromSource encode src.index as a
                    // register number - refuse rather than compare
                    // garbage (review strengthening on 1eb3c21).
                    if (src.phys < 0) {
                        out.diagnostics.push_back(
                            "nv40-general: SelPred source has no "
                            "physical register; refusing");
                        return out;
                    }
                    const int srcSlot =
                        src.fp16 ? (src.phys >> 1) : src.phys;
                    if (srcSlot == dstSlot) {
                        out.diagnostics.push_back(
                            "nv40-general: SelPred destination register "
                            "aliases an early-read source; refusing "
                            "rather than emitting an always-else select");
                        return out;
                    }
                }
            }
            struct nvfx_reg selDst = nvfx_reg(NVFXSR_TEMP, vi.dst.phys);
            selDst.is_fp16 = vi.dst.fp16 ? 1 : 0;
            // OUT_NONE with the 0x3F sentinel index: writes the
            // condition register only (the default path's CC-set shape).
            struct nvfx_reg ccDst = nvfx_reg(NVFXSR_NONE, 0x3F);
            const VSrc noneSrc{};
            const auto emitOne = [&](const struct nvfx_reg& d, int mask,
                                     const VSrc& s, bool ccSet,
                                     bool ccGated) {
                struct nvfx_insn insn = nvfx_insn(
                    ccSet ? 0 : vi.sat, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(d), mask,
                    nvfxSource(s), nvfxSource(noneSrc), nvfxSource(noneSrc));
                if (ccSet)
                    insn.cc_update = 1;
                if (ccGated) {
                    // Commit only where cond != 0; every lane reads
                    // CC.x, where the scalar condition was written.
                    insn.cc_test = 1;
                    insn.cc_cond = NVFX_FP_OP_COND_NE;
                    insn.cc_swz[0] = insn.cc_swz[1] =
                    insn.cc_swz[2] = insn.cc_swz[3] = 0;
                }
                asm_.emit(insn, NVFX_FP_OP_OPCODE_MOV);
                if (s.kind == VSrcKind::Uniform) {
                    const uint32_t offset = asm_.currentByteSize();
                    recordFpUniformOffset(
                        attrs, static_cast<unsigned>(s.index), offset);
                    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    asm_.appendConstBlock(zeros);
                } else if (s.kind == VSrcKind::Literal) {
                    asm_.appendConstBlock(s.literal.data());
                }
            };
            emitOne(selDst, vi.dst.writemask, vi.srcs[2], false, false);
            emitOne(ccDst, 0x1, vi.srcs[0], true, false);
            emitOne(selDst, vi.dst.writemask, vi.srcs[1], false, true);
            emittedInstruction = true;
            continue;
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
    // texCoords2D, derived once from the declared widths and the final
    // read mask - the same rule and the same reason as the fragment
    // default path:
    //
    //   bit n is CLEARED iff TEXCOORDn is READ and DECLARED wider than float2
    //
    attrs.texCoords2D = 0xFFFFu;
    for (const auto& kv : program.texcoordDeclaredWidth) {
        const int n = kv.first;
        if (n < 0 || n > 15) continue;
        if (!((attrs.texCoordsInputMask >> n) & 1u)) continue;
        if (kv.second > 2)
            attrs.texCoords2D &= static_cast<uint16_t>(~(uint16_t{1} << n));
    }

    // A file-scope uniform declared with an initialiser carries that value
    // as its COMPILED DEFAULT: write it into every const block the
    // parameter lists, which are exactly the blocks a runtime patch of that
    // name overwrites.  Done here, over the recorded offsets, for the same
    // reason the default path does it there - the value belongs to the
    // parameter, not to any one instruction that reads it.
    for (const auto& eu : attrs.embeddedUniforms) {
        const auto it = program.fpUniformDefaults.find(eu.entryParamIndex);
        if (it == program.fpUniformDefaults.end())
            continue;
        for (uint32_t off : eu.ucodeByteOffsets)
            asm_.setUniformConstBlock(off, it->second.first.data(),
                static_cast<unsigned>(it->second.first.size()));
    }

    asm_.markEnd();
    if (program.loweringFailed) {
        out.diagnostics.push_back(
            "nv40-general: refusing to emit - the program did not lower completely");
        out.ok = false;
        return out;
    }
    // FRAGMENT TEMP-REGISTER BUDGET.  A program that allocates past the
    // usable fragment register file does not merely render wrong: on RPCS3
    // it paints nothing AND poisons the RSX state, so every later draw in
    // the run paints nothing either (t_5dc260b0; the rig's poison canary
    // was built to catch it).  Refusing is the only safe answer while the
    // allocator can produce such a program.
    //
    // 48 is not proven and the comment should not pretend otherwise.  The
    // instruction ENCODING is not the bound - NV40_FP_OP_OUT_REG_MASK is
    // (63 << 1), six bits, so R0..R63 all encode.  48 is RPCS3's annotation
    // threshold plus an empty band in our corpus: every shader we JUDGE
    // sits at 44 or below (test_18_atan2 and test_61 at 44, most at 2),
    // and the only ones at or above 48 are the known poisoners at 55, 57,
    // 62, 73, 77 and 119.  Nothing occupies 45..54, so the corpus cannot
    // distinguish a bound anywhere in that band.  A synthetic family that
    // keeps N values live for N = 40..60 would turn this citation into a
    // measurement; until then it is a citation.
    //
    // This guard is the COMPANION to the pin yield above, not an
    // independent idea.  The yield un-refuses four programs that the pin
    // collision was accidentally stopping, and all four are past this
    // budget - so landing the yield without this would put known poisoners
    // back into a sweep that no longer excludes two of them.
    static constexpr int kFpTempRegisterBudget = 48;
    const int tempRegs = std::max(2, asm_.numTempRegs());
    if (tempRegs >= kFpTempRegisterBudget) {
        out.diagnostics.push_back(
            "nv40-general-fp: program needs " + std::to_string(tempRegs) +
            " temp registers, at or past the usable fragment budget of " +
            std::to_string(kFpTempRegisterBudget) +
            " - such a program paints nothing and poisons the RSX for every "
            "later draw; refusing (t_5dc260b0)");
        out.ok = false;
        return out;
    }

    // TRANSITIONAL CAPACITY GATE, second half (director, 2026-09-02 17:18:
    // option C).  7243bd9 numbers the fragment bank from the peak live
    // count, so programs the old numbering declared at 48 or more registers
    // now compile - and of the seven in the corpus, SIX paint the wrong
    // picture against sce-cgc (an R0 live-range collision, a store-order
    // defect and three unrelated ones: t_c2582cf1, t_aaa7c7b1), every one
    // hidden until today by the refusal.  Until each shape is judged, what
    // the old numbering refused still refuses, BY NAME, keyed on the count
    // that numbering produces (the dry run in lowerFunction), so the only
    // behaviour the seed fix ships is a correct declared count on programs
    // that already compiled.  Proof: the 318-shader sweep against 6ece362
    // admits nothing new and moves no bytes.
    //
    // RSXCG_UNVERIFIED_CAPACITY=1 lifts the gate for the tests and the rig
    // that exist to judge those shapes; it is not a user flag.
    if (program.legacyDeclaredTemps >= kFpTempRegisterBudget &&
        std::getenv("RSXCG_UNVERIFIED_CAPACITY") == nullptr) {
        out.diagnostics.push_back(
            "nv40-general-fp: the pre-2026-09-02 numbering declared " +
            std::to_string(program.legacyDeclaredTemps) +
            " temp registers for this program (budget " +
            std::to_string(kFpTempRegisterBudget) +
            "); the general path is not yet pixel-verified above that "
            "capacity (six of seven such corpus programs paint wrong) - "
            "refusing until the shape is judged (t_5dc260b0 gate)");
        out.ok = false;
        return out;
    }

    out.words = asm_.words();
    out.ok = true;
    attrs.registerCount = static_cast<uint8_t>(tempRegs);
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
    VpLiteralAlloc literalAlloc;
    for (VInstr& vi : program.instrs) {
        for (VSrc& src : vi.srcs) {
            if (src.kind == VSrcKind::Literal)
                src = assignVpLiteralSource(src, attrs, nextLiteralReg,
                                            literalAlloc);
        }
    }
    // The pool grows DOWN from the last free uniform register and the
    // matrices grow UP from 256, so running past that watermark would
    // hand a literal a register a matrix row already owns - a silent
    // wrong value, not a compile error.  Nothing checked it while every
    // literal was a single packed lane; vector literals take a register
    // each, so it is now reachable and refused (t_3e342903).
    // nextLiteralReg is the NEXT register to hand out, so the LOWEST one
    // actually allocated is nextLiteralReg + 1 - and the floor register
    // itself is legal.  Comparing the next pointer instead refused a
    // shader that exactly filled the last legal register (codex, review
    // of the first version).
    const int lowestLiteralReg = nextLiteralReg + 1;
    if (lowestLiteralReg < program.vpConstFloor) {
        out.diagnostics.push_back(
            "nv40-vp: the literal pool reached c[" +
            std::to_string(lowestLiteralReg) + "], below c[" +
            std::to_string(program.vpConstFloor) +
            "] where the matrix uniforms start; refusing rather than "
            "emitting a literal that reads a matrix row (t_3e342903)");
        return out;
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
    if (program.loweringFailed) {
        out.diagnostics.push_back(
            "nv40-general: refusing to emit - the program did not lower completely");
        out.ok = false;
        return out;
    }
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
