/*
 * NV40 vertex-program lowering.
 *
 * Walks an IRFunction and produces an NV40 ucode stream via VpAssembler.
 *
 * Handles:
 *   - identity passthrough (LoadAttribute → StoreOutput with matching semantic)
 *   - uniform reads:
 *       `uniform float4` allocated top-down from c[467]
 *       `uniform float4x4` allocated bottom-up from c[256] (four rows)
 *       StoreOutput whose source is a LoadUniform emits MOV o[N], c[K]
 *   - matrix × vector → 4 DP4s:
 *       IROp::MatVecMul against a uniform matrix defers emit until we see
 *       StoreOutput consume it; then emit 4 DP4s writing W, Z, Y, X to the
 *       destination output, pulling successive matrix rows (base+3..base+0).
 *       Matches the reference compiler's layout — component order, rows
 *       descending.
 */

#include "nv40_emit.h"
#include "nv40_vp_assembler.h"

#include "nv40_vertprog.h"
#include "nv30_vertprog.h"
#include "nvfx_shader.h"

#include "ir.h"

#include <algorithm>
#include <cctype>
#include <functional>
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
    // module-level globals the front-end may have split off.  An explicit
    // `: register(CN)` binding on a global pins that uniform to c[N] and
    // skips the auto-allocator entirely.
    void allocate(const IRFunction& entry, const IRModule& module)
    {
        for (const auto& param : entry.parameters)
        {
            if (param.storage == StorageQualifier::Uniform)
                assign(param.name, param.type, /*explicitBase=*/-1);
        }
        for (const auto& global : module.globals)
        {
            if (global.storage != StorageQualifier::Uniform) continue;
            if (bindings_.count(global.name)) continue;
            const int explicitBase =
                (global.explicitRegisterBank == 'C')
                    ? global.explicitRegisterIndex
                    : -1;
            assign(global.name, global.type, explicitBase);
        }
    }

    const ConstBinding* lookup(const std::string& name) const
    {
        auto it = bindings_.find(name);
        return (it == bindings_.end()) ? nullptr : &it->second;
    }

private:
    void assign(const std::string& name, const IRTypeInfo& type, int explicitBase)
    {
        ConstBinding b;
        if (type.isMatrix())
        {
            b.rowCount = type.matrixRows;
            if (explicitBase >= 0)
            {
                b.baseReg = explicitBase;
            }
            else
            {
                b.baseReg = nextMatrixReg_;
                nextMatrixReg_ += b.rowCount;
            }
        }
        else
        {
            b.rowCount = 1;
            if (explicitBase >= 0)
            {
                b.baseReg = explicitBase;
            }
            else
            {
                b.baseReg = nextVectorReg_;
                nextVectorReg_ -= 1;
            }
        }
        bindings_[name] = b;
    }

    int nextMatrixReg_ = 256;
    int nextVectorReg_ = 467;
    std::unordered_map<std::string, ConstBinding> bindings_;
};

// Source descriptor handed to the per-output MOV emit routine.  Either a
// vertex-attribute input or a const-bank entry.  Carries the value's
// vector width so the consumer can pick a writemask + swizzle that
// matches sce-cgc when one side is narrower than vec4 (e.g. float2
// texcoord passthrough emits `MOV o[7].xy, v[8].xyxx` — masked dest,
// last-lane-replicated source).
struct ValueSource
{
    enum class Kind { Input, Const };
    Kind kind     = Kind::Input;
    int  regIdx   = 0;  // input bank index (for Input) or absolute const index (for Const)
    int  width    = 4;  // 1..4 — number of meaningful lanes in the source
};

// Compute a NV40 VP writemask covering the first `width` lanes (X, XY,
// XYZ, XYZW).  Output stores narrower than vec4 use this so sce-cgc's
// `o[7].xy = ...` shape lands byte-exact instead of a full-mask MOV.
inline int writemaskForWidth(int width)
{
    switch (width)
    {
    case 1:  return NVFX_VP_MASK_X;
    case 2:  return NVFX_VP_MASK_X | NVFX_VP_MASK_Y;
    case 3:  return NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z;
    default: return NVFX_VP_MASK_ALL;
    }
}

// Build an identity-with-replicated-tail swizzle for a source whose
// meaningful width is `width` (1..4).  NV40 mandates a value in every
// swizzle slot; sce-cgc fills the tail with lane 0 (so a float2 reads
// as `.xyxx`, a float3 as `.xyzx`).  Matches the reference compiler's
// output for narrow-input passthroughs.
inline void identitySwizzleForWidth(int width, uint8_t out[4])
{
    out[0] = 0;
    out[1] = (width >= 2) ? 1 : 0;
    out[2] = (width >= 3) ? 2 : 0;
    out[3] = (width >= 4) ? 3 : 0;
}

// Records a deferred matrix×vector multiply — we emit the per-row dot
// products when the result value is consumed by a StoreOutput (matches
// sce-cgc which writes straight to the output register without a temp).
//
// Two opcode variants:
//   * DP4 (default) for `mul(matrix, float4)` — full 4-component dot.
//   * DPH for the `mul(matrix, float4(vec3, 1.0f))` pattern — dot-
//     product-homogeneous; treats source W as implicit 1.0, so we can
//     skip materialising a (vec3.x, vec3.y, vec3.z, 1) temp.  sce-cgc
//     emits this with a `.xyzx` swizzle on the input to put X in the
//     ignored W lane (NV40 quirk: source swizzle is mandatory; .xyzx
//     reuses an existing component rather than reading from past Z).
struct MatVecMulBinding;  // forward decl — full def below, ties to VecConstructBinding

// Records a `float4(vec3 input, scalar 1.0f)` expression — deferred so
// the consuming MatVecMul can fold it into a DPH (avoids the temp +
// MOV that PSL1GHT cgcomp emits for the same pattern).  If the result
// is consumed by anything OTHER than a MatVecMul, the lowering will
// error out (general VecConstruct support lands later).
struct VecPromoteBinding
{
    int inputAttrIdx = -1;  // the vec3 input's NV40 vertex-attribute index
};

// One lane of a `float4(...)` expression built from up-to-four scalars
// (or sub-vectors via VecShuffle).  sce-cgc lowers these to a sequence
// of partial-mask MOVs grouped by source register, or — when the
// constructor is consumed by a `mul(matrix, ...)` and the W lane is
// literal 1.0f — folded into a DPH chain that materialises only the
// XYZ lanes into a temp register.
struct LaneSource
{
    enum class Kind {
        Unknown,    // bail-out value
        InputLane,  // one lane of a vertex-attribute input (v[N].lane)
        ConstLane,  // one lane of a const-bank entry (c[N].lane)
        Literal,    // literal float; assigned to a literal-pool slot at use time
    };
    Kind  kind     = Kind::Unknown;
    int   regIdx   = 0;  // attribute index or const index, depending on kind
    int   srcLane  = 0;  // 0..3 source lane within regIdx (for InputLane / ConstLane)
    float litValue = 0.0f;
};

struct VecConstructBinding
{
    LaneSource lanes[4];   // one per output lane (X, Y, Z, W)
    int        width = 4;  // number of meaningful lanes (rest filled with default 0)
    bool       allInputLanes = false;       // true iff every meaningful lane is InputLane
    bool       wIsLiteralOne = false;       // shortcut for the DPH-fold detection
};

struct MatVecMulBinding
{
    int      matrixBaseReg  = -1;             // absolute const index of matrix row 0
    int      vectorInputIdx = -1;             // NV40 input-attribute or temp register index of the vec4 operand
    uint8_t  inputSwz[4]    = {0, 1, 2, 3};   // identity (xyzw) for DP4; xyzx for DPH
    uint8_t  vecOpcode      = 0;              // 0 = DP4 (default), set to DPH opcode for vec3+1 promotion
    bool     vectorIsTemp   = false;          // true → vectorInputIdx selects a TEMP, false → INPUT

    // When the vector operand is a VecConstruct that has to materialise
    // into a temp (e.g. sysmenu's `mul(M, vec4(in.x, in.y, 0, 1))`),
    // the build plan stays here.  The MOVs are emitted at consumption
    // time so that simple StoreOutputs (e.g. `out_color = in_color`)
    // emit BEFORE the matvecmul's temp build, matching the reference
    // compiler's "single instruction stores first" ordering.
    bool                hasTempBuild = false;
    VecConstructBinding tempBuild;
};

// Tracks the float literals seen during VP emit and packs them into
// const-bank registers (sce-cgc allocates these top-down from c[467],
// after every uniform vec4 has consumed its own slot).  Each unique
// literal value gets one lane; values are de-duplicated.  A vec4 slot
// holds up to four unique values — once full, the next allocation
// drops to c[N-1].  Two pre-baked layouts cover every shader in the
// current sample set:
//
//   - {0, 0, 0, 0}     →  one slot at c[N], values=[0]            (width 1)
//   - {0, 1, 0, 0}     →  one slot at c[N], values=[0, 1, 0, 0]   (width 2)
//
// The reference compiler always pads the pool slot's values[] to
// four entries (zero-filled) regardless of how many unique values it
// holds, and the container emit follows that layout.
class LiteralPool
{
public:
    explicit LiteralPool(int firstFreeReg) : nextReg_(firstFreeReg) {}

    // Returns (constReg, lane) for a given literal value, allocating
    // a new pool slot or lane as needed.  Used by VecConstruct lowering.
    std::pair<int, int> assign(float v)
    {
        // Fast-path within the current slot.
        if (!slots_.empty())
        {
            auto& back = slots_.back();
            for (uint32_t i = 0; i < back.usedLanes; ++i)
            {
                if (bitsEqual(back.values[i], v))
                    return { static_cast<int>(back.constReg), static_cast<int>(i) };
            }
            if (back.usedLanes < 4)
            {
                back.values[back.usedLanes] = v;
                const int lane = static_cast<int>(back.usedLanes);
                back.usedLanes += 1;
                return { static_cast<int>(back.constReg), lane };
            }
        }
        // No slot, or current slot full — allocate a fresh c[].
        VpLiteralPoolSlot s;
        s.constReg     = static_cast<uint32_t>(nextReg_--);
        s.values[0]    = v;
        s.usedLanes    = 1;
        slots_.push_back(s);
        return { static_cast<int>(s.constReg), 0 };
    }

    bool empty() const { return slots_.empty(); }
    const std::vector<VpLiteralPoolSlot>& slots() const { return slots_; }

private:
    static bool bitsEqual(float a, float b)
    {
        uint32_t pa, pb;
        std::memcpy(&pa, &a, sizeof(pa));
        std::memcpy(&pb, &b, sizeof(pb));
        return pa == pb;
    }
    int                            nextReg_;
    std::vector<VpLiteralPoolSlot> slots_;
};

// Scalar / vector arithmetic (Add, Sub, Mul) between two values.  Lowered
// to a single NV40 VEC op; if both operands come from the const bank we
// pre-move the second into a temp because NV40 allows only one const
// source per instruction (confirmed by sce-cgc on two_vec4_v.cg).
enum class ArithOp { Add, Sub, Mul };

struct ArithBinding
{
    ArithOp   op;
    IRValueID srcIds[2];
};

}  // namespace

// NV40 VP slot map: ADD / SUB fill src0 + src2, leaving src1 unused.
// MUL fills src0 + src1.  Matches PSL1GHT vpparser.cpp's spec
// (`{"ADD", OPCODE_ADD, {0, 2, -1}, ...}`) and sce-cgc output.
// Returned indices are into the 3-slot nvfx_insn::src array.
static inline void arithSlots(ArithOp op, int& slotA, int& slotB)
{
    switch (op)
    {
    case ArithOp::Mul: slotA = 0; slotB = 1; break;
    case ArithOp::Add:
    case ArithOp::Sub:
    default:           slotA = 0; slotB = 2; break;
    }
}

UcodeOutput lowerVertexProgram(const IRModule& module, const IRFunction& entry,
                               const rsx_cg::CompileOptions& /*opts*/,
                               VpAttributes* attrsOut)
{
    // opts is currently unused — every pass we'd gate on an opt level
    // (MAD fusion, dead-code removal, reg reuse) lives in future
    // stages.  Keep the parameter so callers thread it through today
    // and don't have to change signatures when those passes land.
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

    // MatVecMul results — emit the 4 dots once the consumer is known.
    std::unordered_map<IRValueID, MatVecMulBinding> valueToMatVecMul;

    // VecConstruct(vec3 input, scalar 1.0f) results — folded into DPH
    // by a consuming MatVecMul.
    std::unordered_map<IRValueID, VecPromoteBinding> valueToVecPromote;

    // VecShuffle(value, lane) — records that a value is a single-lane
    // extract of a wider source.  Used when classifying VecConstruct
    // operands ("float4(in.x, in.y, 0, 1)" → two InputLane lanes).
    struct ShuffleBinding {
        IRValueID srcId   = 0;
        int       lanes[4] = {0, 0, 0, 0};
        int       width  = 1;  // # of meaningful lanes (1 for scalar, 2 for vec2 swiz, etc.)
    };
    std::unordered_map<IRValueID, ShuffleBinding> valueToShuffle;

    // VecConstruct results — generic 4-scalar / mixed-input version.
    std::unordered_map<IRValueID, VecConstructBinding> valueToVecConstruct;

    // Temp register where a MatVecMul / VecConstruct result has been
    // materialised.  Set when a downstream consumer (chained MatVecMul,
    // etc.) needs the value as a source operand.  Read at consumption
    // time to choose between immediate-emit and temp-read.
    std::unordered_map<IRValueID, int> valueToTempReg;

    // Arithmetic operations awaiting a consumer.
    std::unordered_map<IRValueID, ArithBinding> valueToArith;

    // Literal-pool allocator — slots grow downward from the first const-
    // bank register left unclaimed by uniforms.  Drained into attrs at
    // the end of the function so cg_container_vp can emit the matching
    // `internal-constant-N` parameter table entries.
    int firstFreeConstReg = 467;
    for (const auto& g : module.globals)
    {
        if (g.storage == StorageQualifier::Uniform && !g.type.isMatrix())
        {
            if (g.explicitRegisterBank != 'C') --firstFreeConstReg;
        }
    }
    for (const auto& p : entry.parameters)
    {
        if (p.storage == StorageQualifier::Uniform && !p.type.isMatrix())
            --firstFreeConstReg;
    }
    LiteralPool literals(firstFreeConstReg);

    // Rolling temp register counter (R0, R1, ...).  No liveness analysis
    // yet — every dual-const operand allocates a fresh temp.  sce-cgc reuses
    // temps across instructions when operand lifetimes don't overlap; we'll
    // match that pattern-by-pattern as real shaders need it.
    int nextTempReg = 0;

    // Temps freed by chained MatVecMul lowering — each freed entry
    // becomes the next preferred allocation, which gives us the
    // R0 → R1 → R0 ping-pong sce-cgc emits.  Using a stack (LIFO)
    // matches the reference compiler exactly: the most-recently-
    // freed register is the most-recently-read, so reusing it
    // immediately keeps register pressure low.
    std::vector<int> availableTemps;
    auto allocTemp = [&]() -> int {
        if (!availableTemps.empty())
        {
            int t = availableTemps.back();
            availableTemps.pop_back();
            return t;
        }
        return nextTempReg++;
    };

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

    // Single-instruction StoreOutputs land in this queue first so we
    // can sort by source-attribute index before emitting — the
    // reference compiler emits multi-output passthrough VPs in
    // input-attribute order rather than source-statement order.
    // Same struct as DeferredStore for type uniformity.
    std::vector<DeferredStore> immediateStores;

    // Output-param vector width keyed by (semantic, semanticIndex) — used
    // by emitStoreOutput to pick a writemask narrower than .xyzw when the
    // declared output type is float2 / float3.  Built once at entry so
    // the back-end can resolve from a StoreOutput's semantic alone (the
    // IR's StoreOutput op carries no width / type info on its own).
    std::unordered_map<uint64_t, int> outputWidthBySemantic;
    auto semKey = [](const std::string& semUpper, int semIdx) -> uint64_t
    {
        // Hash the upper-cased semantic into the high bits and the index
        // into the low.  Cheap, collision-free for any realistic shader.
        uint64_t h = 1469598103934665603ull;
        for (char c : semUpper)
        {
            h ^= static_cast<uint8_t>(c);
            h *= 1099511628211ull;
        }
        return (h & ~0xFFFFull) | static_cast<uint64_t>(semIdx & 0xFFFF);
    };

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
                ValueSource vs;
                vs.kind   = ValueSource::Kind::Const;
                vs.regIdx = b->baseReg;
                vs.width  = std::max(1, param.type.vectorSize);
                valueToSource[param.valueId] = vs;
            }
        }
        else if (param.storage == StorageQualifier::Out ||
                 (param.storage == StorageQualifier::InOut &&
                  !param.semanticName.empty()))
        {
            // Record the declared width so StoreOutput can pick the
            // matching writemask (e.g. float2 oTexCoord → .xy mask).
            if (!param.semanticName.empty())
            {
                const std::string semUpper = toUpper(param.semanticName);
                outputWidthBySemantic[semKey(semUpper, param.semanticIndex)] =
                    std::max(1, param.type.vectorSize);
            }
        }

        if (param.storage == StorageQualifier::In ||
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
                    ValueSource vs;
                    vs.kind   = ValueSource::Kind::Input;
                    vs.regIdx = nv40Idx;
                    vs.width  = std::max(1, param.type.vectorSize);
                    valueToSource[param.valueId] = vs;
                }
            }
        }
    }

    bool emittedSomething = false;

    // Build an nvfx_reg for an IR value that was resolved to a simple
    // source (vertex input or const-bank slot).  Used when lowering an
    // arithmetic op at the point of consumption.
    auto regFromValue = [&](IRValueID id, bool& ok) -> struct nvfx_reg
    {
        auto it = valueToSource.find(id);
        if (it == valueToSource.end())
        {
            ok = false;
            return makeReg(NVFXSR_NONE, 0);
        }
        ok = true;
        return (it->second.kind == ValueSource::Kind::Input)
                   ? makeReg(NVFXSR_INPUT, it->second.regIdx)
                   : makeReg(NVFXSR_CONST, it->second.regIdx);
    };

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

        // Arithmetic: ADD / SUB / MUL between two values.  If both source
        // operands are const-bank entries, the second is pre-MOVed into a
        // temp — NV40 VP can read only one const per instruction.
        auto arIt = valueToArith.find(srcId);
        if (arIt != valueToArith.end())
        {
            bool okA = false, okB = false;
            struct nvfx_reg regA = regFromValue(arIt->second.srcIds[0], okA);
            struct nvfx_reg regB = regFromValue(arIt->second.srcIds[1], okB);
            if (!okA || !okB)
            {
                out.diagnostics.push_back(
                    "nv40-vp: arithmetic operand did not resolve to a simple source");
                return false;
            }

            if (regA.type == NVFXSR_CONST && regB.type == NVFXSR_CONST)
            {
                // MOV R_next, c[B] — preload second operand into a temp.
                const int tempIdx = nextTempReg++;
                const struct nvfx_reg tempReg = makeReg(NVFXSR_TEMP, tempIdx);

                struct nvfx_src mov_src0 = makeSrc(regB);
                struct nvfx_src mov_src1 = makeSrc(none);
                struct nvfx_src mov_src2 = makeSrc(none);
                struct nvfx_insn movInsn = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(tempReg),
                    NVFX_VP_MASK_ALL,
                    mov_src0, mov_src1, mov_src2);
                asm_.emit(movInsn, VP_OP(MOV));

                regB = tempReg;
            }

            // Slot assignment: ADD/SUB → src0+src2, MUL → src0+src1.
            int slotA = 0, slotB = 0;
            arithSlots(arIt->second.op, slotA, slotB);

            struct nvfx_src srcs[3] = {
                makeSrc(none), makeSrc(none), makeSrc(none) };
            srcs[slotA] = makeSrc(regA);
            srcs[slotB] = makeSrc(regB);

            struct nvfx_insn opInsn = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dstReg),
                NVFX_VP_MASK_ALL,
                srcs[0], srcs[1], srcs[2]);

            uint8_t opcode = VP_OP(ADD);
            if (arIt->second.op == ArithOp::Sub) opcode = VP_OP(ADD);  // Sub = ADD with neg
            else if (arIt->second.op == ArithOp::Mul) opcode = VP_OP(MUL);
            asm_.emit(opInsn, opcode);

            emittedSomething = true;
            return true;
        }

        auto mvIt = valueToMatVecMul.find(srcId);
        if (mvIt != valueToMatVecMul.end())
        {
            // If the matvecmul's vector operand was a deferred VecConstruct
            // build, emit the MOVs that fill the temp register first.
            // Resolves any remaining literal lanes through the pool.
            if (mvIt->second.hasTempBuild)
            {
                VecConstructBinding vb = mvIt->second.tempBuild;
                const int tempReg = mvIt->second.vectorInputIdx;
                const bool useDph = vb.wIsLiteralOne;
                const int  laneCount = useDph ? 3 : 4;

                // Resolve literal lanes to const-bank slots.
                for (int j = 0; j < laneCount; ++j)
                {
                    if (vb.lanes[j].kind == LaneSource::Kind::Literal)
                    {
                        auto [cReg, cLane] = literals.assign(vb.lanes[j].litValue);
                        vb.lanes[j].kind    = LaneSource::Kind::ConstLane;
                        vb.lanes[j].regIdx  = cReg;
                        vb.lanes[j].srcLane = cLane;
                    }
                }

                bool laneEmitted[4] = {false, false, false, false};
                for (int i = 0; i < laneCount; ++i)
                {
                    if (laneEmitted[i]) continue;
                    const LaneSource::Kind k = vb.lanes[i].kind;
                    const int regIdx         = vb.lanes[i].regIdx;

                    int mask = 0;
                    uint8_t swz[4] = {0, 0, 0, 0};
                    int firstMatchLane = -1;
                    for (int j = 0; j < laneCount; ++j)
                    {
                        if (vb.lanes[j].kind == k && vb.lanes[j].regIdx == regIdx)
                        {
                            if (firstMatchLane < 0) firstMatchLane = j;
                            mask |= (8 >> j);
                            swz[j] = static_cast<uint8_t>(vb.lanes[j].srcLane);
                            laneEmitted[j] = true;
                        }
                    }
                    // Pad out-of-group lanes with the first match's swizzle.
                    for (int j = 0; j < 4; ++j)
                    {
                        if (!(mask & (8 >> j)))
                        {
                            swz[j] = swz[firstMatchLane >= 0 ? firstMatchLane : 0];
                        }
                    }

                    const struct nvfx_reg src = (k == LaneSource::Kind::InputLane)
                        ? makeReg(NVFXSR_INPUT, regIdx)
                        : makeReg(NVFXSR_CONST, regIdx);
                    struct nvfx_src s0 = makeSrc(src);
                    s0.swz[0] = swz[0];
                    s0.swz[1] = swz[1];
                    s0.swz[2] = swz[2];
                    s0.swz[3] = swz[3];

                    const struct nvfx_reg tempRegN = makeReg(NVFXSR_TEMP, tempReg);
                    struct nvfx_insn movInsn = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempRegN),
                        mask,
                        s0, makeSrc(none), makeSrc(none));
                    asm_.emit(movInsn, VP_OP(MOV));
                }
            }

            const struct nvfx_reg vecReg =
                mvIt->second.vectorIsTemp
                    ? makeReg(NVFXSR_TEMP, mvIt->second.vectorInputIdx)
                    : makeReg(NVFXSR_INPUT, mvIt->second.vectorInputIdx);
            const uint8_t opcode =
                mvIt->second.vecOpcode != 0
                    ? mvIt->second.vecOpcode
                    : static_cast<uint8_t>(VP_OP(DP4));
            for (int i = 0; i < 4; ++i)
            {
                const int rowReg =
                    mvIt->second.matrixBaseReg + kDp4RowOffset[i];
                const struct nvfx_reg rowConst = makeReg(NVFXSR_CONST, rowReg);

                struct nvfx_src src0 = makeSrc(vecReg);
                src0.swz[0] = mvIt->second.inputSwz[0];
                src0.swz[1] = mvIt->second.inputSwz[1];
                src0.swz[2] = mvIt->second.inputSwz[2];
                src0.swz[3] = mvIt->second.inputSwz[3];
                struct nvfx_src src1 = makeSrc(rowConst);
                struct nvfx_src src2 = makeSrc(none);

                struct nvfx_insn in = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dstReg),
                    kDp4Writemask[i],
                    src0, src1, src2);
                asm_.emit(in, opcode);
            }
            emittedSomething = true;
            return true;
        }

        // VecConstruct(...) → StoreOutput: emit one MOV per source
        // register (input attr or const-bank slot) with a writemask
        // that covers only the lanes pulled from that source.  This
        // is the canonical sce-cgc shape for `out = float4(in.x,
        // in.y, 0, 1)` (two MOVs: input-fed lanes then literal lanes).
        auto vcIt = valueToVecConstruct.find(srcId);
        if (vcIt != valueToVecConstruct.end())
        {
            VecConstructBinding vb = vcIt->second;

            // Resolve every Literal lane to a const-bank slot up front
            // so the grouping logic below has a uniform LaneSource view
            // (InputLane vs ConstLane).
            for (int i = 0; i < 4; ++i)
            {
                if (vb.lanes[i].kind == LaneSource::Kind::Literal)
                {
                    auto [cReg, cLane] = literals.assign(vb.lanes[i].litValue);
                    vb.lanes[i].kind    = LaneSource::Kind::ConstLane;
                    vb.lanes[i].regIdx  = cReg;
                    vb.lanes[i].srcLane = cLane;
                }
            }

            // Walk lanes; group adjacent lanes that share (kind, regIdx)
            // into a single MOV with a partial writemask.  The output
            // declared width caps the lane count (a float2 oTexCoord
            // sourced from a 4-lane VecConstruct still writes only .xy).
            const int outDeclWidth = [&]() {
                auto wIt = outputWidthBySemantic.find(semKey(semUpper, semanticIndex));
                return (wIt == outputWidthBySemantic.end()) ? 4 : wIt->second;
            }();
            const int laneCount = std::min(vb.width, outDeclWidth);

            bool laneEmitted[4] = {false, false, false, false};
            for (int i = 0; i < laneCount; ++i)
            {
                if (laneEmitted[i]) continue;
                if (vb.lanes[i].kind == LaneSource::Kind::Unknown)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: VecConstruct lane has unknown source");
                    return false;
                }
                const LaneSource::Kind k = vb.lanes[i].kind;
                const int regIdx         = vb.lanes[i].regIdx;

                // Build mask + per-lane swizzle for every lane in this
                // (kind, regIdx) group.  Lanes outside the group still
                // need a swizzle slot (NV40 mandates .swz[0..3]); fill
                // them by replicating the first matching source-lane,
                // matching the reference compiler's `xxxy` / `xyxx`
                // padding pattern.
                int mask = 0;
                uint8_t swz[4] = {0, 0, 0, 0};
                int firstMatchLane = -1;
                for (int j = 0; j < 4; ++j)
                {
                    if (j < laneCount &&
                        vb.lanes[j].kind == k &&
                        vb.lanes[j].regIdx == regIdx)
                    {
                        if (firstMatchLane < 0) firstMatchLane = j;
                        mask |= (8 >> j);  // X=8, Y=4, Z=2, W=1
                        swz[j] = static_cast<uint8_t>(vb.lanes[j].srcLane);
                        laneEmitted[j] = true;
                    }
                }
                // Pad out-of-group lanes with the first match's
                // source lane (sce-cgc default).
                for (int j = 0; j < 4; ++j)
                {
                    if (!(mask & (8 >> j)))
                    {
                        swz[j] = swz[firstMatchLane >= 0 ? firstMatchLane : 0];
                    }
                }

                const struct nvfx_reg src = (k == LaneSource::Kind::InputLane)
                    ? makeReg(NVFXSR_INPUT, regIdx)
                    : makeReg(NVFXSR_CONST, regIdx);
                struct nvfx_src s0 = makeSrc(src);
                s0.swz[0] = swz[0];
                s0.swz[1] = swz[1];
                s0.swz[2] = swz[2];
                s0.swz[3] = swz[3];

                struct nvfx_insn mov = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dstReg),
                    mask,
                    s0, makeSrc(none), makeSrc(none));
                asm_.emit(mov, VP_OP(MOV));
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

        // Narrower-than-vec4 passthroughs: clamp the destination
        // writemask to the declared output width and replicate the
        // tail of the source swizzle from lane 0 (e.g. float2 → .xy
        // mask + .xyxx swizzle).  sce-cgc does the same.  Width info
        // comes from the input's recorded ValueSource width and the
        // output param's declared vectorSize.
        const int outWidth = [&]() {
            auto wIt = outputWidthBySemantic.find(semKey(semUpper, semanticIndex));
            return (wIt == outputWidthBySemantic.end()) ? 4 : wIt->second;
        }();
        const int srcWidth = it->second.width;
        const int copyWidth = std::min(outWidth, srcWidth);
        if (srcWidth < 4)
        {
            uint8_t s[4];
            identitySwizzleForWidth(srcWidth, s);
            src0.swz[0] = s[0];
            src0.swz[1] = s[1];
            src0.swz[2] = s[2];
            src0.swz[3] = s[3];
        }

        struct nvfx_insn in = nvfx_insn(
            0, 0, -1, -1,
            const_cast<struct nvfx_reg&>(dstReg),
            writemaskForWidth(copyWidth),
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

            case IROp::Add:
            case IROp::Sub:
            case IROp::Mul:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-vp: arithmetic op with <2 operands");
                    return out;
                }
                ArithBinding a;
                a.op = (inst.op == IROp::Add) ? ArithOp::Add
                     : (inst.op == IROp::Sub) ? ArithOp::Sub
                                              : ArithOp::Mul;
                a.srcIds[0] = inst.operands[0];
                a.srcIds[1] = inst.operands[1];
                valueToArith[inst.result] = a;
                break;
            }

            case IROp::VecShuffle:
            {
                // Single-lane extract from a wider value.  We treat any
                // shuffle as a record-keeping op: the consumer (a
                // VecConstruct, an Add, a Sub, a StoreOutput) decodes the
                // source + lane when it actually emits hardware.
                if (inst.operands.empty()) break;
                ShuffleBinding sb;
                sb.srcId = inst.operands[0];
                sb.width = std::max(1, inst.resultType.vectorSize);
                for (int i = 0; i < 4; ++i)
                    sb.lanes[i] = (inst.swizzleMask >> (i * 2)) & 3;
                valueToShuffle[inst.result] = sb;
                break;
            }

            case IROp::VecConstruct:
            {
                // Two recognised shapes:
                //   1. `float4(vec3 input, 1.0f)` — fast-path DPH fold
                //      handled the same way as before via VecPromote.
                //   2. up-to-4 scalar lanes from inputs / constants /
                //      literals — recorded as a generic VecConstructBinding
                //      so the consumer (MatVecMul or StoreOutput) can
                //      emit the right hardware sequence.
                //
                // Note: shape 1 is a strict subset of shape 2, but we
                // keep VecPromoteBinding around because the existing
                // DPH path is byte-exact and well-tested — switching
                // those shaders to the generic path is a separate
                // refactor.

                // Shape 1: `float4(vec3 input, 1.0f)` →  VecPromoteBinding.
                if (inst.operands.size() == 2)
                {
                    auto inputIt = valueToSource.find(inst.operands[0]);
                    if (inputIt != valueToSource.end() &&
                        inputIt->second.kind == ValueSource::Kind::Input)
                    {
                        IRValue* v = entry.getValue(inst.operands[1]);
                        auto* c = dynamic_cast<IRConstant*>(v);
                        if (c &&
                            std::holds_alternative<float>(c->value) &&
                            std::get<float>(c->value) == 1.0f)
                        {
                            VecPromoteBinding pb;
                            pb.inputAttrIdx = inputIt->second.regIdx;
                            valueToVecPromote[inst.result] = pb;
                            break;
                        }
                    }
                }

                // Shape 2: up-to-4 scalar lanes.  Classify each operand;
                // bail out (skip recording) if any lane can't be resolved
                // to a known source — the StoreOutput/MatVecMul consumer
                // will then fall through to its existing diagnostic.
                if (inst.operands.empty() || inst.operands.size() > 4) break;

                VecConstructBinding vb;
                vb.width = static_cast<int>(inst.operands.size());
                bool ok = true;
                bool allInputLanes = true;
                for (size_t i = 0; i < inst.operands.size(); ++i)
                {
                    LaneSource ls;
                    const IRValueID id = inst.operands[i];

                    // Direct input attribute reference (scalar input
                    // type — uncommon but legal for `float in.x`).
                    auto srcIt = valueToSource.find(id);
                    if (srcIt != valueToSource.end() &&
                        srcIt->second.kind == ValueSource::Kind::Input)
                    {
                        ls.kind    = LaneSource::Kind::InputLane;
                        ls.regIdx  = srcIt->second.regIdx;
                        ls.srcLane = 0;
                        vb.lanes[i] = ls;
                        continue;
                    }

                    // VecShuffle of a known input → InputLane(srcLane).
                    auto shIt = valueToShuffle.find(id);
                    if (shIt != valueToShuffle.end())
                    {
                        auto inputIt = valueToSource.find(shIt->second.srcId);
                        if (inputIt != valueToSource.end() &&
                            inputIt->second.kind == ValueSource::Kind::Input)
                        {
                            ls.kind    = LaneSource::Kind::InputLane;
                            ls.regIdx  = inputIt->second.regIdx;
                            ls.srcLane = shIt->second.lanes[0];
                            vb.lanes[i] = ls;
                            continue;
                        }
                        if (inputIt != valueToSource.end() &&
                            inputIt->second.kind == ValueSource::Kind::Const)
                        {
                            ls.kind    = LaneSource::Kind::ConstLane;
                            ls.regIdx  = inputIt->second.regIdx;
                            ls.srcLane = shIt->second.lanes[0];
                            vb.lanes[i] = ls;
                            allInputLanes = false;
                            continue;
                        }
                    }

                    // Float literal → enqueue into the literal pool at
                    // emit time.  Stash the value here; the consumer
                    // resolves it via literals.assign(value).
                    IRValue* v = entry.getValue(id);
                    auto* c = dynamic_cast<IRConstant*>(v);
                    if (c && std::holds_alternative<float>(c->value))
                    {
                        ls.kind     = LaneSource::Kind::Literal;
                        ls.litValue = std::get<float>(c->value);
                        vb.lanes[i] = ls;
                        allInputLanes = false;
                        continue;
                    }

                    // Anything else: bail out for this VecConstruct.
                    ok = false;
                    break;
                }
                if (!ok) break;

                // W lane = literal 1.0f → DPH-fold opportunity for a
                // consuming MatVecMul.  Only meaningful when width=4.
                if (vb.width == 4 &&
                    vb.lanes[3].kind == LaneSource::Kind::Literal &&
                    vb.lanes[3].litValue == 1.0f)
                {
                    vb.wIsLiteralOne = true;
                }
                vb.allInputLanes = allInputLanes;

                // Pre-populate the literal pool with every distinct
                // value declared in the constructor — even ones that
                // a downstream DPH will consume implicitly (the W=1.0
                // case).  sce-cgc lists all distinct source literals
                // in the param table regardless of ucode consumption,
                // so the pool entry's `usedLanes` count must match the
                // source.
                for (int i = 0; i < vb.width; ++i)
                {
                    if (vb.lanes[i].kind == LaneSource::Kind::Literal)
                        literals.assign(vb.lanes[i].litValue);
                }

                valueToVecConstruct[inst.result] = vb;
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

                MatVecMulBinding binding;
                binding.matrixBaseReg = matIt->second;

                // DPH path #1: vector operand is a vec3+1.0 promotion.
                auto promoIt = valueToVecPromote.find(inst.operands[1]);
                if (promoIt != valueToVecPromote.end())
                {
                    binding.vectorInputIdx = promoIt->second.inputAttrIdx;
                    binding.vecOpcode      = static_cast<uint8_t>(VP_OP(DPH));
                    binding.inputSwz[0] = 0;
                    binding.inputSwz[1] = 1;
                    binding.inputSwz[2] = 2;
                    binding.inputSwz[3] = 0;
                    valueToMatVecMul[inst.result] = binding;
                    break;
                }

                // DPH path #2 / DP4 path #2: vector operand is a generic
                // 4-scalar VecConstruct that we'll lower to a temp register
                // at consumption time, followed by 4 DP4 / DPH ops against
                // the temp.  Defer the temp-build emit (the MOVs that fill
                // the temp from input/literal lanes) until the consuming
                // StoreOutput runs — that lets simple co-located stores
                // (e.g. `out_color = in_color`) emit first, matching the
                // reference compiler's instruction ordering.
                auto vcIt = valueToVecConstruct.find(inst.operands[1]);
                if (vcIt != valueToVecConstruct.end() && vcIt->second.width == 4)
                {
                    const VecConstructBinding& vb = vcIt->second;
                    const bool useDph = vb.wIsLiteralOne;

                    // Reserve a temp register slot up-front so the
                    // matvecmul binding has a final source operand.
                    // The actual MOVs land at emit time.
                    const int tempReg = allocTemp();

                    binding.vectorInputIdx = tempReg;
                    binding.vectorIsTemp   = true;
                    binding.vecOpcode      = useDph
                        ? static_cast<uint8_t>(VP_OP(DPH))
                        : static_cast<uint8_t>(VP_OP(DP4));
                    if (useDph)
                    {
                        binding.inputSwz[0] = 0;
                        binding.inputSwz[1] = 1;
                        binding.inputSwz[2] = 2;
                        binding.inputSwz[3] = 0;
                    }
                    binding.hasTempBuild = true;
                    binding.tempBuild    = vb;
                    valueToMatVecMul[inst.result] = binding;
                    break;
                }

                // DP4 path #1: vector operand is a direct vertex input.
                auto vecIt = valueToSource.find(inst.operands[1]);
                if (vecIt != valueToSource.end() &&
                    vecIt->second.kind == ValueSource::Kind::Input)
                {
                    binding.vectorInputIdx = vecIt->second.regIdx;
                    valueToMatVecMul[inst.result] = binding;
                    break;
                }

                // DP4 path #3: vector operand is a previous MatVecMul's
                // result — chained matrix-vector multiplies.  Force the
                // earlier result to materialise into a temp register,
                // then read from that temp.
                auto chainIt = valueToMatVecMul.find(inst.operands[1]);
                if (chainIt != valueToMatVecMul.end())
                {
                    int tempReg;
                    auto trIt = valueToTempReg.find(inst.operands[1]);
                    if (trIt != valueToTempReg.end())
                    {
                        tempReg = trIt->second;
                    }
                    else
                    {
                        tempReg = allocTemp();
                        valueToTempReg[inst.operands[1]] = tempReg;

                        // Emit the prior MatVecMul into tempReg now.
                        // Same 4-DP4/DPH unrolling as emitStoreOutput,
                        // but with a temp dst instead of an output dst.
                        const MatVecMulBinding& prev = chainIt->second;
                        const struct nvfx_reg noneR  = makeReg(NVFXSR_NONE, 0);
                        const struct nvfx_reg dstTemp = makeReg(NVFXSR_TEMP, tempReg);
                        const struct nvfx_reg vecSrc =
                            prev.vectorIsTemp
                                ? makeReg(NVFXSR_TEMP, prev.vectorInputIdx)
                                : makeReg(NVFXSR_INPUT, prev.vectorInputIdx);
                        const uint8_t opcode =
                            prev.vecOpcode != 0
                                ? prev.vecOpcode
                                : static_cast<uint8_t>(VP_OP(DP4));
                        for (int i = 0; i < 4; ++i)
                        {
                            const int rowReg =
                                prev.matrixBaseReg + kDp4RowOffset[i];
                            const struct nvfx_reg rowConst =
                                makeReg(NVFXSR_CONST, rowReg);
                            struct nvfx_src s0 = makeSrc(vecSrc);
                            s0.swz[0] = prev.inputSwz[0];
                            s0.swz[1] = prev.inputSwz[1];
                            s0.swz[2] = prev.inputSwz[2];
                            s0.swz[3] = prev.inputSwz[3];
                            struct nvfx_src s1 = makeSrc(rowConst);
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstTemp),
                                kDp4Writemask[i],
                                s0, s1, makeSrc(noneR));
                            asm_.emit(in, opcode);
                        }
                        // After consuming the prior MatVecMul's vector
                        // source, the temp it lived in is dead — recycle
                        // for the next chain step.  This is what gives
                        // us the R0 → R1 → R0 ping-pong instead of the
                        // naive R0 → R1 → R2 cascade.  Skip recycling
                        // when the source was a vertex input (M0 case).
                        if (prev.vectorIsTemp)
                            availableTemps.push_back(prev.vectorInputIdx);

                        // The prior MatVecMul has now been emitted; we
                        // remove its `valueToMatVecMul` record so a
                        // downstream StoreOutput reading it picks up the
                        // temp instead of triggering a re-emit.
                        valueToMatVecMul.erase(chainIt);
                    }
                    binding.vectorInputIdx = tempReg;
                    binding.vectorIsTemp   = true;
                    binding.vecOpcode      = 0;  // DP4 (default)
                    valueToMatVecMul[inst.result] = binding;
                    break;
                }

                out.diagnostics.push_back(
                    "nv40-vp: MatVecMul vector operand must be a vertex input, "
                    "a float4(vec3, 1.0f) promotion, a float4(...) constructor, "
                    "or a previous MatVecMul result");
                return out;
            }

            case IROp::StoreOutput:
            {
                if (inst.operands.empty())
                {
                    out.diagnostics.push_back("nv40-vp: StoreOutput with no operand");
                    return out;
                }

                const IRValueID srcId = inst.operands[0];

                // Multi-instruction expansions (matvecmul, arithmetic needing a
                // temp) are deferred until after all single-instruction
                // StoreOutputs have been emitted.
                if (valueToMatVecMul.count(srcId) || valueToArith.count(srcId))
                {
                    deferredStores.push_back(
                        { srcId, inst.semanticName, inst.semanticIndex });
                    break;
                }

                // Single-insn StoreOutputs are buffered and emitted
                // sorted by output-register index ascending (matches
                // the reference compiler's instruction ordering for
                // shaders with multiple passthrough outputs).
                immediateStores.push_back(
                    { srcId, inst.semanticName, inst.semanticIndex });
                break;
            }

            case IROp::Return:
                break;

            default:
                out.diagnostics.push_back(
                    "nv40-vp: unsupported IR op (handles MOV from attr/uniform only)");
                return out;
            }
        }
    }

    // Sort single-insn StoreOutputs by the *source's* primary input
    // attribute index ascending — the reference compiler emits
    // passthrough output writes in input-attribute order rather than
    // source-statement order.  Stores whose source is a VecConstruct
    // pulled from an attribute (e.g. `float4(pos.xy, 0, 1)`) inherit
    // that attribute's index.  Stores with no traceable input-attr
    // source fall back to output-semantic index +1024 (sorting after
    // attribute-sourced ones).
    std::function<int(IRValueID)> attrIndexFromValue = [&](IRValueID id) -> int
    {
        auto vsIt = valueToSource.find(id);
        if (vsIt != valueToSource.end() &&
            vsIt->second.kind == ValueSource::Kind::Input)
        {
            return vsIt->second.regIdx;
        }
        auto vcIt = valueToVecConstruct.find(id);
        if (vcIt != valueToVecConstruct.end())
        {
            for (int li = 0; li < vcIt->second.width; ++li)
            {
                if (vcIt->second.lanes[li].kind == LaneSource::Kind::InputLane)
                    return vcIt->second.lanes[li].regIdx;
            }
        }
        auto shIt = valueToShuffle.find(id);
        if (shIt != valueToShuffle.end())
            return attrIndexFromValue(shIt->second.srcId);
        return -1;
    };
    auto storeSortKey = [&](const DeferredStore& s) -> int
    {
        int attr = attrIndexFromValue(s.srcId);
        if (attr >= 0) return attr;
        return 1024 + vertexOutputIndex(toUpper(s.semanticName), s.semanticIndex);
    };
    std::stable_sort(immediateStores.begin(), immediateStores.end(),
        [&](const DeferredStore& a, const DeferredStore& b) {
            return storeSortKey(a) < storeSortKey(b);
        });

    for (const auto& imm : immediateStores)
    {
        if (!emitStoreOutput(imm.srcId, imm.semanticName, imm.semanticIndex))
            return out;
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

    if (attrsOut)
    {
        VpAttributes attrs;
        attrs.attributeInputMask  = asm_.inputMask();
        attrs.attributeOutputMask = asm_.outputMask();
        const int regs = asm_.numTempRegs();
        attrs.registerCount = regs > 0 ? static_cast<uint32_t>(regs) : 1u;
        // instructionSlot / userClipMask remain at sce-cgc defaults (0).
        attrs.literalPool = literals.slots();
        *attrsOut = attrs;
    }
    return out;
}

}  // namespace nv40::detail
