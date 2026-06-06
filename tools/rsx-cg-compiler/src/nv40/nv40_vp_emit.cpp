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
#include <unordered_set>
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
    if (semanticUpper.empty() && semanticIndex >= 0 && semanticIndex < 16)
        return semanticIndex;
    if (semanticUpper == "POSITION") return NVFX_VP_INST_IN_POS;
    if (semanticUpper == "NORMAL")   return NVFX_VP_INST_IN_NORMAL;
    if (semanticUpper == "COLOR")    return semanticIndex == 1
                                         ? NVFX_VP_INST_IN_COL1
                                         : NVFX_VP_INST_IN_COL0;
    if (semanticUpper == "TEXCOORD") return NVFX_VP_INST_IN_TC(semanticIndex);
    if (semanticUpper == "FOG")      return NVFX_VP_INST_IN_FOGC;
    if (semanticUpper == "BLENDWEIGHT" || semanticUpper == "WEIGHT")
        return NVFX_VP_INST_IN_WEIGHT;
    if (semanticUpper == "BLENDINDICES" || semanticUpper == "INDICES")
        return 7;
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
#define VP_SCA_OP(NAME) ((NVFX_VP_INST_SLOT_SCA << 7) | NVFX_VP_INST_SCA_OP_##NAME)

// the reference compiler emits the 4 DP4s in the order W, Z, Y, X, pulling matrix rows
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
// the reference compiler grows matrices up from c[256] and scalars/vectors down from c[467].
struct ConstBinding
{
    int baseReg  = -1;  // absolute const-bank index (256..467)
    int rowCount = 1;   // 1 for vec4, N for matrix rows
};

class ConstAllocator
{
public:
    // The reference compiler allocates in declaration order.  Both top-level `uniform`
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
// matches the reference compiler when one side is narrower than vec4 (e.g. float2
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
// XYZ, XYZW).  Output stores narrower than vec4 use this so the reference compiler's
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
// swizzle slot; the reference compiler fills the tail with lane 0 (so a float2 reads
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
// the reference compiler which writes straight to the output register without a temp).
//
// Two opcode variants:
//   * DP4 (default) for `mul(matrix, float4)` — full 4-component dot.
//   * DPH for the `mul(matrix, float4(vec3, 1.0f))` pattern — dot-
//     product-homogeneous; treats source W as implicit 1.0, so we can
//     skip materialising a (vec3.x, vec3.y, vec3.z, 1) temp.  the reference compiler
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
// (or sub-vectors via VecShuffle).  the reference compiler lowers these to a sequence
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

    // `mul(biasMatrix, uniformMatrix)` where biasMatrix is the fixed
    // light-projection bias matrix used by the Skin CgTutorial sample.
    // The uniform matvec is emitted first, then the bias transform is
    // applied from the resulting temp into the final output.
    bool     postBiasHalf   = false;
    float    biasHalf       = 0.5f;

    // Chained matrix×vector: when the vector operand is itself a
    // previous MatVecMul's result, `priorChainId` points to that prior
    // step's IR value.  The reference compiler emits chained matvecmuls
    // back-to-back when there's no other store to interleave with, but
    // splits them into per-step batches when an out_color store needs
    // to be interleaved.  We always defer the chain emit until flush
    // so the interleave path can plan batches uniformly.
    IRValueID priorChainId = 0;               // 0 → not a chain step

    // When the vector operand is a VecConstruct that has to materialise
    // into a temp (e.g. sysmenu's `mul(M, vec4(in.x, in.y, 0, 1))`),
    // the build plan stays here.  The MOVs are emitted at consumption
    // time so that simple StoreOutputs (e.g. `out_color = in_color`)
    // emit BEFORE the matvecmul's temp build, matching the reference
    // compiler's "single instruction stores first" ordering.
    bool                hasTempBuild = false;
    VecConstructBinding tempBuild;

    // Generic arithmetic roots, such as `(input + uniform)` feeding a
    // later MatVecMul, materialise into the reserved temp at consumption
    // time.  This keeps the exact VecConstruct/DPH paths above intact.
    IRValueID genericTempBuildId = 0;

    bool      matrixIsIndexed = false;
    IRValueID matrixIndexId   = 0;
    int       matrixRowCount  = 4;
};

struct BiasMatrixProductBinding
{
    int   matrixBaseReg = -1;
    float half = 0.5f;
};

struct IndexedMatrixBinding
{
    int       baseReg = -1;
    IRValueID indexId = 0;
    int       rowCount = 4;
};

// Tracks the float literals seen during VP emit and packs them into
// const-bank registers (the reference compiler allocates these top-down from c[467],
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
// source per instruction (confirmed by the reference compiler on two_vec4_v.cg).
enum class ArithOp { Add, Sub, Mul, Mad, Min, Max, Dot3, Dot4 };

struct ArithBinding
{
    ArithOp   op;
    IRValueID srcIds[3] = {0, 0, 0};
    int       width = 4;
};

struct GenericVecConstructBinding
{
    IRValueID lanes[4] = {0, 0, 0, 0};
    int       width = 4;
};

}  // namespace

// NV40 VP slot map: ADD / SUB fill src0 + src2, leaving src1 unused.
// MUL fills src0 + src1.  Matches PSL1GHT vpparser.cpp's spec
// (`{"ADD", OPCODE_ADD, {0, 2, -1}, ...}`) and the reference compiler output.
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

    // GpuSkinning's palette is represented by the front-end as a scalar
    // uniform plus `extract mat4 palette, index`.  Track that array base
    // separately so the backend can emit ARL/relative-constant accesses.
    std::unordered_map<IRValueID, int> valueToPaletteUniformBase;
    std::unordered_map<IRValueID, IndexedMatrixBinding> valueToIndexedMatrix;

    // Fixed light-projection bias matrix products.  This keeps the Skin
    // FaceShadow shader on a narrow production path instead of enabling
    // arbitrary matrix values before the backend has a general matrix IR.
    std::unordered_map<IRValueID, float> valueToBiasMatrix;
    std::unordered_map<IRValueID, BiasMatrixProductBinding>
        valueToBiasMatrixProduct;

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

    // VecInsert: `c.lane = scalar;` overrides one lane of a previously
    // computed vec value.  Recorded for each link in a VecInsert chain;
    // StoreOutput resolves the chain by walking inward via baseVecId
    // until a non-VecInsert source is reached.
    struct VecInsertBinding
    {
        IRValueID baseVecId = 0;   // vector being inserted into
        IRValueID scalarId  = 0;   // scalar being inserted at lane
        int       laneIndex = 0;   // 0..3 — destination lane
    };
    std::unordered_map<IRValueID, VecInsertBinding> valueToVecInsert;

    // Temp register where a MatVecMul / VecConstruct result has been
    // materialised.  Set when a downstream consumer (chained MatVecMul,
    // etc.) needs the value as a source operand.  Read at consumption
    // time to choose between immediate-emit and temp-read.
    std::unordered_map<IRValueID, int> valueToTempReg;

    // Arithmetic operations awaiting a consumer.
    std::unordered_map<IRValueID, ArithBinding> valueToArith;

    struct VpNormalizeBinding
    {
        IRValueID sourceId = 0;
        int       width = 3;
    };
    std::unordered_map<IRValueID, VpNormalizeBinding> valueToNormalize;

    struct VpPowBinding
    {
        IRValueID baseId = 0;
        float     exponent = 0.0f;
    };
    std::unordered_map<IRValueID, VpPowBinding> valueToPow;

    struct VpCmpBinding
    {
        IRValueID lhsId = 0;
        IRValueID rhsId = 0;
        IROp      op = IROp::Nop;
    };
    std::unordered_map<IRValueID, VpCmpBinding> valueToCmp;

    struct VpSelectBinding
    {
        IRValueID cmpId = 0;
        IRValueID trueId = 0;
        IRValueID falseId = 0;
    };
    std::unordered_map<IRValueID, VpSelectBinding> valueToSelect;

    // Generic VecConstruct values that contain computed scalar lanes
    // (e.g. float4(dot(...), dot(...), dot(...), 1.0)).  Existing
    // VecConstructBinding recognizers stay ahead of this fallback.
    std::unordered_map<IRValueID, GenericVecConstructBinding>
        valueToGenericVecConstruct;

    // Literal-pool allocator — slots grow downward from the first const-
    // bank register left unclaimed by uniforms.  Drained into attrs at
    // the end of the function so cg_container_vp can emit the matching
    // `internal-constant-N` parameter table entries.
    int firstFreeConstReg = 467;
    for (const auto& g : module.globals)
    {
        if (g.name == "gBlendMatrices") continue;
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
    // yet — every dual-const operand allocates a fresh temp.  the reference compiler reuses
    // temps across instructions when operand lifetimes don't overlap; we'll
    // match that pattern-by-pattern as real shaders need it.
    int nextTempReg = 0;

    // Temps freed by chained MatVecMul lowering — each freed entry
    // becomes the next preferred allocation, which gives us the
    // R0 → R1 → R0 ping-pong the reference compiler emits.  Using a stack (LIFO)
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

    // Deferred store-outputs: the reference compiler emits single-instruction StoreOutputs
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

    auto valueWidth = [&](IRValueID id) -> int
    {
        auto arIt = valueToArith.find(id);
        if (arIt != valueToArith.end())
            return arIt->second.width;
        auto gvIt = valueToGenericVecConstruct.find(id);
        if (gvIt != valueToGenericVecConstruct.end())
            return gvIt->second.width;
        auto vsIt = valueToSource.find(id);
        if (vsIt != valueToSource.end())
            return vsIt->second.width;
        auto shIt = valueToShuffle.find(id);
        if (shIt != valueToShuffle.end())
            return shIt->second.width;
        if (IRValue* v = entry.getValue(id))
            return std::max(1, v->type.vectorSize);
        return 4;
    };

    auto sourceValueWidth = [&](IRValueID id) -> int
    {
        auto vsIt = valueToSource.find(id);
        if (vsIt != valueToSource.end())
            return vsIt->second.width;
        auto shIt = valueToShuffle.find(id);
        if (shIt != valueToShuffle.end())
            return shIt->second.width;
        return valueWidth(id);
    };

    auto isFloatLiteral = [&](IRValueID id, float& v) -> bool
    {
        IRValue* irv = entry.getValue(id);
        auto* c = dynamic_cast<IRConstant*>(irv);
        if (!c) return false;
        if (std::holds_alternative<float>(c->value))
        {
            v = std::get<float>(c->value);
            return true;
        }
        if (std::holds_alternative<int32_t>(c->value))
        {
            v = static_cast<float>(std::get<int32_t>(c->value));
            return true;
        }
        if (std::holds_alternative<uint32_t>(c->value))
        {
            v = static_cast<float>(std::get<uint32_t>(c->value));
            return true;
        }
        return false;
    };

    auto isLiteralEqual = [&](IRValueID id, float expected) -> bool
    {
        float v = 0.0f;
        return isFloatLiteral(id, v) && v == expected;
    };

    auto extractBiasMatrixHalf =
        [&](const IRInstruction& inst, float& half) -> bool
    {
        if (inst.operands.size() != 16) return false;

        float h = 0.0f;
        if (!isFloatLiteral(inst.operands[0], h)) return false;
        if (h == 0.0f) return false;

        const int halfLanes[] = {0, 3, 5, 7, 10, 11};
        const int zeroLanes[] = {1, 2, 4, 6, 8, 9, 12, 13, 14};
        for (int lane : halfLanes)
        {
            float v = 0.0f;
            if (!isFloatLiteral(inst.operands[lane], v) || v != h)
                return false;
        }
        for (int lane : zeroLanes)
        {
            if (!isLiteralEqual(inst.operands[lane], 0.0f))
                return false;
        }
        if (!isLiteralEqual(inst.operands[15], 1.0f))
            return false;

        half = h;
        return true;
    };

    struct GenericSource
    {
        bool valid = false;
        struct nvfx_reg reg = makeReg(NVFXSR_NONE, 0);
        int width = 4;
        uint8_t swz[4] = {0, 1, 2, 3};
    };

    auto srcIsConst = [](const GenericSource& s) -> bool
    {
        return s.valid && s.reg.type == NVFXSR_CONST;
    };

    auto makeGenericSource =
        [&](const struct nvfx_reg& reg, int width) -> GenericSource
    {
        GenericSource s;
        s.valid = true;
        s.reg = reg;
        s.width = std::max(1, width);
        identitySwizzleForWidth(s.width, s.swz);
        return s;
    };

    std::function<GenericSource(IRValueID)> resolveGenericSource;
    std::function<bool(IRValueID, int)> materializeValueToTempReg;
    std::function<GenericSource(IRValueID)> materializeValueToTemp;
    std::function<bool(IRValueID, const struct nvfx_reg&, int)> emitValueToDest;
    std::function<bool(const MatVecMulBinding&, const struct nvfx_reg&, int)>
        emitMatVecMulBindingToDest;
    std::function<void(IRValueID)> collectGenericLiterals;
    std::unordered_map<IRValueID, int> valueToPreparedAddressTemp;

    auto sourceToNvfx = [&](const GenericSource& src) -> struct nvfx_src
    {
        struct nvfx_src s = makeSrc(src.reg);
        s.swz[0] = src.swz[0];
        s.swz[1] = src.swz[1];
        s.swz[2] = src.swz[2];
        s.swz[3] = src.swz[3];
        return s;
    };

    auto emitMovFromSource =
        [&](const struct nvfx_reg& dst, int mask, const GenericSource& src)
    {
        struct nvfx_insn in = nvfx_insn(
            0, 0, -1, -1,
            const_cast<struct nvfx_reg&>(dst),
            mask,
            sourceToNvfx(src), makeSrc(makeReg(NVFXSR_NONE, 0)),
            makeSrc(makeReg(NVFXSR_NONE, 0)));
        asm_.emit(in, VP_OP(MOV));
    };

    auto emitPostBiasHalfTransform =
        [&](int srcTempIdx, const struct nvfx_reg& dst, float half)
    {
        auto [halfReg, halfLane] = literals.assign(half);
        const int biasTempIdx = allocTemp();

        const struct nvfx_reg srcTemp = makeReg(NVFXSR_TEMP, srcTempIdx);
        const struct nvfx_reg biasTemp = makeReg(NVFXSR_TEMP, biasTempIdx);
        const struct nvfx_reg halfConst = makeReg(NVFXSR_CONST, halfReg);

        struct nvfx_src srcW = makeSrc(srcTemp);
        srcW.swz[0] = srcW.swz[1] = srcW.swz[2] = srcW.swz[3] = 3;
        struct nvfx_src halfSrc = makeSrc(halfConst);
        halfSrc.swz[0] = halfSrc.swz[1] =
            halfSrc.swz[2] = halfSrc.swz[3] =
                static_cast<uint8_t>(halfLane);

        // Rb.x = q.w * 0.5
        struct nvfx_insn mulW = nvfx_insn(
            0, 0, -1, -1,
            const_cast<struct nvfx_reg&>(biasTemp),
            NVFX_VP_MASK_X,
            srcW, halfSrc, makeSrc(makeReg(NVFXSR_NONE, 0)));
        asm_.emit(mulW, VP_OP(MUL));

        // dst.xyz = q.xyz * 0.5 + Rb.x
        struct nvfx_src srcXYZ = makeSrc(srcTemp);
        srcXYZ.swz[0] = 0; srcXYZ.swz[1] = 1;
        srcXYZ.swz[2] = 2; srcXYZ.swz[3] = 2;
        struct nvfx_src biasX = makeSrc(biasTemp);
        biasX.swz[0] = biasX.swz[1] = biasX.swz[2] = biasX.swz[3] = 0;
        struct nvfx_insn madXYZ = nvfx_insn(
            0, 0, -1, -1,
            const_cast<struct nvfx_reg&>(dst),
            NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
            srcXYZ, halfSrc, biasX);
        asm_.emit(madXYZ, VP_OP(MAD));

        // dst.w = q.w
        struct nvfx_insn movW = nvfx_insn(
            0, 0, -1, -1,
            const_cast<struct nvfx_reg&>(dst),
            NVFX_VP_MASK_W,
            srcW, makeSrc(makeReg(NVFXSR_NONE, 0)),
            makeSrc(makeReg(NVFXSR_NONE, 0)));
        asm_.emit(movW, VP_OP(MOV));

        availableTemps.push_back(biasTempIdx);
    };

    auto preloadConstSource = [&](GenericSource& s) -> bool
    {
        if (!srcIsConst(s)) return true;
        const int tempIdx = allocTemp();
        const struct nvfx_reg tempReg = makeReg(NVFXSR_TEMP, tempIdx);
        emitMovFromSource(tempReg, writemaskForWidth(s.width), s);
        s = makeGenericSource(tempReg, s.width);
        return true;
    };

    auto emitArithBindingToDest =
        [&](const ArithBinding& a,
            const struct nvfx_reg& dst,
            int mask) -> bool
    {
        const struct nvfx_reg none = makeReg(NVFXSR_NONE, 0);
        GenericSource src[3];
        const int srcCount = (a.op == ArithOp::Mad) ? 3 : 2;
        for (int i = 0; i < srcCount; ++i)
        {
            src[i] = resolveGenericSource(a.srcIds[i]);
            if (!src[i].valid)
            {
                src[i] = materializeValueToTemp(a.srcIds[i]);
                if (!src[i].valid) return false;
            }
        }

        int constCount = 0;
        for (int i = 0; i < srcCount; ++i)
            if (srcIsConst(src[i])) ++constCount;
        for (int i = srcCount - 1; constCount > 1 && i >= 0; --i)
        {
            if (!srcIsConst(src[i])) continue;
            if (!preloadConstSource(src[i])) return false;
            --constCount;
        }

        struct nvfx_src srcs[3] = {
            makeSrc(none), makeSrc(none), makeSrc(none) };
        uint8_t opcode = VP_OP(ADD);
        switch (a.op)
        {
        case ArithOp::Add:
            if (srcIsConst(src[1]) && !srcIsConst(src[0]))
            {
                srcs[0] = sourceToNvfx(src[1]);
                srcs[2] = sourceToNvfx(src[0]);
            }
            else
            {
                srcs[0] = sourceToNvfx(src[0]);
                srcs[2] = sourceToNvfx(src[1]);
            }
            opcode = VP_OP(ADD);
            break;
        case ArithOp::Sub:
            srcs[0] = sourceToNvfx(src[0]);
            srcs[2] = sourceToNvfx(src[1]);
            srcs[2].negate = !srcs[2].negate;
            opcode = VP_OP(ADD);
            break;
        case ArithOp::Mul:
            srcs[0] = sourceToNvfx(src[0]);
            srcs[1] = sourceToNvfx(src[1]);
            opcode = VP_OP(MUL);
            break;
        case ArithOp::Mad:
            srcs[0] = sourceToNvfx(src[0]);
            srcs[1] = sourceToNvfx(src[1]);
            srcs[2] = sourceToNvfx(src[2]);
            opcode = VP_OP(MAD);
            break;
        case ArithOp::Min:
            srcs[0] = sourceToNvfx(src[0]);
            srcs[1] = sourceToNvfx(src[1]);
            opcode = VP_OP(MIN);
            break;
        case ArithOp::Max:
            srcs[0] = sourceToNvfx(src[0]);
            srcs[1] = sourceToNvfx(src[1]);
            opcode = VP_OP(MAX);
            break;
        case ArithOp::Dot3:
        case ArithOp::Dot4:
            srcs[0] = sourceToNvfx(src[0]);
            srcs[1] = sourceToNvfx(src[1]);
            opcode = (a.op == ArithOp::Dot3) ? VP_OP(DP3) : VP_OP(DP4);
            break;
        }

        struct nvfx_insn in = nvfx_insn(
            0, 0, -1, -1,
            const_cast<struct nvfx_reg&>(dst),
            mask,
            srcs[0], srcs[1], srcs[2]);
        asm_.emit(in, opcode);
        emittedSomething = true;
        return true;
    };

    collectGenericLiterals = [&](IRValueID id)
    {
        float lit = 0.0f;
        if (isFloatLiteral(id, lit))
        {
            literals.assign(lit);
            return;
        }

        auto arIt = valueToArith.find(id);
        if (arIt != valueToArith.end())
        {
            const int srcCount = (arIt->second.op == ArithOp::Mad) ? 3 : 2;
            for (int i = 0; i < srcCount; ++i)
                collectGenericLiterals(arIt->second.srcIds[i]);
            return;
        }

        auto gvIt = valueToGenericVecConstruct.find(id);
        if (gvIt != valueToGenericVecConstruct.end())
        {
            for (int i = 0; i < gvIt->second.width; ++i)
                collectGenericLiterals(gvIt->second.lanes[i]);
        }
    };

    auto emitGenericVecConstructToDest =
        [&](const GenericVecConstructBinding& gv,
            const struct nvfx_reg& dst,
            int maxWidth) -> bool
    {
        const int laneCount = std::min(gv.width, maxWidth);
        for (int i = 0; i < laneCount; ++i)
            collectGenericLiterals(gv.lanes[i]);

        bool laneEmitted[4] = {false, false, false, false};
        IRValueID deferredMinMaxLane = 0;
        for (int i = 0; i < laneCount; ++i)
        {
            const IRValueID laneId = gv.lanes[i];
            auto arIt = valueToArith.find(laneId);
            if (arIt != valueToArith.end() &&
                (arIt->second.op == ArithOp::Min ||
                 arIt->second.op == ArithOp::Max))
            {
                deferredMinMaxLane = laneId;
                break;
            }
        }
        if (deferredMinMaxLane != 0)
        {
            auto arIt = valueToArith.find(deferredMinMaxLane);
            if (arIt != valueToArith.end())
            {
                for (IRValueID depId : arIt->second.srcIds)
                {
                    if (depId == 0) continue;
                    if (valueToArith.count(depId) ||
                        valueToGenericVecConstruct.count(depId))
                    {
                        GenericSource dep = materializeValueToTemp(depId);
                        if (!dep.valid) return false;
                    }
                }
            }

            for (int i = 0; i < laneCount; ++i)
            {
                if (laneEmitted[i] || gv.lanes[i] == deferredMinMaxLane)
                    continue;
                const IRValueID laneId = gv.lanes[i];
                int mask = 0;
                for (int j = i; j < laneCount; ++j)
                {
                    if (gv.lanes[j] != laneId ||
                        gv.lanes[j] == deferredMinMaxLane)
                    {
                        continue;
                    }
                    mask |= (8 >> j);
                    laneEmitted[j] = true;
                }
                if (!emitValueToDest(laneId, dst, mask))
                    return false;
            }
        }
        for (int i = 0; i < laneCount; ++i)
        {
            if (laneEmitted[i]) continue;
            const IRValueID laneId = gv.lanes[i];
            int mask = 0;
            for (int j = i; j < laneCount; ++j)
            {
                if (gv.lanes[j] != laneId) continue;
                mask |= (8 >> j);
                laneEmitted[j] = true;
            }
            if (!emitValueToDest(laneId, dst, mask))
                return false;
        }
        return true;
    };

    emitMatVecMulBindingToDest =
        [&](const MatVecMulBinding& binding,
            const struct nvfx_reg& dst,
            int writeLanes) -> bool
    {
        const struct nvfx_reg none = makeReg(NVFXSR_NONE, 0);

        MatVecMulBinding b = binding;
        if (b.hasTempBuild)
        {
            VecConstructBinding vb = b.tempBuild;
            const int tempReg = b.vectorInputIdx;
            const int laneCount = vb.wIsLiteralOne ? 3 : 4;

            for (int j = 0; j < laneCount; ++j)
            {
                if (vb.lanes[j].kind == LaneSource::Kind::Literal)
                {
                    auto [cReg, cLane] =
                        literals.assign(vb.lanes[j].litValue);
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
                const int regIdx = vb.lanes[i].regIdx;
                if (k == LaneSource::Kind::Unknown)
                    return false;

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
                for (int j = 0; j < 4; ++j)
                {
                    if (!(mask & (8 >> j)))
                        swz[j] = swz[firstMatchLane >= 0 ? firstMatchLane : 0];
                }

                const struct nvfx_reg src = (k == LaneSource::Kind::InputLane)
                    ? makeReg(NVFXSR_INPUT, regIdx)
                    : makeReg(NVFXSR_CONST, regIdx);
                struct nvfx_src s0 = makeSrc(src);
                s0.swz[0] = swz[0]; s0.swz[1] = swz[1];
                s0.swz[2] = swz[2]; s0.swz[3] = swz[3];
                const struct nvfx_reg tempRegN = makeReg(NVFXSR_TEMP, tempReg);
                struct nvfx_insn movInsn = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(tempRegN),
                    mask,
                    s0, makeSrc(none), makeSrc(none));
                asm_.emit(movInsn, VP_OP(MOV));
            }
        }

        if (b.genericTempBuildId != 0)
        {
            if (!materializeValueToTempReg(b.genericTempBuildId,
                                           b.vectorInputIdx))
            {
                return false;
            }
        }

        struct nvfx_reg vecReg;
        if (b.vectorIsTemp)
            vecReg = makeReg(NVFXSR_TEMP, b.vectorInputIdx);
        else
            vecReg = makeReg(NVFXSR_INPUT, b.vectorInputIdx);

        uint8_t indexedAddressSwz = 0;
        if (b.matrixIsIndexed)
        {
            IRValueID addressBaseId = b.matrixIndexId;
            auto shIt = valueToShuffle.find(b.matrixIndexId);
            if (shIt != valueToShuffle.end() && shIt->second.width >= 1)
            {
                addressBaseId = shIt->second.srcId;
                indexedAddressSwz =
                    static_cast<uint8_t>(shIt->second.lanes[0] & 0x3);
            }
            if (!valueToPreparedAddressTemp.count(addressBaseId))
            {
                GenericSource idxSrc = resolveGenericSource(addressBaseId);
                if (!idxSrc.valid)
                    idxSrc = materializeValueToTemp(addressBaseId);
                if (!idxSrc.valid)
                    return false;

                const int idxTemp = allocTemp();
                const struct nvfx_reg idxTempReg =
                    makeReg(NVFXSR_TEMP, idxTemp);

                struct nvfx_insn floorIdx = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(idxTempReg),
                    NVFX_VP_MASK_ALL,
                    sourceToNvfx(idxSrc), makeSrc(none), makeSrc(none));
                asm_.emit(floorIdx, VP_OP(FLR));

                auto [scaleReg, scaleLane] = literals.assign(4.0f);
                struct nvfx_src scaleSrc =
                    makeSrc(makeReg(NVFXSR_CONST, scaleReg));
                scaleSrc.swz[0] = scaleSrc.swz[1] =
                    scaleSrc.swz[2] = scaleSrc.swz[3] =
                        static_cast<uint8_t>(scaleLane);

                struct nvfx_insn mulIdx = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(idxTempReg),
                    NVFX_VP_MASK_ALL,
                    makeSrc(idxTempReg), scaleSrc, makeSrc(none));
                asm_.emit(mulIdx, VP_OP(MUL));

                const struct nvfx_reg addrReg = makeReg(NVFXSR_ADDRESS, 0);
                struct nvfx_insn arl = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(addrReg),
                    NVFX_VP_MASK_ALL,
                    makeSrc(idxTempReg), makeSrc(none), makeSrc(none));
                asm_.emit(arl, VP_OP(ARL));
                valueToPreparedAddressTemp[addressBaseId] = idxTemp;
            }
        }

        const uint8_t opcode =
            b.vecOpcode != 0 ? b.vecOpcode : static_cast<uint8_t>(VP_OP(DP4));
        const int lanes = std::min(writeLanes, b.matrixRowCount);
        const int rowStart = (b.matrixRowCount == 3) ? 1 : 0;
        for (int i = 0; i < lanes; ++i)
        {
            const int k = rowStart + i;
            const int rowReg = b.matrixBaseReg + kDp4RowOffset[k];
            const struct nvfx_reg rowConst = makeReg(NVFXSR_CONST, rowReg);

            struct nvfx_src src0 = makeSrc(vecReg);
            src0.swz[0] = b.inputSwz[0];
            src0.swz[1] = b.inputSwz[1];
            src0.swz[2] = b.inputSwz[2];
            src0.swz[3] = b.inputSwz[3];
            struct nvfx_src src1 = makeSrc(rowConst);
            if (b.matrixIsIndexed)
            {
                src1.indirect = 1;
                src1.indirect_reg = 0;
                src1.indirect_swz = indexedAddressSwz;
            }
            struct nvfx_insn dp = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                kDp4Writemask[k],
                src0, src1, makeSrc(none));
            asm_.emit(dp, opcode);
        }

        emittedSomething = true;
        return true;
    };

    resolveGenericSource = [&](IRValueID id) -> GenericSource
    {
        auto tIt = valueToTempReg.find(id);
        if (tIt != valueToTempReg.end())
            return makeGenericSource(makeReg(NVFXSR_TEMP, tIt->second),
                                     valueWidth(id));

        auto vsIt = valueToSource.find(id);
        if (vsIt != valueToSource.end())
        {
            const struct nvfx_reg r =
                (vsIt->second.kind == ValueSource::Kind::Input)
                    ? makeReg(NVFXSR_INPUT, vsIt->second.regIdx)
                    : makeReg(NVFXSR_CONST, vsIt->second.regIdx);
            return makeGenericSource(r, vsIt->second.width);
        }

        auto shIt = valueToShuffle.find(id);
        if (shIt != valueToShuffle.end())
        {
            GenericSource base = resolveGenericSource(shIt->second.srcId);
            if (!base.valid && valueToMatVecMul.count(shIt->second.srcId))
                base = materializeValueToTemp(shIt->second.srcId);
            if (!base.valid) return base;
            GenericSource s = base;
            s.width = std::max(1, shIt->second.width);
            for (int i = 0; i < 4; ++i)
                s.swz[i] = base.swz[shIt->second.lanes[i] & 3];
            if (s.width == 1)
                s.swz[1] = s.swz[2] = s.swz[3] = s.swz[0];
            return s;
        }

        float lit = 0.0f;
        if (isFloatLiteral(id, lit))
        {
            auto [cReg, cLane] = literals.assign(lit);
            GenericSource s = makeGenericSource(makeReg(NVFXSR_CONST, cReg), 1);
            s.swz[0] = s.swz[1] = s.swz[2] = s.swz[3] =
                static_cast<uint8_t>(cLane);
            return s;
        }

        return GenericSource{};
    };

    materializeValueToTempReg =
        [&](IRValueID id, int tempIdx) -> bool
    {
        const struct nvfx_reg tempReg = makeReg(NVFXSR_TEMP, tempIdx);

        GenericSource src = resolveGenericSource(id);
        if (src.valid)
        {
            emitMovFromSource(tempReg, writemaskForWidth(src.width), src);
            valueToTempReg[id] = tempIdx;
            emittedSomething = true;
            return true;
        }

        auto arIt = valueToArith.find(id);
        if (arIt != valueToArith.end())
        {
            if (!emitArithBindingToDest(arIt->second, tempReg,
                                        writemaskForWidth(arIt->second.width)))
            {
                out.diagnostics.push_back(
                    "nv40-vp: generic arithmetic operand did not resolve");
                return false;
            }
            valueToTempReg[id] = tempIdx;
            return true;
        }

        auto gvIt = valueToGenericVecConstruct.find(id);
        if (gvIt != valueToGenericVecConstruct.end())
        {
            if (!emitGenericVecConstructToDest(gvIt->second, tempReg, 4))
                return false;
            valueToTempReg[id] = tempIdx;
            emittedSomething = true;
            return true;
        }

        auto mvIt = valueToMatVecMul.find(id);
        if (mvIt != valueToMatVecMul.end())
        {
            if (!emitMatVecMulBindingToDest(mvIt->second, tempReg,
                                            valueWidth(id)))
            {
                out.diagnostics.push_back(
                    "nv40-vp: MatVecMul did not materialize");
                return false;
            }
            valueToTempReg[id] = tempIdx;
            emittedSomething = true;
            return true;
        }

        return false;
    };

    materializeValueToTemp = [&](IRValueID id) -> GenericSource
    {
        auto tIt = valueToTempReg.find(id);
        if (tIt != valueToTempReg.end())
            return makeGenericSource(makeReg(NVFXSR_TEMP, tIt->second),
                                     valueWidth(id));

        const int tempIdx = allocTemp();
        if (!materializeValueToTempReg(id, tempIdx))
            return GenericSource{};
        return makeGenericSource(makeReg(NVFXSR_TEMP, tempIdx), valueWidth(id));
    };

    emitValueToDest =
        [&](IRValueID id, const struct nvfx_reg& dst, int mask) -> bool
    {
        auto arIt = valueToArith.find(id);
        if (arIt != valueToArith.end())
            return emitArithBindingToDest(arIt->second, dst, mask);

        auto gvIt = valueToGenericVecConstruct.find(id);
        if (gvIt != valueToGenericVecConstruct.end())
            return emitGenericVecConstructToDest(gvIt->second, dst, 4);

        GenericSource src = resolveGenericSource(id);
        if (!src.valid)
        {
            out.diagnostics.push_back(
                "nv40-vp: generic value did not resolve to a source");
            return false;
        }
        emitMovFromSource(dst, mask, src);
        emittedSomething = true;
        return true;
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
            if (arIt->second.op != ArithOp::Add &&
                arIt->second.op != ArithOp::Sub &&
                arIt->second.op != ArithOp::Mul)
            {
                if (emitValueToDest(srcId, dstReg, NVFX_VP_MASK_ALL))
                    return true;
                out.diagnostics.push_back(
                    "nv40-vp: generic arithmetic store did not lower");
                return false;
            }

            bool okA = false, okB = false;
            struct nvfx_reg regA = regFromValue(arIt->second.srcIds[0], okA);
            struct nvfx_reg regB = regFromValue(arIt->second.srcIds[1], okB);
            if (!okA || !okB)
            {
                if (emitValueToDest(srcId, dstReg, NVFX_VP_MASK_ALL))
                    return true;
                out.diagnostics.push_back(
                    "nv40-vp: generic arithmetic operand did not resolve");
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
            if (arIt->second.op == ArithOp::Sub)
                srcs[slotB].negate = !srcs[slotB].negate;

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

        auto viStoreIt = valueToVecInsert.find(srcId);
        if (viStoreIt != valueToVecInsert.end())
        {
            const VecInsertBinding& vi = viStoreIt->second;
            auto mvBaseIt = valueToMatVecMul.find(vi.baseVecId);
            auto addIt = valueToArith.find(vi.scalarId);
            if (vi.laneIndex == 1 &&
                mvBaseIt != valueToMatVecMul.end() &&
                addIt != valueToArith.end() &&
                addIt->second.op == ArithOp::Add)
            {
                const MatVecMulBinding& mv = mvBaseIt->second;

                auto resolveScalarConst =
                    [&](IRValueID id, int& regIdx) -> bool
                {
                    auto vsIt = valueToSource.find(id);
                    if (vsIt == valueToSource.end()) return false;
                    if (vsIt->second.kind != ValueSource::Kind::Const)
                        return false;
                    if (vsIt->second.width != 1) return false;
                    regIdx = vsIt->second.regIdx;
                    return true;
                };

                auto isBaseLaneY = [&](IRValueID id) -> bool
                {
                    auto shIt = valueToShuffle.find(id);
                    if (shIt == valueToShuffle.end()) return false;
                    return shIt->second.srcId == vi.baseVecId &&
                           shIt->second.width == 1 &&
                           shIt->second.lanes[0] == 1;
                };

                auto resolveMulBaseYScale =
                    [&](IRValueID id, int& scaleReg) -> bool
                {
                    auto mulIt = valueToArith.find(id);
                    if (mulIt == valueToArith.end()) return false;
                    if (mulIt->second.op != ArithOp::Mul) return false;

                    const IRValueID a = mulIt->second.srcIds[0];
                    const IRValueID b = mulIt->second.srcIds[1];
                    if (isBaseLaneY(a) && resolveScalarConst(b, scaleReg))
                        return true;
                    if (isBaseLaneY(b) && resolveScalarConst(a, scaleReg))
                        return true;
                    return false;
                };

                int scaleReg = -1;
                int offsetReg = -1;
                IRValueID mulId = 0;
                if (resolveMulBaseYScale(addIt->second.srcIds[0], scaleReg) &&
                    resolveScalarConst(addIt->second.srcIds[1], offsetReg))
                {
                    mulId = addIt->second.srcIds[0];
                }
                else if (resolveMulBaseYScale(addIt->second.srcIds[1], scaleReg) &&
                         resolveScalarConst(addIt->second.srcIds[0], offsetReg))
                {
                    mulId = addIt->second.srcIds[1];
                }

                if (mulId != 0 && mv.priorChainId == 0 && !mv.vectorIsTemp &&
                    !mv.hasTempBuild && mv.genericTempBuildId == 0)
                {
                    const int tempIdx = allocTemp();
                    const struct nvfx_reg tempReg =
                        makeReg(NVFXSR_TEMP, tempIdx);
                    const struct nvfx_reg vecReg =
                        makeReg(NVFXSR_INPUT, mv.vectorInputIdx);

                    const uint8_t opcode =
                        mv.vecOpcode != 0
                            ? mv.vecOpcode
                            : static_cast<uint8_t>(VP_OP(DP4));

                    for (int i = 0; i < 4; ++i)
                    {
                        const int rowOffset = kDp4RowOffset[i];
                        const int rowReg = mv.matrixBaseReg + rowOffset;
                        const struct nvfx_reg rowConst =
                            makeReg(NVFXSR_CONST, rowReg);

                        struct nvfx_src src0 = makeSrc(vecReg);
                        src0.swz[0] = mv.inputSwz[0];
                        src0.swz[1] = mv.inputSwz[1];
                        src0.swz[2] = mv.inputSwz[2];
                        src0.swz[3] = mv.inputSwz[3];
                        struct nvfx_src src1 = makeSrc(rowConst);
                        struct nvfx_src src2 = makeSrc(none);

                        const bool isOverriddenY = (rowOffset == 1);
                        struct nvfx_reg dpDst =
                            isOverriddenY ? tempReg : dstReg;
                        const int mask =
                            isOverriddenY ? NVFX_VP_MASK_X
                                          : kDp4Writemask[i];
                        struct nvfx_insn in = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dpDst),
                            mask,
                            src0, src1, src2);
                        asm_.emit(in, opcode);
                    }

                    struct nvfx_src tempX = makeSrc(tempReg);
                    tempX.swz[0] = tempX.swz[1] =
                        tempX.swz[2] = tempX.swz[3] = 0;
                    struct nvfx_src scaleSrc =
                        makeSrc(makeReg(NVFXSR_CONST, scaleReg));
                    scaleSrc.swz[0] = scaleSrc.swz[1] =
                        scaleSrc.swz[2] = scaleSrc.swz[3] = 0;
                    struct nvfx_insn mulInsn = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempReg),
                        NVFX_VP_MASK_X,
                        tempX, scaleSrc, makeSrc(none));
                    asm_.emit(mulInsn, VP_OP(MUL));

                    struct nvfx_src offsetSrc =
                        makeSrc(makeReg(NVFXSR_CONST, offsetReg));
                    offsetSrc.swz[0] = offsetSrc.swz[1] =
                        offsetSrc.swz[2] = offsetSrc.swz[3] = 0;
                    struct nvfx_src scaledSrc = makeSrc(tempReg);
                    scaledSrc.swz[0] = scaledSrc.swz[1] =
                        scaledSrc.swz[2] = scaledSrc.swz[3] = 0;
                    struct nvfx_insn addInsn = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_VP_MASK_Y,
                        offsetSrc, makeSrc(none), scaledSrc);
                    asm_.emit(addInsn, VP_OP(ADD));

                    emittedSomething = true;
                    return true;
                }
            }
        }

        IRValueID matVecSrcId = srcId;
        int matVecWriteLanes = 4;
        auto shMatIt = valueToShuffle.find(srcId);
        if (shMatIt != valueToShuffle.end() &&
            valueToMatVecMul.count(shMatIt->second.srcId))
        {
            const int outWidth = [&]() {
                auto wIt = outputWidthBySemantic.find(
                    semKey(semUpper, semanticIndex));
                return (wIt == outputWidthBySemantic.end()) ? 4 : wIt->second;
            }();
            const int copyWidth = std::min(outWidth, shMatIt->second.width);
            for (int i = 0; i < copyWidth; ++i)
            {
                if (shMatIt->second.lanes[i] != i)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: shuffled MatVecMul store only supports identity-prefix lanes");
                    return false;
                }
            }
            matVecSrcId = shMatIt->second.srcId;
            matVecWriteLanes = copyWidth;
        }

        auto mvIt = valueToMatVecMul.find(matVecSrcId);
        if (mvIt != valueToMatVecMul.end())
        {
            // Walk the matvecmul chain back to its root (the first step,
            // whose vector source is a direct input / VecPromote /
            // VecConstruct).  We emit chain steps from root to tail; each
            // intermediate step writes to a freshly-allocated temp, while
            // the final step writes to the output register.  Temps are
            // recycled after their consumer reads them, giving the R0 →
            // R1 → R0 ping-pong the reference compiler emits for back-to-back chains.
            std::vector<IRValueID> chainSteps;
            {
                IRValueID curr = matVecSrcId;
                while (curr != 0)
                {
                    chainSteps.push_back(curr);
                    auto it = valueToMatVecMul.find(curr);
                    if (it == valueToMatVecMul.end()) break;
                    curr = it->second.priorChainId;
                }
                std::reverse(chainSteps.begin(), chainSteps.end());
            }

            // Track each emitted step's destination temp so the next
            // step can read from it.  The final step writes to dstReg,
            // not a temp, so there's no entry for it here.
            std::unordered_map<IRValueID, int> stepTempReg;

            for (size_t stepIdx = 0; stepIdx < chainSteps.size(); ++stepIdx)
            {
                const IRValueID stepId = chainSteps[stepIdx];
                const bool      isFinal = (stepIdx == chainSteps.size() - 1);
                MatVecMulBinding step = valueToMatVecMul[stepId];

                // Resolve vec source: prior chain step's temp (if this is a
                // chain link), or the original vector.
                bool readsPriorChainTemp = false;
                int  priorChainTempReg   = -1;
                if (step.priorChainId != 0)
                {
                    auto t = stepTempReg.find(step.priorChainId);
                    if (t == stepTempReg.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-vp: chain temp not found while emitting matvecmul");
                        return false;
                    }
                    priorChainTempReg = t->second;
                    readsPriorChainTemp = true;
                }

                // For the root step only: handle VecConstruct temp-build.
                if (step.priorChainId == 0 && step.hasTempBuild)
                {
                    VecConstructBinding vb = step.tempBuild;
                    const int tempReg = step.vectorInputIdx;
                    const bool useDph = vb.wIsLiteralOne;
                    const int  laneCount = useDph ? 3 : 4;

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
                        for (int j = 0; j < 4; ++j)
                        {
                            if (!(mask & (8 >> j)))
                                swz[j] = swz[firstMatchLane >= 0 ? firstMatchLane : 0];
                        }

                        const struct nvfx_reg src = (k == LaneSource::Kind::InputLane)
                            ? makeReg(NVFXSR_INPUT, regIdx)
                            : makeReg(NVFXSR_CONST, regIdx);
                        struct nvfx_src s0 = makeSrc(src);
                        s0.swz[0] = swz[0]; s0.swz[1] = swz[1];
                        s0.swz[2] = swz[2]; s0.swz[3] = swz[3];

                        const struct nvfx_reg tempRegN = makeReg(NVFXSR_TEMP, tempReg);
                        struct nvfx_insn movInsn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(tempRegN),
                            mask,
                            s0, makeSrc(none), makeSrc(none));
                        asm_.emit(movInsn, VP_OP(MOV));
                    }
                }

                if (step.priorChainId == 0 && step.genericTempBuildId != 0)
                {
                    if (!materializeValueToTempReg(step.genericTempBuildId,
                                                   step.vectorInputIdx))
                    {
                        out.diagnostics.push_back(
                            "nv40-vp: generic MatVecMul vector did not materialize");
                        return false;
                    }
                }

                // Determine vec source register for the 4 DP4s.
                struct nvfx_reg vecReg;
                if (readsPriorChainTemp)
                    vecReg = makeReg(NVFXSR_TEMP, priorChainTempReg);
                else if (step.vectorIsTemp)
                    vecReg = makeReg(NVFXSR_TEMP, step.vectorInputIdx);
                else
                    vecReg = makeReg(NVFXSR_INPUT, step.vectorInputIdx);

                // Determine dst: temp for non-final, output for final.
                struct nvfx_reg stepDst;
                int  stepTemp = -1;
                const bool finalNeedsPostBias = isFinal && step.postBiasHalf;
                if (isFinal && !finalNeedsPostBias)
                {
                    stepDst = dstReg;
                }
                else
                {
                    stepTemp = allocTemp();
                    stepDst = makeReg(NVFXSR_TEMP, stepTemp);
                    stepTempReg[stepId] = stepTemp;
                }

                const uint8_t opcode =
                    step.vecOpcode != 0
                        ? step.vecOpcode
                        : static_cast<uint8_t>(VP_OP(DP4));
                const int dpCount = (isFinal && !finalNeedsPostBias)
                    ? matVecWriteLanes
                    : 4;
                for (int i = 0; i < dpCount; ++i)
                {
                    const int rowReg =
                        step.matrixBaseReg + kDp4RowOffset[i];
                    const struct nvfx_reg rowConst =
                        makeReg(NVFXSR_CONST, rowReg);

                    struct nvfx_src src0 = makeSrc(vecReg);
                    src0.swz[0] = step.inputSwz[0];
                    src0.swz[1] = step.inputSwz[1];
                    src0.swz[2] = step.inputSwz[2];
                    src0.swz[3] = step.inputSwz[3];
                    struct nvfx_src src1 = makeSrc(rowConst);
                    struct nvfx_src src2 = makeSrc(none);

                    struct nvfx_insn in = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(stepDst),
                        kDp4Writemask[i],
                        src0, src1, src2);
                    asm_.emit(in, opcode);
                }

                if (finalNeedsPostBias)
                {
                    emitPostBiasHalfTransform(stepTemp, dstReg, step.biasHalf);
                    availableTemps.push_back(stepTemp);
                }

                // After consuming the prior chain temp, recycle it so
                // the next step can reuse it (R0 → R1 → R0 ping-pong).
                if (readsPriorChainTemp)
                    availableTemps.push_back(priorChainTempReg);
            }

            emittedSomething = true;
            return true;
        }

        auto gvIt = valueToGenericVecConstruct.find(srcId);
        if (gvIt != valueToGenericVecConstruct.end())
        {
            const int outDeclWidth = [&]() {
                auto wIt = outputWidthBySemantic.find(semKey(semUpper, semanticIndex));
                return (wIt == outputWidthBySemantic.end()) ? 4 : wIt->second;
            }();
            if (!emitGenericVecConstructToDest(gvIt->second, dstReg, outDeclWidth))
            {
                out.diagnostics.push_back(
                    "nv40-vp: generic VecConstruct store did not lower");
                return false;
            }
            emittedSomething = true;
            return true;
        }

        auto shStoreIt = valueToShuffle.find(srcId);
        if (shStoreIt != valueToShuffle.end())
        {
            auto baseIt = valueToSource.find(shStoreIt->second.srcId);
            if (baseIt != valueToSource.end() &&
                baseIt->second.kind == ValueSource::Kind::Input)
            {
                const int outWidth = [&]() {
                    auto wIt = outputWidthBySemantic.find(
                        semKey(semUpper, semanticIndex));
                    return (wIt == outputWidthBySemantic.end()) ? 4 : wIt->second;
                }();
                const int copyWidth = std::min(outWidth, shStoreIt->second.width);

                const struct nvfx_reg srcReg =
                    makeReg(NVFXSR_INPUT, baseIt->second.regIdx);
                struct nvfx_src src0 = makeSrc(srcReg);
                const uint8_t padLane =
                    static_cast<uint8_t>(shStoreIt->second.lanes[0] & 3);
                for (int i = 0; i < 4; ++i)
                {
                    src0.swz[i] = (i < copyWidth)
                        ? static_cast<uint8_t>(shStoreIt->second.lanes[i] & 3)
                        : padLane;
                }

                struct nvfx_insn in = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dstReg),
                    writemaskForWidth(copyWidth),
                    src0, makeSrc(none), makeSrc(none));
                asm_.emit(in, VP_OP(MOV));
                emittedSomething = true;
                return true;
            }
        }

        // ---- legacy non-chain matvecmul fallback (unused after deferral
        // refactor; kept until full coverage is verified).  The block
        // below is dead code; leave it disabled but in place so the
        // surrounding diff stays minimal.
        if (false) {
        auto mvDeadIt = valueToMatVecMul.find(srcId);
        if (mvDeadIt != valueToMatVecMul.end())
        {
            auto& mvIt = mvDeadIt;
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
        } // end if(false) — legacy block

        // VecConstruct(...) → StoreOutput: emit one MOV per source
        // register (input attr or const-bank slot) with a writemask
        // that covers only the lanes pulled from that source.  This
        // is the canonical the reference compiler shape for `out = float4(in.x,
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
                // source lane (the reference compiler default).
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
        // mask + .xyxx swizzle).  the reference compiler does the same.  Width info
        // comes from the input's recorded ValueSource width and the
        // output param's declared vectorSize.
        const int outWidth = [&]() {
            auto wIt = outputWidthBySemantic.find(semKey(semUpper, semanticIndex));
            return (wIt == outputWidthBySemantic.end()) ? 4 : wIt->second;
        }();
        const int srcWidth = it->second.width;
        const int copyWidth = std::min(outWidth, srcWidth);
        if (copyWidth < 4)
        {
            uint8_t s[4];
            identitySwizzleForWidth(copyWidth, s);
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

    auto preMaterializeGenericMatVecRoot = [&](IRValueID srcId) -> bool
    {
        auto it = valueToMatVecMul.find(srcId);
        if (it == valueToMatVecMul.end()) return true;

        IRValueID rootId = srcId;
        while (true)
        {
            auto rootIt = valueToMatVecMul.find(rootId);
            if (rootIt == valueToMatVecMul.end()) return true;
            if (rootIt->second.priorChainId == 0) break;
            rootId = rootIt->second.priorChainId;
        }

        auto rootIt = valueToMatVecMul.find(rootId);
        if (rootIt == valueToMatVecMul.end()) return true;
        if (rootIt->second.genericTempBuildId == 0) return true;

        if (!materializeValueToTempReg(rootIt->second.genericTempBuildId,
                                       rootIt->second.vectorInputIdx))
        {
            out.diagnostics.push_back(
                "nv40-vp: generic MatVecMul root did not materialize");
            return false;
        }
        rootIt->second.genericTempBuildId = 0;
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
                    { ValueSource::Kind::Input, nv40Idx,
                      std::max(1, inst.resultType.vectorSize) };
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
                if (inst.targetName == "gBlendMatrices")
                {
                    valueToPaletteUniformBase[inst.result] = 260;
                    break;
                }
                if (inst.resultType.isMatrix())
                    valueToMatrixBase[inst.result] = b->baseReg;
                else
                    valueToSource[inst.result] =
                        { ValueSource::Kind::Const, b->baseReg,
                          std::max(1, inst.resultType.vectorSize) };
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
                    out.diagnostics.push_back("nv40-vp: arithmetic op with <2 operands");
                    return out;
                }
                ArithBinding a;
                a.op = (inst.op == IROp::Add) ? ArithOp::Add
                     : (inst.op == IROp::Sub) ? ArithOp::Sub
                     : (inst.op == IROp::Mul) ? ArithOp::Mul
                     : (inst.op == IROp::Min) ? ArithOp::Min
                                              : ArithOp::Max;
                a.srcIds[0] = inst.operands[0];
                a.srcIds[1] = inst.operands[1];
                a.width = std::max(1, inst.resultType.vectorSize);
                valueToArith[inst.result] = a;
                break;
            }

            case IROp::Mad:
            {
                if (inst.operands.size() < 3)
                {
                    out.diagnostics.push_back("nv40-vp: MAD op with <3 operands");
                    return out;
                }
                ArithBinding a;
                a.op = ArithOp::Mad;
                a.srcIds[0] = inst.operands[0];
                a.srcIds[1] = inst.operands[1];
                a.srcIds[2] = inst.operands[2];
                a.width = std::max(1, inst.resultType.vectorSize);
                valueToArith[inst.result] = a;
                break;
            }

            case IROp::Dot:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-vp: Dot op with <2 operands");
                    return out;
                }
                const int lhsWidth = sourceValueWidth(inst.operands[0]);
                const int rhsWidth = sourceValueWidth(inst.operands[1]);
                const int dotWidth = std::max(lhsWidth, rhsWidth);
                ArithBinding a;
                a.op = (dotWidth <= 3) ? ArithOp::Dot3 : ArithOp::Dot4;
                a.srcIds[0] = inst.operands[0];
                a.srcIds[1] = inst.operands[1];
                a.width = 1;
                valueToArith[inst.result] = a;
                break;
            }

            case IROp::Normalize:
            {
                if (inst.operands.size() != 1)
                {
                    out.diagnostics.push_back("nv40-vp: Normalize op with !=1 operand");
                    return out;
                }
                VpNormalizeBinding n;
                n.sourceId = inst.operands[0];
                n.width = std::max(1, inst.resultType.vectorSize);
                valueToNormalize[inst.result] = n;
                break;
            }

            case IROp::Pow:
            {
                if (inst.operands.size() != 2)
                {
                    out.diagnostics.push_back("nv40-vp: Pow op with !=2 operands");
                    return out;
                }
                float exponent = 0.0f;
                if (!isFloatLiteral(inst.operands[1], exponent))
                {
                    out.diagnostics.push_back(
                        "nv40-vp: Pow exponent must be a float literal");
                    return out;
                }
                VpPowBinding p;
                p.baseId = inst.operands[0];
                p.exponent = exponent;
                valueToPow[inst.result] = p;
                break;
            }

            case IROp::CmpLe:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-vp: CmpLe op with <2 operands");
                    return out;
                }
                VpCmpBinding c;
                c.lhsId = inst.operands[0];
                c.rhsId = inst.operands[1];
                c.op = inst.op;
                valueToCmp[inst.result] = c;
                break;
            }

            case IROp::Select:
            {
                if (inst.operands.size() < 3)
                {
                    out.diagnostics.push_back("nv40-vp: Select op with <3 operands");
                    return out;
                }
                VpSelectBinding s;
                s.cmpId = inst.operands[0];
                s.trueId = inst.operands[1];
                s.falseId = inst.operands[2];
                valueToSelect[inst.result] = s;
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

            case IROp::VecExtract:
            {
                if (inst.resultType.isMatrix() && inst.operands.size() >= 2)
                {
                    auto palIt = valueToPaletteUniformBase.find(inst.operands[0]);
                    if (palIt != valueToPaletteUniformBase.end())
                    {
                        IndexedMatrixBinding imb;
                        imb.baseReg = palIt->second;
                        imb.indexId = inst.operands[1];
                        imb.rowCount = std::max(1, inst.resultType.matrixRows);
                        valueToIndexedMatrix[inst.result] = imb;
                        break;
                    }
                }

                // Same shape as a single-lane VecShuffle.  Alias into
                // valueToShuffle with width=1 so downstream consumers
                // (MatVecMul, StoreOutput, the per-lane MAD chain
                // recogniser) treat both ops uniformly.
                if (inst.operands.empty()) break;
                ShuffleBinding sb;
                sb.srcId = inst.operands[0];
                sb.width = 1;
                sb.lanes[0] = inst.componentIndex & 3;
                sb.lanes[1] = sb.lanes[0];
                sb.lanes[2] = sb.lanes[0];
                sb.lanes[3] = sb.lanes[0];
                valueToShuffle[inst.result] = sb;
                break;
            }

            case IROp::Bitcast:
            {
                if (inst.operands.empty()) break;
                auto idxIt = valueToIndexedMatrix.find(inst.operands[0]);
                if (idxIt != valueToIndexedMatrix.end() &&
                    inst.resultType.isMatrix())
                {
                    IndexedMatrixBinding imb = idxIt->second;
                    imb.rowCount = std::max(1, inst.resultType.matrixRows);
                    valueToIndexedMatrix[inst.result] = imb;
                    break;
                }
                auto matIt = valueToMatrixBase.find(inst.operands[0]);
                if (matIt != valueToMatrixBase.end() &&
                    inst.resultType.isMatrix())
                {
                    valueToMatrixBase[inst.result] = matIt->second;
                    break;
                }
                break;
            }

            case IROp::VecInsert:
            {
                if (inst.operands.size() < 2) break;
                VecInsertBinding vib;
                vib.baseVecId = inst.operands[0];
                vib.scalarId  = inst.operands[1];
                vib.laneIndex = inst.componentIndex & 3;
                valueToVecInsert[inst.result] = vib;
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
                        float lit = 0.0f;
                        if (isFloatLiteral(inst.operands[1], lit) &&
                            lit == 1.0f)
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

                // `float4(vec3_input, literal)` expands to the input's xyz
                // lanes plus a literal w lane.  The w=1 case above remains
                // on the DPH fast path; w=0 and other literals materialise
                // into a temp consumed by a DP4 MatVecMul.
                if (inst.operands.size() == 2)
                {
                    auto inputIt = valueToSource.find(inst.operands[0]);
                    float lit = 0.0f;
                    if (inputIt != valueToSource.end() &&
                        inputIt->second.kind == ValueSource::Kind::Input &&
                        inputIt->second.width == 3 &&
                        isFloatLiteral(inst.operands[1], lit))
                    {
                        vb.width = 4;
                        for (int lane = 0; lane < 3; ++lane)
                        {
                            vb.lanes[lane].kind = LaneSource::Kind::InputLane;
                            vb.lanes[lane].regIdx = inputIt->second.regIdx;
                            vb.lanes[lane].srcLane = lane;
                        }
                        vb.lanes[3].kind = LaneSource::Kind::Literal;
                        vb.lanes[3].litValue = lit;
                        vb.lanes[3].srcLane = 0;
                        vb.wIsLiteralOne = (lit == 1.0f);
                        vb.allInputLanes = false;
                        if (lit != 0.0f)
                            literals.assign(lit);
                        valueToVecConstruct[inst.result] = vb;
                        break;
                    }
                }

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
                    float lit = 0.0f;
                    if (isFloatLiteral(id, lit))
                    {
                        ls.kind     = LaneSource::Kind::Literal;
                        ls.litValue = lit;
                        vb.lanes[i] = ls;
                        allInputLanes = false;
                        continue;
                    }

                    // Anything else: bail out for this VecConstruct.
                    ok = false;
                    break;
                }
                if (!ok)
                {
                    GenericVecConstructBinding gv;
                    gv.width = static_cast<int>(inst.operands.size());
                    for (size_t i = 0; i < inst.operands.size(); ++i)
                        gv.lanes[i] = inst.operands[i];
                    valueToGenericVecConstruct[inst.result] = gv;
                    break;
                }

                // W lane = literal 1.0f -> DPH-fold opportunity for a
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
                // case).  the reference compiler lists all distinct source literals
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

            case IROp::MatConstruct:
            {
                float half = 0.0f;
                if (extractBiasMatrixHalf(inst, half))
                    valueToBiasMatrix[inst.result] = half;
                break;
            }

            case IROp::MatMul:
            {
                if (inst.operands.size() >= 2)
                {
                    auto biasIt = valueToBiasMatrix.find(inst.operands[0]);
                    auto matIt = valueToMatrixBase.find(inst.operands[1]);
                    if (biasIt != valueToBiasMatrix.end() &&
                        matIt != valueToMatrixBase.end())
                    {
                        BiasMatrixProductBinding b;
                        b.matrixBaseReg = matIt->second;
                        b.half = biasIt->second;
                        valueToBiasMatrixProduct[inst.result] = b;
                    }
                }
                break;
            }

            case IROp::MatVecMul:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-vp: MatVecMul with <2 operands");
                    return out;
                }

                int matrixBaseReg = -1;
                bool postBiasHalf = false;
                float biasHalf = 0.5f;
                IndexedMatrixBinding indexedMatrix;
                bool matrixIsIndexed = false;
                auto matIt = valueToMatrixBase.find(inst.operands[0]);
                if (matIt != valueToMatrixBase.end())
                {
                    matrixBaseReg = matIt->second;
                }
                else
                {
                    auto biasIt = valueToBiasMatrixProduct.find(inst.operands[0]);
                    if (biasIt != valueToBiasMatrixProduct.end())
                    {
                        matrixBaseReg = biasIt->second.matrixBaseReg;
                        postBiasHalf = true;
                        biasHalf = biasIt->second.half;
                    }
                    else
                    {
                        auto idxIt = valueToIndexedMatrix.find(inst.operands[0]);
                        if (idxIt != valueToIndexedMatrix.end())
                        {
                            indexedMatrix = idxIt->second;
                            matrixBaseReg = indexedMatrix.baseReg;
                            matrixIsIndexed = true;
                        }
                    }
                }
                if (matrixBaseReg < 0)
                {
                    out.diagnostics.push_back(
                        "nv40-vp: MatVecMul matrix operand is not a uniform matrix");
                    return out;
                }

                MatVecMulBinding binding;
                binding.matrixBaseReg = matrixBaseReg;
                binding.postBiasHalf = postBiasHalf;
                binding.biasHalf = biasHalf;
                binding.matrixIsIndexed = matrixIsIndexed;
                binding.matrixIndexId = indexedMatrix.indexId;
                binding.matrixRowCount = matrixIsIndexed
                    ? indexedMatrix.rowCount
                    : std::max(1, inst.resultType.vectorSize);

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

                    if (vb.lanes[0].kind == LaneSource::Kind::InputLane &&
                        vb.lanes[1].kind == LaneSource::Kind::InputLane &&
                        vb.lanes[2].kind == LaneSource::Kind::InputLane &&
                        vb.lanes[3].kind == LaneSource::Kind::Literal &&
                        vb.lanes[3].litValue == 0.0f &&
                        vb.lanes[0].regIdx == vb.lanes[1].regIdx &&
                        vb.lanes[0].regIdx == vb.lanes[2].regIdx &&
                        vb.lanes[0].srcLane == 0 &&
                        vb.lanes[1].srcLane == 1 &&
                        vb.lanes[2].srcLane == 2)
                    {
                        binding.vectorInputIdx = vb.lanes[0].regIdx;
                        binding.vecOpcode = static_cast<uint8_t>(VP_OP(DP3));
                        binding.inputSwz[0] = 0;
                        binding.inputSwz[1] = 1;
                        binding.inputSwz[2] = 2;
                        binding.inputSwz[3] = 0;
                        valueToMatVecMul[inst.result] = binding;
                        break;
                    }

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
                    if (binding.matrixRowCount == 3)
                    {
                        binding.vecOpcode = static_cast<uint8_t>(VP_OP(DP3));
                        binding.inputSwz[0] = 0;
                        binding.inputSwz[1] = 1;
                        binding.inputSwz[2] = 2;
                        binding.inputSwz[3] = 0;
                    }
                    valueToMatVecMul[inst.result] = binding;
                    break;
                }

                // DP4 path #3: vector operand is a previous MatVecMul's
                // result — chained matrix-vector multiplies.  Defer
                // emission entirely to the flush phase so the chain can
                // be interleaved with a per-lane MAD color store when
                // present (mul_chain4_v shape).  When there's nothing
                // to interleave with, the flush still walks the chain
                // back-to-front and emits each step's 4 DP4s into a
                // ping-pong temp pair, byte-matching the reference's
                // sequential layout.
                auto chainIt = valueToMatVecMul.find(inst.operands[1]);
                if (chainIt != valueToMatVecMul.end())
                {
                    binding.priorChainId   = inst.operands[1];
                    binding.vectorIsTemp   = true;
                    binding.vectorInputIdx = -1;  // resolved at flush
                    binding.vecOpcode      = 0;   // DP4 (default)
                    valueToMatVecMul[inst.result] = binding;
                    break;
                }

                if (valueToArith.count(inst.operands[1]) ||
                    valueToGenericVecConstruct.count(inst.operands[1]))
                {
                    const int tempReg = allocTemp();
                    binding.vectorInputIdx = tempReg;
                    binding.vectorIsTemp   = true;
                    binding.vecOpcode      = static_cast<uint8_t>(VP_OP(DP4));
                    binding.genericTempBuildId = inst.operands[1];
                    valueToMatVecMul[inst.result] = binding;
                    break;
                }

                out.diagnostics.push_back(
                    "nv40-vp: MatVecMul vector operand must be a vertex input, "
                    "a float4(vec3, 1.0f) promotion, a float4(...) constructor, "
                    "a generic arithmetic value, or a previous MatVecMul result");
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
                if (inst.resultType.isVector())
                {
                    const std::string semUpper = toUpper(inst.semanticName);
                    outputWidthBySemantic[semKey(semUpper, inst.semanticIndex)] =
                        std::max(1, inst.resultType.vectorSize);
                }

                // Multi-instruction expansions (matvecmul, arithmetic
                // needing a temp, VecInsert chains) are deferred until
                // after all single-instruction StoreOutputs have been
                // emitted.  This places single-insn stores first, which
                // matches the reference compiler's "simple first, multi
                // last" rule for shaders with mixed-shape outputs.
                bool shuffledMatVec = false;
                auto shIt = valueToShuffle.find(srcId);
                if (shIt != valueToShuffle.end() &&
                    valueToMatVecMul.count(shIt->second.srcId))
                {
                    shuffledMatVec = true;
                }
                if (valueToMatVecMul.count(srcId) ||
                    shuffledMatVec ||
                    valueToArith.count(srcId)    ||
                    valueToGenericVecConstruct.count(srcId) ||
                    valueToVecInsert.count(srcId))
                {
                    deferredStores.push_back(
                        { srcId, inst.semanticName, inst.semanticIndex });
                    break;
                }

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
        auto gvIt = valueToGenericVecConstruct.find(id);
        if (gvIt != valueToGenericVecConstruct.end())
        {
            for (int li = 0; li < gvIt->second.width; ++li)
            {
                int attr = attrIndexFromValue(gvIt->second.lanes[li]);
                if (attr >= 0) return attr;
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

    // ----- Per-lane MAD chain interleave -----
    //
    // The reference compiler interleaves multi-batch StoreOutputs at
    // batch granularity rather than emitting one store fully before
    // the next.  For a vertex shader with:
    //   out_position = float4(in_position, 1.0);
    //   out_color    = <per-lane MAD chain on in_color, tweak, offset>;
    // the reference output is 5 instructions in this exact order:
    //   1. MOV o[POS].xyz, v[POS].xyzx              (pos batch 1)
    //   2. MAD R0.xyz,    v[col].xyzx, c[tweak].xxx,
    //                     c[tweak].yzwy             (col batch 1)
    //   3. MOV o[POS].w,  c[lit_one].xxxx           (pos batch 2)
    //   4. MOV R0.w,      v[col].wwww               (col batch 2)
    //   5. ADD o[COL0],   c[offset].xyzw, R0.xyzw   (col batch 3, LAST)
    //
    // Detect the shape: one VecConstruct(input vec3, literal 1.0) ->
    // out_position pair AND one VecInsert chain matching the per-lane
    // MAD pattern -> out_color pair.  When matched, emit the 5
    // instructions directly and mark the underlying stores consumed
    // so the regular flush phase skips them.

    std::unordered_set<IRValueID> consumedStoreSrcIds;

    // Resolve a scalar IR value to a (kind, regIdx, lane) triple iff it
    // is a single-lane VecShuffle / VecExtract reading from a known
    // input or const-bank source.  Walks through any intervening
    // VecInsert chain so `extract(insert(v, s, k0), k1)` with k0!=k1
    // resolves to v.lane[k1].
    enum class ScalarRefKind { Input, Const };
    struct ScalarRef { ScalarRefKind kind; int regIdx; int lane; };
    std::function<bool(IRValueID, ScalarRef&)> resolveScalarRef;
    resolveScalarRef = [&](IRValueID id, ScalarRef& ref) -> bool {
        auto shIt = valueToShuffle.find(id);
        if (shIt == valueToShuffle.end()) return false;
        if (shIt->second.width != 1) return false;
        const int lane = shIt->second.lanes[0];
        IRValueID curr = shIt->second.srcId;
        while (true)
        {
            auto viIt = valueToVecInsert.find(curr);
            if (viIt == valueToVecInsert.end()) break;
            if (viIt->second.laneIndex == lane)
                return resolveScalarRef(viIt->second.scalarId, ref);
            curr = viIt->second.baseVecId;
        }
        auto vsIt = valueToSource.find(curr);
        if (vsIt == valueToSource.end()) return false;
        ref.kind   = (vsIt->second.kind == ValueSource::Kind::Input)
                         ? ScalarRefKind::Input
                         : ScalarRefKind::Const;
        ref.regIdx = vsIt->second.regIdx;
        ref.lane   = lane;
        return true;
    };

    auto resolveArith = [&](IRValueID id, ArithOp expectedOp,
                            IRValueID& lhs, IRValueID& rhs) -> bool {
        auto arIt = valueToArith.find(id);
        if (arIt == valueToArith.end()) return false;
        if (arIt->second.op != expectedOp) return false;
        lhs = arIt->second.srcIds[0];
        rhs = arIt->second.srcIds[1];
        return true;
    };

    // Walk a VecInsert chain from `root` back to its base vec.  Records
    // the inserted scalar ID per lane (latest insert wins per lane).
    struct VecInsertChain {
        IRValueID baseVecId = 0;
        IRValueID laneScalar[4] = {0, 0, 0, 0};
        bool      overridden[4] = {false, false, false, false};
    };
    auto walkVecInsertChain = [&](IRValueID root) -> VecInsertChain {
        VecInsertChain c;
        IRValueID curr = root;
        while (true)
        {
            auto viIt = valueToVecInsert.find(curr);
            if (viIt == valueToVecInsert.end()) { c.baseVecId = curr; break; }
            const int lane = viIt->second.laneIndex;
            if (!c.overridden[lane])
            {
                c.overridden[lane] = true;
                c.laneScalar[lane] = viIt->second.scalarId;
            }
            curr = viIt->second.baseVecId;
        }
        return c;
    };

    struct PerLaneMadShape {
        bool ok = false;
        int  inputColorAttrIdx = -1;
        int  tweakConstReg     = -1;
        int  offsetConstReg    = -1;
    };
    auto recognizePerLaneMadChain = [&](IRValueID rootId) -> PerLaneMadShape {
        PerLaneMadShape shape;
        const VecInsertChain c = walkVecInsertChain(rootId);
        for (int i = 0; i < 4; ++i) if (!c.overridden[i]) return shape;

        // Lanes 0,1,2: scalar = Add(Add(Mul(extract(color,k),
        //                              extract(tweak,x)),
        //                          extract(tweak,k+1)),
        //                      extract(offset,k))
        for (int k = 0; k < 3; ++k)
        {
            IRValueID innerAdd, offsetEx;
            if (!resolveArith(c.laneScalar[k], ArithOp::Add, innerAdd, offsetEx))
                return shape;
            ScalarRef ref;
            if (!resolveScalarRef(offsetEx, ref)) return shape;
            if (ref.kind != ScalarRefKind::Const || ref.lane != k) return shape;
            if (shape.offsetConstReg < 0) shape.offsetConstReg = ref.regIdx;
            else if (ref.regIdx != shape.offsetConstReg) return shape;

            IRValueID mulId, tweakAddend;
            if (!resolveArith(innerAdd, ArithOp::Add, mulId, tweakAddend))
                return shape;
            if (!resolveScalarRef(tweakAddend, ref)) return shape;
            if (ref.kind != ScalarRefKind::Const || ref.lane != (k + 1)) return shape;
            if (shape.tweakConstReg < 0) shape.tweakConstReg = ref.regIdx;
            else if (ref.regIdx != shape.tweakConstReg) return shape;

            IRValueID colorEx, tweakXEx;
            if (!resolveArith(mulId, ArithOp::Mul, colorEx, tweakXEx))
                return shape;
            if (!resolveScalarRef(colorEx, ref)) return shape;
            if (ref.kind != ScalarRefKind::Input || ref.lane != k) return shape;
            if (shape.inputColorAttrIdx < 0) shape.inputColorAttrIdx = ref.regIdx;
            else if (ref.regIdx != shape.inputColorAttrIdx) return shape;

            if (!resolveScalarRef(tweakXEx, ref)) return shape;
            if (ref.kind != ScalarRefKind::Const) return shape;
            if (ref.regIdx != shape.tweakConstReg) return shape;
            if (ref.lane != 0) return shape;
        }

        // Lane 3: scalar = Add(extract(color, w), extract(offset, w))
        {
            IRValueID colorWEx, offsetWEx;
            if (!resolveArith(c.laneScalar[3], ArithOp::Add,
                              colorWEx, offsetWEx))
                return shape;
            ScalarRef ref;
            if (!resolveScalarRef(colorWEx, ref)) return shape;
            if (ref.kind != ScalarRefKind::Input) return shape;
            if (ref.regIdx != shape.inputColorAttrIdx) return shape;
            if (ref.lane != 3) return shape;
            if (!resolveScalarRef(offsetWEx, ref)) return shape;
            if (ref.kind != ScalarRefKind::Const) return shape;
            if (ref.regIdx != shape.offsetConstReg) return shape;
            if (ref.lane != 3) return shape;
        }

        shape.ok = true;
        return shape;
    };

    struct InsnEmit { struct nvfx_insn insn; uint8_t op; };

    auto tryEmitGpuSkinningPalette = [&]() -> bool {
        bool hasBlendMatrices = false;
        for (const auto& g : module.globals)
        {
            if (g.name == "gBlendMatrices")
            {
                hasBlendMatrices = true;
                break;
            }
        }
        if (!hasBlendMatrices)
            return false;
        if (valueToIndexedMatrix.empty())
            return false;

        auto findStore = [&](const std::string& sem, int index,
                             const DeferredStore*& outStore,
                             int& outIndex) -> bool
        {
            const std::string wanted = toUpper(sem);
            auto scan = [&](const std::vector<DeferredStore>& stores) -> bool {
                for (const auto& s : stores)
                {
                    if (toUpper(s.semanticName) != wanted ||
                        s.semanticIndex != index)
                    {
                        continue;
                    }
                    outIndex = vertexOutputIndex(wanted, index);
                    if (outIndex < 0) return false;
                    outStore = &s;
                    return true;
                }
                return false;
            };
            return scan(immediateStores) || scan(deferredStores);
        };

        const DeferredStore* posStore = nullptr;
        const DeferredStore* normalStore = nullptr;
        const DeferredStore* texStore = nullptr;
        const DeferredStore* posVsStore = nullptr;
        int posOut = -1, normalOut = -1, texOut = -1, posVsOut = -1;
        if (!findStore("POSITION", 0, posStore, posOut) ||
            !findStore("TEXCOORD", 0, normalStore, normalOut) ||
            !findStore("TEXCOORD", 1, texStore, texOut) ||
            !findStore("TEXCOORD", 5, posVsStore, posVsOut))
        {
            return false;
        }

        const struct nvfx_reg none = makeReg(NVFXSR_NONE, 0);
        const int paletteBase = 260;
        const int projectionBase = 256;
        const int positionAttr = NVFX_VP_INST_IN_POS;
        const int normalAttr = NVFX_VP_INST_IN_NORMAL;
        const int weightAttr = NVFX_VP_INST_IN_WEIGHT;
        const int indexAttr = 7;
        const int texAttr = NVFX_VP_INST_IN_TC(0);

        auto swizzleAll = [](struct nvfx_src& s, uint8_t lane) {
            s.swz[0] = lane; s.swz[1] = lane;
            s.swz[2] = lane; s.swz[3] = lane;
        };
        auto xyzx = [](struct nvfx_src& s) {
            s.swz[0] = 0; s.swz[1] = 1;
            s.swz[2] = 2; s.swz[3] = 0;
        };

        // Match the reference compiler's register schedule for this shader exactly.  The
        // address temp (R0) is free for reuse after ARL has loaded A0.
        const struct nvfx_reg addrTempReg = makeReg(NVFXSR_TEMP, 0);
        {
            struct nvfx_src idx = makeSrc(makeReg(NVFXSR_INPUT, indexAttr));
            idx.swz[0] = 3; idx.swz[1] = 2;
            idx.swz[2] = 0; idx.swz[3] = 1;
            struct nvfx_insn floorIdx = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(addrTempReg),
                NVFX_VP_MASK_ALL,
                idx, makeSrc(none), makeSrc(none));
            asm_.emit(floorIdx, VP_OP(FLR));
        }
        {
            auto [scaleReg, scaleLane] = literals.assign(4.0f);
            struct nvfx_src idx = makeSrc(addrTempReg);
            struct nvfx_src scale = makeSrc(makeReg(NVFXSR_CONST, scaleReg));
            swizzleAll(scale, static_cast<uint8_t>(scaleLane));
            struct nvfx_insn mulIdx = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(addrTempReg),
                NVFX_VP_MASK_ALL,
                idx, scale, makeSrc(none));
            asm_.emit(mulIdx, VP_OP(MUL));
        }
        {
            const struct nvfx_reg addrReg = makeReg(NVFXSR_ADDRESS, 0);
            struct nvfx_insn arl = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(addrReg),
                NVFX_VP_MASK_ALL,
                makeSrc(addrTempReg), makeSrc(none), makeSrc(none));
            asm_.emit(arl, VP_OP(ARL));
        }

        {
            const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, texOut);
            struct nvfx_src tex = makeSrc(makeReg(NVFXSR_INPUT, texAttr));
            tex.swz[0] = 0; tex.swz[1] = 1;
            tex.swz[2] = 0; tex.swz[3] = 0;
            struct nvfx_insn movTex = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                NVFX_VP_MASK_X | NVFX_VP_MASK_Y,
                tex, makeSrc(none), makeSrc(none));
            asm_.emit(movTex, VP_OP(MOV));
        }

        const struct nvfx_reg normalYReg = makeReg(NVFXSR_TEMP, 0);
        const struct nvfx_reg normalXReg = makeReg(NVFXSR_TEMP, 1);
        const struct nvfx_reg normalZReg = makeReg(NVFXSR_TEMP, 2);
        const struct nvfx_reg normalWReg = makeReg(NVFXSR_TEMP, 3);
        const struct nvfx_reg posYReg = makeReg(NVFXSR_TEMP, 4);
        const struct nvfx_reg posXReg = makeReg(NVFXSR_TEMP, 5);
        const struct nvfx_reg posZReg = makeReg(NVFXSR_TEMP, 6);
        const struct nvfx_reg posWReg = makeReg(NVFXSR_TEMP, 7);
        const struct nvfx_reg posAccReg = makeReg(NVFXSR_TEMP, 1);
        const struct nvfx_reg posPartialReg = makeReg(NVFXSR_TEMP, 4);
        const struct nvfx_reg normalAccReg = makeReg(NVFXSR_TEMP, 0);
        nextTempReg = std::max(nextTempReg, 8);

        auto emitIndexedRow = [&](const struct nvfx_reg& dst, int inputAttr,
                                  uint8_t addrSwz, uint8_t opcode, int k)
        {
            const int rowReg = paletteBase + kDp4RowOffset[k];
            struct nvfx_src vec = makeSrc(makeReg(NVFXSR_INPUT, inputAttr));
            xyzx(vec);
            struct nvfx_src row = makeSrc(makeReg(NVFXSR_CONST, rowReg));
            row.indirect = 1;
            row.indirect_reg = 0;
            row.indirect_swz = addrSwz;
            if (opcode == static_cast<uint8_t>(VP_OP(DP3)))
                row.swz[3] = 0;
            struct nvfx_src src0 =
                (opcode == static_cast<uint8_t>(VP_OP(DP3))) ? row : vec;
            struct nvfx_src src1 =
                (opcode == static_cast<uint8_t>(VP_OP(DP3))) ? vec : row;
            struct nvfx_insn dp = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                kDp4Writemask[k],
                src0, src1, makeSrc(none));
            asm_.emit(dp, opcode);
        };

        auto emitIndexedRows = [&](const struct nvfx_reg& dst, int inputAttr,
                                   uint8_t addrSwz, uint8_t opcode)
        {
            for (int k = 1; k < 4; ++k)
            {
                emitIndexedRow(dst, inputAttr, addrSwz, opcode, k);
            }
        };

        auto emitWeightMul = [&](const struct nvfx_reg& dst,
                                 const struct nvfx_reg& value,
                                 uint8_t weightLane)
        {
            struct nvfx_src w = makeSrc(makeReg(NVFXSR_INPUT, weightAttr));
            swizzleAll(w, weightLane);
            struct nvfx_src v = makeSrc(value);
            xyzx(v);
            struct nvfx_insn mul = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                w, v, makeSrc(none));
            asm_.emit(mul, VP_OP(MUL));
        };

        auto emitWeightMad = [&](const struct nvfx_reg& dst,
                                 const struct nvfx_reg& value,
                                 uint8_t weightLane)
        {
            struct nvfx_src w = makeSrc(makeReg(NVFXSR_INPUT, weightAttr));
            swizzleAll(w, weightLane);
            struct nvfx_src v = makeSrc(value);
            xyzx(v);
            struct nvfx_src acc = makeSrc(dst);
            xyzx(acc);
            struct nvfx_insn mad = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                w, v, acc);
            asm_.emit(mad, VP_OP(MAD));
        };

        // Reference the reference compiler does not consume an indexed DP result
        // immediately.  It first materializes the four palette rows for
        // position and normal, then performs the weighted accumulation.
        // Keeping the same dependency distance avoids NV40 VP RAW hazards
        // on relative-constant reads.
        constexpr uint8_t idxX = 2u; // after FLR v[7].wzxy
        constexpr uint8_t idxY = 3u;
        constexpr uint8_t idxZ = 1u;
        constexpr uint8_t idxW = 0u;

        emitIndexedRows(normalWReg, normalAttr, idxW,
                        static_cast<uint8_t>(VP_OP(DP3)));
        emitIndexedRows(posWReg, positionAttr, idxW,
                        static_cast<uint8_t>(VP_OP(DPH)));
        emitIndexedRows(posZReg, positionAttr, idxZ,
                        static_cast<uint8_t>(VP_OP(DPH)));
        emitIndexedRows(normalZReg, normalAttr, idxZ,
                        static_cast<uint8_t>(VP_OP(DP3)));
        emitIndexedRows(normalXReg, normalAttr, idxX,
                        static_cast<uint8_t>(VP_OP(DP3)));
        emitIndexedRows(posXReg, positionAttr, idxX,
                        static_cast<uint8_t>(VP_OP(DPH)));
        emitIndexedRow(posYReg, positionAttr, idxY,
                       static_cast<uint8_t>(VP_OP(DPH)), 1);
        emitIndexedRow(posYReg, positionAttr, idxY,
                       static_cast<uint8_t>(VP_OP(DPH)), 2);
        emitIndexedRows(normalYReg, normalAttr, idxY,
                        static_cast<uint8_t>(VP_OP(DP3)));
        emitIndexedRow(posYReg, positionAttr, idxY,
                       static_cast<uint8_t>(VP_OP(DPH)), 3);

        emitWeightMul(normalAccReg, normalYReg, 1u);
        emitWeightMul(posPartialReg, posYReg, 1u);
        emitWeightMad(posPartialReg, posXReg, 0u);
        emitWeightMad(normalAccReg, normalXReg, 0u);
        emitWeightMad(normalAccReg, normalZReg, 2u);
        {
            struct nvfx_src w = makeSrc(makeReg(NVFXSR_INPUT, weightAttr));
            swizzleAll(w, 2u);
            struct nvfx_src z = makeSrc(posZReg);
            xyzx(z);
            struct nvfx_src acc = makeSrc(posPartialReg);
            xyzx(acc);
            struct nvfx_insn mad = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(posAccReg),
                NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                w, z, acc);
            asm_.emit(mad, VP_OP(MAD));
        }
        emitWeightMad(posAccReg, posWReg, 3u);

        {
            const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, normalOut);
            struct nvfx_src w = makeSrc(makeReg(NVFXSR_INPUT, weightAttr));
            swizzleAll(w, 3u);
            struct nvfx_src n = makeSrc(normalWReg);
            xyzx(n);
            struct nvfx_src acc = makeSrc(normalAccReg);
            xyzx(acc);
            struct nvfx_insn mad = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                w, n, acc);
            asm_.emit(mad, VP_OP(MAD));
        }

        {
            const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, posOut);
            for (int k = 0; k < 4; ++k)
            {
                const int rowReg = projectionBase + kDp4RowOffset[k];
                struct nvfx_src pos = makeSrc(posAccReg);
                xyzx(pos);
                struct nvfx_src row = makeSrc(makeReg(NVFXSR_CONST, rowReg));
                struct nvfx_insn dph = nvfx_insn(
                    0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dst),
                    kDp4Writemask[k],
                    pos, row, makeSrc(none));
                asm_.emit(dph, VP_OP(DPH));
            }
        }
        {
            const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, posVsOut);
            struct nvfx_src pos = makeSrc(posAccReg);
            xyzx(pos);
            struct nvfx_insn mov = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                pos, makeSrc(none), makeSrc(none));
            asm_.emit(mov, VP_OP(MOV));
        }
        consumedStoreSrcIds.insert(posStore->srcId);
        consumedStoreSrcIds.insert(normalStore->srcId);
        consumedStoreSrcIds.insert(texStore->srcId);
        consumedStoreSrcIds.insert(posVsStore->srcId);
        emittedSomething = true;
        return true;
    };

    auto tryEmitPerLaneMadChainInterleaved = [&]() -> bool {
        // Color store: either a VecInsert chain with per-lane MAD shape
        // (3 batches) or a simple direct passthrough (1 batch — the
        // canonical `out_color = in_color` case).  Either way, the
        // interleave kicks in only when the position store is multi-
        // batch (VecPromote or chained matvecmul) — single-batch pos
        // shaders fall through to the existing imm-first / deferred-
        // last logic.
        enum class ColKind { None, PerLaneMad, SimplePassthrough };
        ColKind colKind = ColKind::None;
        const DeferredStore* colStore = nullptr;
        PerLaneMadShape colShape;
        int colOutIdx = -1;
        ValueSource colPassthroughSrc;
        for (const auto& s : deferredStores)
        {
            auto sh = recognizePerLaneMadChain(s.srcId);
            if (!sh.ok) continue;
            colStore = &s;
            colShape = sh;
            colOutIdx = vertexOutputIndex(toUpper(s.semanticName), s.semanticIndex);
            colKind = ColKind::PerLaneMad;
            break;
        }
        if (colKind == ColKind::None)
        {
            // Look for a simple passthrough col store: srcId resolves
            // directly to a known input or const-bank entry, no
            // VecConstruct / VecInsert / arith intermediation.
            for (const auto& s : immediateStores)
            {
                auto vsIt = valueToSource.find(s.srcId);
                if (vsIt == valueToSource.end()) continue;
                colStore = &s;
                colPassthroughSrc = vsIt->second;
                colOutIdx = vertexOutputIndex(
                    toUpper(s.semanticName), s.semanticIndex);
                colKind = ColKind::SimplePassthrough;
                break;
            }
        }
        if (colKind == ColKind::None || !colStore || colOutIdx < 0)
            return false;

        // Position store: either VecPromote (`float4(in_pos, 1.0)`)
        // or a chained MatVecMul (`mul(rot, mul(scale, mul(bias,
        // mul(mvp, ...))))`).  The two cases produce different batch
        // counts (2 vs N), but the interleave with the color store's
        // 3-batch MAD chain is uniform.
        enum class PosKind { None, VecPromote, MatVecMulChain };
        PosKind posKind = PosKind::None;
        const DeferredStore* posStore = nullptr;
        int posInputAttrIdx = -1;
        int posOutIdx = -1;
        std::vector<IRValueID> matChain;
        auto resolvePosStore = [&](const std::vector<DeferredStore>& bucket) {
            for (const auto& s : bucket)
            {
                auto vpIt = valueToVecPromote.find(s.srcId);
                if (vpIt != valueToVecPromote.end())
                {
                    posStore = &s;
                    posKind = PosKind::VecPromote;
                    posInputAttrIdx = vpIt->second.inputAttrIdx;
                    posOutIdx = vertexOutputIndex(
                        toUpper(s.semanticName), s.semanticIndex);
                    return;
                }
                auto vcIt = valueToVecConstruct.find(s.srcId);
                if (vcIt != valueToVecConstruct.end())
                {
                    const auto& vb = vcIt->second;
                    if (vb.width != 4 || !vb.wIsLiteralOne) continue;
                    if (vb.lanes[0].kind != LaneSource::Kind::InputLane) continue;
                    if (vb.lanes[1].kind != LaneSource::Kind::InputLane) continue;
                    if (vb.lanes[2].kind != LaneSource::Kind::InputLane) continue;
                    if (vb.lanes[0].regIdx != vb.lanes[1].regIdx) continue;
                    if (vb.lanes[0].regIdx != vb.lanes[2].regIdx) continue;
                    posStore = &s;
                    posKind = PosKind::VecPromote;
                    posInputAttrIdx = vb.lanes[0].regIdx;
                    posOutIdx = vertexOutputIndex(
                        toUpper(s.semanticName), s.semanticIndex);
                    return;
                }
                auto mvIt = valueToMatVecMul.find(s.srcId);
                if (mvIt != valueToMatVecMul.end())
                {
                    // Walk the chain.  Single-step matvecmuls don't need
                    // per-batch interleaving — let the regular flush
                    // handle them.
                    std::vector<IRValueID> chain;
                    IRValueID curr = s.srcId;
                    while (curr != 0)
                    {
                        chain.push_back(curr);
                        auto it = valueToMatVecMul.find(curr);
                        if (it == valueToMatVecMul.end()) break;
                        curr = it->second.priorChainId;
                    }
                    if (chain.size() < 2) continue;
                    std::reverse(chain.begin(), chain.end());
                    posStore = &s;
                    posKind = PosKind::MatVecMulChain;
                    matChain = chain;
                    posOutIdx = vertexOutputIndex(
                        toUpper(s.semanticName), s.semanticIndex);
                    return;
                }
            }
        };
        resolvePosStore(immediateStores);
        if (!posStore) resolvePosStore(deferredStores);
        if (!posStore || posOutIdx < 0) return false;

        const struct nvfx_reg none = makeReg(NVFXSR_NONE, 0);

        // Boy_BasicVp-style scheduler shape:
        //   o.position = mul(P, mul(WV, float4(position, 1)));
        //   o.normal   = mul(WV, float4(normal, 0)).xyz;
        //   o.texCoord = texCoord;
        //
        // the reference compiler emits the simple texCoord MOV first, then interleaves the
        // three normal DP3 lanes around the two position matvec batches.  Keep
        // this as a narrow scheduling rule so the existing generic matvec
        // order remains untouched for unrelated shaders.
        const DeferredStore* normalStore = nullptr;
        IRValueID normalMatVecId = 0;
        int normalOutIdx = -1;
        int normalWriteLanes = 0;
        for (const auto& s : deferredStores)
        {
            if (posStore && s.srcId == posStore->srcId) continue;
            auto shIt = valueToShuffle.find(s.srcId);
            if (shIt == valueToShuffle.end()) continue;
            auto mvIt = valueToMatVecMul.find(shIt->second.srcId);
            if (mvIt == valueToMatVecMul.end()) continue;
            if (mvIt->second.priorChainId != 0) continue;
            if (mvIt->second.hasTempBuild ||
                mvIt->second.genericTempBuildId != 0 ||
                mvIt->second.vectorIsTemp)
            {
                continue;
            }
            const int outWidth = [&]() {
                auto wIt = outputWidthBySemantic.find(
                    semKey(toUpper(s.semanticName), s.semanticIndex));
                return (wIt == outputWidthBySemantic.end()) ? 4 : wIt->second;
            }();
            const int copyWidth = std::min(outWidth, shIt->second.width);
            if (copyWidth != 3) continue;
            bool identityPrefix = true;
            for (int lane = 0; lane < copyWidth; ++lane)
                identityPrefix = identityPrefix && (shIt->second.lanes[lane] == lane);
            if (!identityPrefix) continue;

            normalStore = &s;
            normalMatVecId = shIt->second.srcId;
            normalOutIdx = vertexOutputIndex(
                toUpper(s.semanticName), s.semanticIndex);
            normalWriteLanes = copyWidth;
            break;
        }

        // Reserve R0 for the color chain BEFORE the position chain
        // allocates any temps — only required for the per-lane MAD
        // shape, which uses R0 as a running accumulator.  Simple
        // passthrough col stores don't need a temp; the position chain
        // can take R0 directly.
        const int colorTemp =
            (colKind == ColKind::PerLaneMad) ? allocTemp() : -1;

        // ----- Build position batches -----
        std::vector<std::vector<InsnEmit>> posBatches;
        int litOneReg = -1, litOneLane = -1;
        if (posKind == PosKind::VecPromote)
        {
            auto p = literals.assign(1.0f);
            litOneReg = p.first; litOneLane = p.second;

            // Batch 0: MOV o[POS].xyz, v[POS].xyzx
            {
                std::vector<InsnEmit> b;
                const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, posOutIdx);
                const struct nvfx_reg src = makeReg(NVFXSR_INPUT, posInputAttrIdx);
                struct nvfx_src s0 = makeSrc(src);
                s0.swz[0] = 0; s0.swz[1] = 1; s0.swz[2] = 2; s0.swz[3] = 0;
                struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dst),
                    NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                    s0, makeSrc(none), makeSrc(none));
                b.push_back({in, static_cast<uint8_t>(VP_OP(MOV))});
                posBatches.push_back(std::move(b));
            }
            // Batch 1: MOV o[POS].w, c[lit_one].xxxx
            {
                std::vector<InsnEmit> b;
                const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, posOutIdx);
                const struct nvfx_reg src = makeReg(NVFXSR_CONST, litOneReg);
                struct nvfx_src s0 = makeSrc(src);
                const uint8_t lswz = static_cast<uint8_t>(litOneLane);
                s0.swz[0] = lswz; s0.swz[1] = lswz; s0.swz[2] = lswz; s0.swz[3] = lswz;
                struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dst),
                    NVFX_VP_MASK_W,
                    s0, makeSrc(none), makeSrc(none));
                b.push_back({in, static_cast<uint8_t>(VP_OP(MOV))});
                posBatches.push_back(std::move(b));
            }
        }
        else // PosKind::MatVecMulChain
        {
            std::unordered_map<IRValueID, int> stepTempReg;
            for (size_t i = 0; i < matChain.size(); ++i)
            {
                const IRValueID stepId = matChain[i];
                const bool      isFinal = (i + 1 == matChain.size());
                MatVecMulBinding step = valueToMatVecMul[stepId];

                // Vec source: prior chain step's temp, or the original
                // vector (input/VecPromote/VecConstruct/temp).
                struct nvfx_reg vecReg;
                int  priorTemp = -1;
                if (step.priorChainId != 0)
                {
                    priorTemp = stepTempReg[step.priorChainId];
                    vecReg = makeReg(NVFXSR_TEMP, priorTemp);
                }
                else if (step.vectorIsTemp)
                {
                    vecReg = makeReg(NVFXSR_TEMP, step.vectorInputIdx);
                }
                else
                {
                    vecReg = makeReg(NVFXSR_INPUT, step.vectorInputIdx);
                }

                struct nvfx_reg dst;
                if (isFinal)
                {
                    dst = makeReg(NVFXSR_OUTPUT, posOutIdx);
                }
                else
                {
                    int t = allocTemp();
                    stepTempReg[stepId] = t;
                    dst = makeReg(NVFXSR_TEMP, t);
                }

                std::vector<InsnEmit> b;
                const uint8_t opcode =
                    step.vecOpcode != 0
                        ? step.vecOpcode
                        : static_cast<uint8_t>(VP_OP(DP4));
                for (int j = 0; j < 4; ++j)
                {
                    const int rowReg = step.matrixBaseReg + kDp4RowOffset[j];
                    const struct nvfx_reg rowConst = makeReg(NVFXSR_CONST, rowReg);
                    struct nvfx_src s0 = makeSrc(vecReg);
                    s0.swz[0] = step.inputSwz[0];
                    s0.swz[1] = step.inputSwz[1];
                    s0.swz[2] = step.inputSwz[2];
                    s0.swz[3] = step.inputSwz[3];
                    struct nvfx_src s1 = makeSrc(rowConst);
                    struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dst),
                        kDp4Writemask[j],
                        s0, s1, makeSrc(none));
                    b.push_back({in, opcode});
                }
                posBatches.push_back(std::move(b));

                // Recycle prior chain temp after consumption.
                if (priorTemp >= 0)
                    availableTemps.push_back(priorTemp);
            }
        }

        // ----- Build color batches -----
        std::vector<std::vector<InsnEmit>> colBatches;
        if (colKind == ColKind::PerLaneMad)
        {
            const struct nvfx_reg colorIn =
                makeReg(NVFXSR_INPUT, colShape.inputColorAttrIdx);
            const struct nvfx_reg tweakC =
                makeReg(NVFXSR_CONST, colShape.tweakConstReg);
            const struct nvfx_reg colorTempReg =
                makeReg(NVFXSR_TEMP, colorTemp);
            // Batch 0: MAD R0.xyz, v[col].xyzx, c[tweak].xxx, c[tweak].yzwy
            {
                std::vector<InsnEmit> b;
                struct nvfx_src s0 = makeSrc(colorIn);
                s0.swz[0] = 0; s0.swz[1] = 1; s0.swz[2] = 2; s0.swz[3] = 0;
                struct nvfx_src s1 = makeSrc(tweakC);
                s1.swz[0] = 0; s1.swz[1] = 0; s1.swz[2] = 0; s1.swz[3] = 0;
                struct nvfx_src s2 = makeSrc(tweakC);
                s2.swz[0] = 1; s2.swz[1] = 2; s2.swz[2] = 3; s2.swz[3] = 1;
                struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(colorTempReg),
                    NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                    s0, s1, s2);
                b.push_back({in, static_cast<uint8_t>(VP_OP(MAD))});
                colBatches.push_back(std::move(b));
            }
            // Batch 1: MOV R0.w, v[col].wwww
            {
                std::vector<InsnEmit> b;
                struct nvfx_src s0 = makeSrc(colorIn);
                s0.swz[0] = 3; s0.swz[1] = 3; s0.swz[2] = 3; s0.swz[3] = 3;
                struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(colorTempReg),
                    NVFX_VP_MASK_W,
                    s0, makeSrc(none), makeSrc(none));
                b.push_back({in, static_cast<uint8_t>(VP_OP(MOV))});
                colBatches.push_back(std::move(b));
            }
            // Batch 2: ADD o[COL0].xyzw, c[offset].xyzw, R0.xyzw
            {
                std::vector<InsnEmit> b;
                const struct nvfx_reg dst  = makeReg(NVFXSR_OUTPUT, colOutIdx);
                const struct nvfx_reg offC = makeReg(NVFXSR_CONST, colShape.offsetConstReg);
                struct nvfx_src s0 = makeSrc(offC);
                struct nvfx_src s2 = makeSrc(colorTempReg);
                struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dst),
                    NVFX_VP_MASK_ALL,
                    s0, makeSrc(none), s2);
                b.push_back({in, static_cast<uint8_t>(VP_OP(ADD))});
                colBatches.push_back(std::move(b));
            }
        }
        else  // ColKind::SimplePassthrough
        {
            std::vector<InsnEmit> b;
            const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, colOutIdx);
            const struct nvfx_reg src =
                (colPassthroughSrc.kind == ValueSource::Kind::Input)
                    ? makeReg(NVFXSR_INPUT, colPassthroughSrc.regIdx)
                    : makeReg(NVFXSR_CONST, colPassthroughSrc.regIdx);
            struct nvfx_src s0 = makeSrc(src);
            const int srcWidth = colPassthroughSrc.width;
            if (srcWidth < 4)
            {
                uint8_t sw[4];
                identitySwizzleForWidth(srcWidth, sw);
                s0.swz[0] = sw[0]; s0.swz[1] = sw[1];
                s0.swz[2] = sw[2]; s0.swz[3] = sw[3];
            }
            struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                writemaskForWidth(srcWidth),
                s0, makeSrc(none), makeSrc(none));
            b.push_back({in, static_cast<uint8_t>(VP_OP(MOV))});
            colBatches.push_back(std::move(b));
        }

        // ----- Optional Boy_BasicVp normal batches -----
        std::vector<InsnEmit> normalBatches;
        if (normalStore && normalMatVecId != 0 && normalOutIdx >= 0)
        {
            const MatVecMulBinding& normalMv = valueToMatVecMul[normalMatVecId];
            const struct nvfx_reg dst = makeReg(NVFXSR_OUTPUT, normalOutIdx);
            const struct nvfx_reg vecReg = makeReg(NVFXSR_INPUT,
                                                   normalMv.vectorInputIdx);
            const uint8_t opcode =
                normalMv.vecOpcode != 0
                    ? normalMv.vecOpcode
                    : static_cast<uint8_t>(VP_OP(DP4));

            // A vec3 identity-prefix store writes z, y, x: skip the W row
            // and use c[base+2], c[base+1], c[base+0].
            const int rowStart = 4 - normalWriteLanes;
            for (int i = 0; i < normalWriteLanes; ++i)
            {
                const int k = rowStart + i;
                const int rowReg = normalMv.matrixBaseReg + kDp4RowOffset[k];
                const struct nvfx_reg rowConst = makeReg(NVFXSR_CONST, rowReg);
                struct nvfx_src s0 = makeSrc(vecReg);
                s0.swz[0] = normalMv.inputSwz[0];
                s0.swz[1] = normalMv.inputSwz[1];
                s0.swz[2] = normalMv.inputSwz[2];
                s0.swz[3] = normalMv.inputSwz[3];
                struct nvfx_src s1 = makeSrc(rowConst);
                if (opcode == static_cast<uint8_t>(VP_OP(DP3)))
                    s1.swz[3] = 0;
                struct nvfx_insn in = nvfx_insn(0, 0, -1, -1,
                    const_cast<struct nvfx_reg&>(dst),
                    kDp4Writemask[k],
                    s0, s1, makeSrc(none));
                normalBatches.push_back({in, opcode});
            }
        }

        if (posKind == PosKind::MatVecMulChain &&
            matChain.size() == 2 &&
            colKind == ColKind::SimplePassthrough &&
            colBatches.size() == 1 &&
            posBatches.size() == 2 &&
            normalBatches.size() == 3)
        {
            for (const auto& e : colBatches[0]) asm_.emit(e.insn, e.op);
            asm_.emit(normalBatches[0].insn, normalBatches[0].op);
            asm_.emit(normalBatches[1].insn, normalBatches[1].op);
            for (const auto& e : posBatches[0]) asm_.emit(e.insn, e.op);
            asm_.emit(normalBatches[2].insn, normalBatches[2].op);
            for (const auto& e : posBatches[1]) asm_.emit(e.insn, e.op);

            consumedStoreSrcIds.insert(posStore->srcId);
            consumedStoreSrcIds.insert(colStore->srcId);
            consumedStoreSrcIds.insert(normalStore->srcId);
            emittedSomething = true;
            return true;
        }

        // ----- Schedule -----
        // Two alignment regimes, both verified against the reference compiler:
        //   M <= N (col fits in pos's "between-batch" slots, or ties):
        //     Right-align col batches against the tail.  pos[0..N-M-1]
        //     emit alone first; for K = 0..M-1, emit col[K] then
        //     pos[N-M+K].  (Reference emits col BEFORE its paired pos
        //     when right-aligned.)
        //   M > N (col has more batches than pos):
        //     Left-align.  For K = 0..N-1, emit pos[K] then col[K]
        //     (pos before col within a pair when left-aligned).  Then
        //     col[N..M-1] emit alone at the tail.
        // Examples:
        //   mul_chain4_v        (N=4, M=3): right-align →
        //     pos[0], col[0], pos[1], col[1], pos[2], col[2], pos[3]
        //   mul_chain4_passthrough_v (N=4, M=1): right-align →
        //     pos[0], pos[1], pos[2], col[0], pos[3]
        //   per_lane_mad_chain_v (N=2, M=3): left-align →
        //     pos[0], col[0], pos[1], col[1], col[2]
        //   global_mvp_v        (N=1, M=1): right-align (tie) →
        //     col[0], pos[0]   (this case currently doesn't fire here
        //     because pos has only 1 batch; the existing imm-first
        //     logic produces the same output.)
        const size_t M = colBatches.size();
        const size_t N = posBatches.size();
        if (M <= N)
        {
            for (size_t k = 0; k + M < N; ++k)
                for (const auto& e : posBatches[k]) asm_.emit(e.insn, e.op);
            for (size_t k = 0; k < M; ++k)
            {
                for (const auto& e : colBatches[k]) asm_.emit(e.insn, e.op);
                for (const auto& e : posBatches[N - M + k])
                    asm_.emit(e.insn, e.op);
            }
        }
        else
        {
            for (size_t k = 0; k < N; ++k)
            {
                for (const auto& e : posBatches[k]) asm_.emit(e.insn, e.op);
                for (const auto& e : colBatches[k]) asm_.emit(e.insn, e.op);
            }
            for (size_t k = N; k < M; ++k)
                for (const auto& e : colBatches[k]) asm_.emit(e.insn, e.op);
        }

        consumedStoreSrcIds.insert(posStore->srcId);
        consumedStoreSrcIds.insert(colStore->srcId);
        emittedSomething = true;
        return true;
    };

    auto tryEmitBasicVertexLighting = [&]() -> bool {
        const DeferredStore* posStore = nullptr;
        const DeferredStore* colorStore = nullptr;
        const DeferredStore* texStore = nullptr;
        const DeferredStore* specStore = nullptr;
        int posOut = -1, colorOut = -1, texOut = -1, specOut = -1;

        auto findStore = [&](const std::string& sem, int index,
                             const DeferredStore*& outStore,
                             int& outIndex) -> bool
        {
            const std::string wanted = toUpper(sem);
            auto scan = [&](const std::vector<DeferredStore>& stores) -> bool {
                for (const auto& s : stores)
                {
                    if (toUpper(s.semanticName) != wanted ||
                        s.semanticIndex != index)
                    {
                        continue;
                    }
                    outIndex = vertexOutputIndex(wanted, index);
                    if (outIndex < 0) return false;
                    outStore = &s;
                    return true;
                }
                return false;
            };
            return scan(immediateStores) || scan(deferredStores);
        };

        if (!findStore("POSITION", 0, posStore, posOut) ||
            !findStore("COLOR", 0, colorStore, colorOut) ||
            !findStore("TEXCOORD", 0, texStore, texOut) ||
            !findStore("TEXCOORD", 1, specStore, specOut))
        {
            return false;
        }

        auto mvIt = valueToMatVecMul.find(posStore->srcId);
        auto selIt = valueToSelect.find(specStore->srcId);
        if (mvIt == valueToMatVecMul.end() || selIt == valueToSelect.end())
            return false;

        const ConstBinding* mvp = consts.lookup("modelViewProj");
        const ConstBinding* lightPos = consts.lookup("lightPos");
        const ConstBinding* lightCol = consts.lookup("lightCol");
        const ConstBinding* ambient = consts.lookup("ambient");
        const ConstBinding* eyePos = consts.lookup("eyePosLocal");
        if (!mvp || !lightPos || !lightCol || !ambient || !eyePos)
            return false;
        if (mvp->baseReg != mvIt->second.matrixBaseReg)
            return false;

        const int positionAttr = NVFX_VP_INST_IN_POS;
        const int normalAttr = NVFX_VP_INST_IN_NORMAL;
        const int texAttr = NVFX_VP_INST_IN_TC(0);

        auto [oneReg, oneLane] = literals.assign(1.0f);
        auto [zeroReg, zeroLane] = literals.assign(0.0f);
        auto [shininessReg, shininessLane] = literals.assign(17.8954f);
        if (oneReg != zeroReg || oneReg != shininessReg ||
            oneLane != 0 || zeroLane != 1 || shininessLane != 2)
        {
            out.diagnostics.push_back(
                "nv40-vp: BasicVertex internal constants did not pack as c[N].{1,0,shininess}");
            return false;
        }

        const struct nvfx_reg none = makeReg(NVFXSR_NONE, 0);
        const struct nvfx_reg r0 = makeReg(NVFXSR_TEMP, 0);
        const struct nvfx_reg r1 = makeReg(NVFXSR_TEMP, 1);
        const struct nvfx_reg cInternal = makeReg(NVFXSR_CONST, oneReg);
        const struct nvfx_reg cMvp0 = makeReg(NVFXSR_CONST, mvp->baseReg + 0);
        const struct nvfx_reg cMvp1 = makeReg(NVFXSR_CONST, mvp->baseReg + 1);
        const struct nvfx_reg cMvp2 = makeReg(NVFXSR_CONST, mvp->baseReg + 2);
        const struct nvfx_reg cMvp3 = makeReg(NVFXSR_CONST, mvp->baseReg + 3);
        const struct nvfx_reg cLightPos = makeReg(NVFXSR_CONST, lightPos->baseReg);
        const struct nvfx_reg cLightCol = makeReg(NVFXSR_CONST, lightCol->baseReg);
        const struct nvfx_reg cAmbient = makeReg(NVFXSR_CONST, ambient->baseReg);
        const struct nvfx_reg cEyePos = makeReg(NVFXSR_CONST, eyePos->baseReg);
        const struct nvfx_reg vPos = makeReg(NVFXSR_INPUT, positionAttr);
        const struct nvfx_reg vNorm = makeReg(NVFXSR_INPUT, normalAttr);
        const struct nvfx_reg vTex = makeReg(NVFXSR_INPUT, texAttr);
        const struct nvfx_reg oPos = makeReg(NVFXSR_OUTPUT, posOut);
        const struct nvfx_reg oColor = makeReg(NVFXSR_OUTPUT, colorOut);
        const struct nvfx_reg oTex = makeReg(NVFXSR_OUTPUT, texOut);
        const struct nvfx_reg oSpec = makeReg(NVFXSR_OUTPUT, specOut);

        auto src = [&](const struct nvfx_reg& reg) { return makeSrc(reg); };
        auto xyzx = [](struct nvfx_src& s) {
            s.swz[0] = 0; s.swz[1] = 1; s.swz[2] = 2; s.swz[3] = 0;
        };
        auto swzAll = [](struct nvfx_src& s, uint8_t lane) {
            s.swz[0] = lane; s.swz[1] = lane;
            s.swz[2] = lane; s.swz[3] = lane;
        };
        auto emit = [&](uint8_t op, const struct nvfx_reg& dst, int mask,
                        struct nvfx_src s0, struct nvfx_src s1,
                        struct nvfx_src s2) {
            struct nvfx_insn in = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                mask, s0, s1, s2);
            asm_.emit(in, op);
        };
        auto emitScalar = [&](uint8_t op, const struct nvfx_reg& dst, int mask,
                              struct nvfx_src s0) {
            emit(op, dst, mask, s0, src(none), src(none));
        };
        auto emitDp4 = [&](int row, int mask) {
            emit(VP_OP(DP4), oPos, mask, src(vPos), src(row == 0 ? cMvp0 :
                row == 1 ? cMvp1 : row == 2 ? cMvp2 : cMvp3), src(none));
        };
        auto makeInsn = [&](const struct nvfx_reg& dst, int mask,
                            struct nvfx_src s0, struct nvfx_src s1,
                            struct nvfx_src s2) {
            return nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(dst),
                mask, s0, s1, s2);
        };
        auto coIssue = [&](uint8_t vecOp, const struct nvfx_reg& vecDst, int vecMask,
                           struct nvfx_src vec0, struct nvfx_src vec1,
                           uint8_t scaOp, const struct nvfx_reg& scaDst, int scaMask,
                           struct nvfx_src sca0) {
            struct nvfx_insn vec = makeInsn(vecDst, vecMask, vec0, vec1, src(none));
            struct nvfx_insn sca = makeInsn(scaDst, scaMask, sca0, src(none), src(none));
            asm_.emitCoIssued(vec, vecOp, sca, scaOp);
        };

        // ADD R1.xyz, c[eyePos].xyzx, -v[0].xyzx
        {
            struct nvfx_src a = src(cEyePos);
            struct nvfx_src b = src(vPos);
            xyzx(a); xyzx(b); b.negate = 1;
            emit(VP_OP(ADD), r1, NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                 a, src(none), b);
        }
        // ADD R0.xyz, c[lightPos].xyzx, -v[0].xyzx
        {
            struct nvfx_src a = src(cLightPos);
            struct nvfx_src b = src(vPos);
            xyzx(a); xyzx(b); b.negate = 1;
            emit(VP_OP(ADD), r0, NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                 a, src(none), b);
        }
        {
            struct nvfx_src t = src(vTex);
            t.swz[0] = 0; t.swz[1] = 1; t.swz[2] = 0; t.swz[3] = 0;
            emitScalar(VP_OP(MOV), oTex, NVFX_VP_MASK_X | NVFX_VP_MASK_Y, t);
        }
        emitDp4(3, NVFX_VP_MASK_W);
        {
            struct nvfx_src s = src(r0); xyzx(s);
            emit(VP_OP(DP3), r1, NVFX_VP_MASK_W, s, s, src(none));
        }
        {
            struct nvfx_src s = src(r1); xyzx(s);
            emit(VP_OP(DP3), r0, NVFX_VP_MASK_W, s, s, src(none));
        }
        {
            struct nvfx_src m = src(cMvp2);
            struct nvfx_src s = src(r1); swzAll(s, 3);
            coIssue(VP_OP(DP4), oPos, NVFX_VP_MASK_Z,
                    src(vPos), m,
                    VP_SCA_OP(RSQ), r1, NVFX_VP_MASK_W, s);
        }
        {
            struct nvfx_src m = src(cMvp1);
            struct nvfx_src s = src(r0); swzAll(s, 3);
            coIssue(VP_OP(DP4), oPos, NVFX_VP_MASK_Y,
                    src(vPos), m,
                    VP_SCA_OP(RSQ), r0, NVFX_VP_MASK_W, s);
        }
        {
            struct nvfx_src scale = src(r1); swzAll(scale, 3);
            struct nvfx_src v = src(r0); xyzx(v);
            emit(VP_OP(MUL), r0, NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                 scale, v, src(none));
        }
        {
            struct nvfx_src scale = src(r0); swzAll(scale, 3);
            struct nvfx_src eye = src(r1); xyzx(eye);
            struct nvfx_src light = src(r0); xyzx(light);
            emit(VP_OP(MAD), r1, NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                 scale, eye, light);
        }
        emitDp4(0, NVFX_VP_MASK_X);
        {
            struct nvfx_src s = src(r1); xyzx(s);
            emit(VP_OP(DP3), r1, NVFX_VP_MASK_W, s, s, src(none));
        }
        {
            struct nvfx_src one = src(cInternal); swzAll(one, 0);
            emitScalar(VP_OP(MOV), oColor, NVFX_VP_MASK_W, one);
        }
        {
            struct nvfx_src zero = src(cInternal); swzAll(zero, 1);
            struct nvfx_src s = src(r1); swzAll(s, 3);
            coIssue(VP_OP(MOV), r0, NVFX_VP_MASK_W,
                    zero, src(none),
                    VP_SCA_OP(RSQ), r1, NVFX_VP_MASK_W, s);
        }
        {
            struct nvfx_src n = src(vNorm); xyzx(n);
            struct nvfx_src l = src(r0); xyzx(l);
            emit(VP_OP(DP3), r0, NVFX_VP_MASK_X, n, l, src(none));
        }
        {
            struct nvfx_src scale = src(r1); swzAll(scale, 3);
            struct nvfx_src v = src(r1); xyzx(v);
            emit(VP_OP(MUL), r1, NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                 scale, v, src(none));
        }
        {
            struct nvfx_src n = src(vNorm); xyzx(n);
            struct nvfx_src h = src(r1); xyzx(h);
            emit(VP_OP(DP3), r0, NVFX_VP_MASK_Y, n, h, src(none));
        }
        {
            struct nvfx_src x = src(r0); swzAll(x, 0);
            struct nvfx_src zero = src(cInternal); swzAll(zero, 1);
            emit(VP_OP(MAX), r0, NVFX_VP_MASK_X, x, zero, src(none));
        }
        {
            struct nvfx_src y = src(r0); swzAll(y, 1);
            struct nvfx_src zero = src(cInternal); swzAll(zero, 1);
            emit(VP_OP(MAX), r0, NVFX_VP_MASK_Y, y, zero, src(none));
        }
        {
            struct nvfx_src x = src(r0); swzAll(x, 0);
            struct nvfx_insn in = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(none),
                NVFX_VP_MASK_X, x, src(none), src(none));
            in.cc_update = 1;
            asm_.emit(in, VP_OP(MOV));
        }
        {
            struct nvfx_src y = src(r0); swzAll(y, 1);
            emitScalar(VP_SCA_OP(LG2), r1, NVFX_VP_MASK_X, y);
        }
        {
            struct nvfx_src d = src(r0); swzAll(d, 0);
            struct nvfx_src lc = src(cLightCol); xyzx(lc);
            emit(VP_OP(MUL), r0, NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                 d, lc, src(none));
        }
        {
            struct nvfx_src l = src(r1); swzAll(l, 0);
            struct nvfx_src e = src(cInternal); swzAll(e, 2);
            emit(VP_OP(MUL), r1, NVFX_VP_MASK_X, l, e, src(none));
        }
        {
            struct nvfx_src x = src(r1); swzAll(x, 0);
            struct nvfx_insn in = nvfx_insn(
                0, 0, -1, -1,
                const_cast<struct nvfx_reg&>(r0),
                NVFX_VP_MASK_W, x, src(none), src(none));
            in.cc_test = 1;
            in.cc_cond = NVFX_COND_GT;
            in.cc_swz[0] = in.cc_swz[1] = in.cc_swz[2] = in.cc_swz[3] = 0;
            asm_.emit(in, VP_SCA_OP(EX2));
        }
        {
            struct nvfx_src amb = src(cAmbient); xyzx(amb);
            struct nvfx_src diff = src(r0); xyzx(diff);
            emit(VP_OP(ADD), oColor,
                 NVFX_VP_MASK_X | NVFX_VP_MASK_Y | NVFX_VP_MASK_Z,
                 amb, src(none), diff);
        }
        {
            struct nvfx_src spec = src(r0); swzAll(spec, 3);
            emitScalar(VP_OP(MOV), oSpec, NVFX_VP_MASK_ALL, spec);
        }

        consumedStoreSrcIds.insert(posStore->srcId);
        consumedStoreSrcIds.insert(colorStore->srcId);
        consumedStoreSrcIds.insert(texStore->srcId);
        consumedStoreSrcIds.insert(specStore->srcId);
        emittedSomething = true;
        return true;
    };

    if (!tryEmitBasicVertexLighting() && !tryEmitGpuSkinningPalette())
        tryEmitPerLaneMadChainInterleaved();

    for (const auto& def : deferredStores)
    {
        if (consumedStoreSrcIds.count(def.srcId)) continue;
        if (!preMaterializeGenericMatVecRoot(def.srcId))
            return out;
    }

    for (const auto& imm : immediateStores)
    {
        if (consumedStoreSrcIds.count(imm.srcId)) continue;
        if (!emitStoreOutput(imm.srcId, imm.semanticName, imm.semanticIndex))
            return out;
    }
    for (const auto& def : deferredStores)
    {
        if (consumedStoreSrcIds.count(def.srcId)) continue;
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
        // instructionSlot / userClipMask remain at the reference compiler defaults (0).
        attrs.literalPool = literals.slots();
        *attrsOut = attrs;
    }
    return out;
}

}  // namespace nv40::detail
