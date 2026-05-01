/*
 * NV40 fragment-program lowering.
 *
 * Walks the IRFunction, resolves IR value IDs to their NV40 FP
 * input-source codes, and emits the ucode stream via FpAssembler.
 * Handles identity varying passthrough, arithmetic, TXP, samplerCube,
 * and Select lowering.
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

    // N-ary arithmetic chain: e.g. `vcol + a + b + c` (3 adds, one root
    // varying, three uniform addends).  sce-cgc lowers this with an
    // fp16 preload then a sequence of ADDR R0 = (prev_temp) + c[inline]:
    //
    //   MOVH H0.xyzw, f[input]                           # preload root
    //   ADDR R0.xyzw, H0.xyzw, c[u0]   <16-byte zero>    # link 0
    //   ADDR R0.xyzw, R0.xyzw, c[u1]   <16-byte zero>    # link 1
    //   ADDR R0.xyzw [END], R0.xyzw, c[uN]<16-byte zero> # last link carries END
    //
    // Per-link op currently restricted to Add — chained Mul / mixed-op
    // chains lower to the existing arith path one at a time.  Each
    // link's RHS must be a uniform; chain-of-temps and chain-of-
    // varyings land later (NV40 FP only allows one INPUT_SRC per
    // instruction, so a `varying + varying` chain link needs its own
    // preload sequence).
    struct FpChainBinding
    {
        IRValueID rootInputId   = 0;     // base varying preloaded into H0
        bool      rootInputAbs  = false;
        bool      rootInputNeg  = false;
        struct Link
        {
            FpArithOp op;
            IRValueID uniformId   = 0;
            bool      uniformAbs  = false;
            bool      uniformNeg  = false;
        };
        std::vector<Link> links;
    };
    std::unordered_map<IRValueID, FpChainBinding> valueToChain;

    // Repeated-add fold: `vcol + vcol + vcol` (3 SSA references to the
    // same varying) collapses to `MAD R0 = H0 * 2.0 + H0` — i.e. a
    // 2-insn MOVH+MAD shape with a single inline literal block holding
    // (scale-1, 0, 0, 0).  Built up as the IR walks consecutive Adds:
    //   Add(x, x)       → scale=2, varyingId=x
    //   Add(scale, x)   → scale+=1
    // Future: handle the `0 + x + x + x` shape produced by static-for
    // unroll once an algebraic-simplification pass runs `0+x → x`.
    struct FpScaleVaryingBinding
    {
        IRValueID inputId = 0;   // the repeated varying (resolves via valueToInputSrc)
        int       scale   = 0;   // ≥ 2; literal multiplier in (scale-1, 0, 0, 0)
        int       lane    = -1;  // -1 = full vec4; 0..3 = scalar lane (.x/.y/.z/.w)
    };
    std::unordered_map<IRValueID, FpScaleVaryingBinding> valueToScale;

    // `float4(scale_scalar, k1, k2, k3)` — VecConstruct that mixes a
    // scalar scale-fold result with float constants in the other
    // lanes.  Lowers to a 3-insn shape (single-lane MOVH preload,
    // lane-elided MOVR for the constant lanes, single-lane MAD for
    // the scaled lane); see the StoreOutput handler for details.
    struct FpScaledLanesBinding
    {
        FpScaleVaryingBinding scale;
        int                   scaledLane = 0;     // which output lane gets the MAD
        float                 consts[4]  = {0,0,0,0}; // ignored at scaledLane
    };
    std::unordered_map<IRValueID, FpScaledLanesBinding> valueToScaledLanes;

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

    // `Mul(varying, tex2D_result)` — the THEN-branch shape inside a
    // uniform-conditional if-only:
    //   if (uniform != 0.0) c = c * tex2D(s, uv);
    // The reference compiler doesn't compute the product unconditionally
    // — it folds it into the conditional MUL itself (cc_cond=NE.<lane>).
    // We record the operand pair so the consuming Select dispatch can
    // emit the 5-instruction CC-blend pattern.
    struct FpVaryingTexMulBinding
    {
        IRValueID varyingId = 0;   // resolves via valueToInputSrc
        IRValueID texId     = 0;   // resolves via valueToTex
        int       lane      = -1;  // scalar lane of varying; -1 = full scalar (use .x)
    };
    std::unordered_map<IRValueID, FpVaryingTexMulBinding> valueToVaryingTexMul;

    // Track which tex results have already been emitted as TEX
    // instructions (by the Discard handler for tex-LHS comparisons).
    // Prevents duplicate emission when StoreOutput later encounters
    // the same tex result.
    std::unordered_set<IRValueID> emittedTexResults;

    // Post-discard VecConstruct lowering for
    //   `float4(tex.x, tex.y, tex.z, varying * tex.w)`
    // The reference compiler emits MOVH H2.x, f[varying];
    // MOVR R0.xyz, R0; MULR R0.w, R0, H2.x.  The Mul operand
    // resolves through valueToVaryingTexMul (which captures
    // the (varying, tex_sample) Mul separately from the
    // valueToArith path so it doesn't interfere with Select
    // shapes).
    struct FpVecConstructTexMulBinding
    {
        IRValueID texBaseId   = 0;   // tex2D result
        IRValueID varyingId   = 0;   // varying input (from valueToVaryingTexMul)
        int       mulLane     = 3;   // w lane carries the Mul
        int       varyingLane = -1;  // scalar lane of varying; -1 = scalar varyings
    };
    std::unordered_map<IRValueID, FpVecConstructTexMulBinding>
        valueToVecConstructTexMul;

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
        // LHS shape:
        //   Varying: scalar lane of an entry-point varying (the
        //            existing case — `vcol.x > 0.5`).
        //   TexResult: scalar lane of a tex2D / texCUBE / tex2Dproj
        //             result.  The tex sample emits as TEX into an
        //             R-temp (NOT R0) at the cmp evaluation point;
        //             the cmp then sources from that temp with a
        //             smeared lane swizzle.  See the Select tex-LHS
        //             schedule in the StoreOutput emit.
        //   UniformScalar: `if (uniformFloat != 0.0)` shape.  LHS is
        //              a direct LoadUniform of a scalar uniform.  The
        //              cmp emits as a MOV-with-cond_write_enable on a
        //              CC lane, with the uniform sourced via a 16-byte
        //              inline const block (zeros, runtime-patched).
        enum class LhsKind { Varying, TexResult, UniformScalar };
        LhsKind   lhsKind        = LhsKind::Varying;
        IRValueID lhsBase        = 0;
        int       lhsLane        = 0;
        IRValueID lhsTexId       = 0;  // (TexResult only) the TexSample IR value
        IRValueID lhsUniformId   = 0;  // (UniformScalar only) the LoadUniform SSA
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
        // sce-cgc schedules ternary vs if-else differently:
        //   ternary  → preload `trueId`, conditional EQ-write `falseId`
        //   if-else  → preload `falseId`, conditional NE-write `trueId`
        // The IR conveys the choice through IRInstruction::componentIndex
        // (set by nv40_if_convert).
        bool      ifElseSchedule = false;
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
        enum class Kind { Rcp, Rsq, Cos, Sin };
        Kind      kind     = Kind::Rcp;
        IRValueID inputId  = 0;
        int       lane     = 0;  // 0..3 — which lane of the input to read
    };
    std::unordered_map<IRValueID, FpScalarUnaryBinding> valueToScalarUnary;

    // `length(vec3)` — sce-cgc lowers to MOVH + DP3R + DIVSQR + MOVR.
    // DIVSQR (NV40+RSX extension, opcode 0x3B) computes
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
        // Optional arith subexpression: when set, the input isn't a
        // direct varying — it's `varying ± uniform` (or similar).
        // Emit-side computes the arith into R0 first, then runs the
        // DP3 + DIVSQR sequence directly on R0.  Required for shapes
        // like `normalize(worldPos - gEye)`.
        bool         hasArith     = false;
        FpArithOp    arithOp      = FpArithOp::Add;
        IRValueID    arithInputId = 0;   // varying operand
        IRValueID    arithUniformId = 0; // uniform operand
        bool         arithUniformNeg = false;
    };
    std::unordered_map<IRValueID, FpNormalizeBinding> valueToNormalize;

    // `reflect(I, N)` — Cg standard library: I - 2 * dot(N, I) * N.
    // The reference compiler folds the `2 *` into a DP3 with
    // DST_SCALE_2X, then emits a single MAD with R0.wwww carrying
    // the scaled dot.  Five-instruction shape:
    //   MOVR R0.xyz, f[N]
    //   MOVR R1.xyz, f[I]
    //   DP3R(2X) R0.w, R0.xyz, R1.xyz
    //   FENCBR
    //   MADR R0.xyz, -R0.xyzw, R0.wwww, R1
    // Captured operands resolve through SrcMod chains so identity-
    // prefix shuffles (`v.xyz`) on either operand are handled.
    struct FpReflectBinding
    {
        IRValueID iId = 0;   // resolves via valueToInputSrc
        IRValueID nId = 0;   // resolves via valueToInputSrc
        bool      wrapW1 = false;  // set when VecConstruct(reflect, 1.0)
    };
    std::unordered_map<IRValueID, FpReflectBinding> valueToReflect;

    // Multi-instruction if-else predication chain — see header
    // comments + nv40_if_convert.h Shape 3 for the IR shape, and
    // the StoreOutput PredCarry handler for the lowering scheme.
    struct PredCarryBinding
    {
        IRValueID cmpId;
        IRValueID defaultId;       // initial default OR prior PredCarry result
        IROp      innerOp = IROp::Nop;
        std::vector<IRValueID> opArgs;  // original inner-op operands (with prior-link IDs remapped)
    };
    std::unordered_map<IRValueID, PredCarryBinding> valueToPredCarry;

    // Maps a PredCarry result SSA → R-temp index it lives in after
    // the chain emit.  Populated lazily as chains are emitted; lets
    // subsequent links resolve "prior link's result" → TEMP[idx].
    std::unordered_map<IRValueID, int> predCarryDstReg;

    // Track the most recent comparison or LogicalAnd result for the
    // discard handler.  When IROp::Discard is encountered, this holds
    // the condition that gates the kill.
    IRValueID lastConditionalId = InvalidIRValue;

    // Per-discard-instruction → condition ID.  Populated during the
    // analysis pass so the emit pass can find the correct condition
    // for each discard (rather than relying on the mutable
    // lastConditionalId which gets overwritten).
    std::unordered_map<const IRInstruction*, IRValueID> discardToCond;

    // LogicalAnd compound condition binding: two comparison results
    // combined with && in the source Cg.  Lowers to MULXC (multiply
    // condition codes) to combine the two CC-lane bits.
    struct LogicalAndBinding
    {
        IRValueID lhsId = 0;
        IRValueID rhsId = 0;
    };
    std::unordered_map<IRValueID, LogicalAndBinding> valueToLogicalAnd;

    // VecInsert: `c.a = scalar;` overrides one lane of a previously
    // computed vec value.  The reference compiler emits the underlying
    // producer with a full writemask, then a tail MOV writing only the
    // overridden lane to the same dst (typically R0) with the scalar
    // source.  Pattern probed against RenderSkyBoxFragment.cg
    // (texCUBE → c.a = 1.0f → return c).
    struct VecInsertBinding
    {
        IRValueID vectorId = 0;
        IRValueID scalarId = 0;
        int       lane     = 0;  // 0..3 — destination lane
    };
    std::unordered_map<IRValueID, VecInsertBinding> valueToVecInsert;

    // Varying-pack: `vec2(varying.x, varying.z)` (or wider).  The
    // reference compiler materialises this with one MOV writing the
    // pack lanes into a temp register's matching lanes and using a
    // partial mask.  Captured here so a consuming Dot can fold the
    // pack into an ahead-of-DP2 setup MOV without going through the
    // generic VecConstruct lowering.  Width 2 = vec2, 3 = vec3, 4 =
    // vec4 (latter is just an identity MOV).
    struct FpVaryingPackBinding
    {
        IRValueID baseId = 0;        // resolves via valueToInputSrc
        int       lanes[4] = {0, 0, 0, 0};
        int       width  = 2;
    };
    std::unordered_map<IRValueID, FpVaryingPackBinding> valueToVaryingPack;

    // `dot(literal_vec, varying_pack)` shape.  The reference compiler
    // emits this as 2 instructions: a setup MOV that packs the
    // varying's source lanes into a temp register's matching mask,
    // then a DP2/DP3/DP4 reading the temp + an inline literal const
    // block.  When the dot result feeds a `float4(d, d, d, 1.0)`
    // VecConstruct (a common single-channel-blend output), the
    // wrapW1 flag inserts a third MOV writing R_dst.W = 1.0 BEFORE
    // the dot itself so the dot insn carries PROGRAM_END.  Captured
    // operands resolve via valueToVaryingPack and valueToLiteralVec4.
    struct FpDotLitPackBinding
    {
        FpVaryingPackBinding pack;
        float                literal[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
        bool                 wrapW1      = false;  // float4(d,d,d,1.0)
    };
    std::unordered_map<IRValueID, FpDotLitPackBinding> valueToDotLitPack;

    // `vec3(d1, d2, d3)` where d1/d2/d3 are 3 distinct
    // FpDotLitPackBindings against the same varying-pack baseId.
    // Pre-wrap shape — recorded so a downstream `vec4(.., 1.0)` wrap
    // (2-operand form) or a `Mul(literal_vec, this_vec3)` scale step
    // can fold into the wrap binding below.
    struct FpDot3LitPackBinding
    {
        FpVaryingPackBinding pack;
        float                literals[3][4] = {{0}};
    };
    std::unordered_map<IRValueID, FpDot3LitPackBinding>
        valueToDot3LitPack;

    // `Mul(literal_vec3, vec3(d1, d2, d3))` — 3-dot pack scaled by a
    // folded literal vec3.  Captures the kitchen-sink shader's
    // `freq * Length` step.
    struct FpDot3LitPackScaledBinding
    {
        FpDot3LitPackBinding base;
        float                scale[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };
    std::unordered_map<IRValueID, FpDot3LitPackScaledBinding>
        valueToDot3LitPackScaled;

    // `Mul(literal_vec3, uniform_scalar)` — folded literal vec
    // multiplied by a uniform scalar (broadcast).  Captures the
    // kitchen-sink water FP `phase * gTime` step.  Recognized in
    // IROp::Mul; consumed by VecConstruct(.., 1.0) wrap below.
    struct FpLitVecUniformScaleBinding
    {
        float     literal[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
        IRValueID uniformId   = 0;
        int       vecWidth    = 3;
    };
    std::unordered_map<IRValueID, FpLitVecUniformScaleBinding>
        valueToLitVecUniformScale;

    // `vec4(literal_vec3 * uniform_scalar, 1.0)` — float4 wrap of
    // the FpLitVecUniformScaleBinding for a COLOR sink.  Lowers to
    // 3 instructions:
    //   MOV R0.x, c[0].xxxx + <uniform-patched zeros>
    //   MOV R0.w, c[0].yyyy + {0, 1.0, 0, 0}    ← W=1.0 at lane 1
    //                                           by the function-wide
    //                                           "prev .xxxx → avoid"
    //                                           literal-pool rule
    //   MUL R0.xyz [END], R0.xxxx, c[0].xyzw + {lit.x, lit.y, lit.z, 0}
    struct FpLitVecUniformScaleWrapBinding
    {
        float     literal[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
        IRValueID uniformId   = 0;
        int       vecWidth    = 3;
    };
    std::unordered_map<IRValueID, FpLitVecUniformScaleWrapBinding>
        valueToLitVecUniformScaleWrap;

    // `Sub(literal_vec3 * 3-dot pack, literal_vec3 * uniform_scalar)`
    // — the kitchen-sink water FP `freq * Length - phase * gTime`
    // step.  Combines an FpDot3LitPackScaled with an
    // FpLitVecUniformScale and routes the dots to a SECOND R-temp
    // (R1) so R0 can carry uniform + W=1.0 + final MAD.  Captures
    // the multi-register schedule the reference compiler chooses
    // for this specific composition.
    struct FpScaledDotsMinusLitMulUniformBinding
    {
        FpDot3LitPackScaledBinding   scaledDots;
        FpLitVecUniformScaleBinding  litTimesUni;
    };
    std::unordered_map<IRValueID, FpScaledDotsMinusLitMulUniformBinding>
        valueToScaledDotsMinusLitMulUni;

    // `vec4(scaled-dots-minus-lit-mul-uniform, 1.0)` — final wrap
    // form for the COLOR sink.  9-instruction shape:
    //   MOV R0.xy,    f[INPUT].<pack-swizzle>   (pack)
    //   MOV R0.w,     c[0].xxxx + {1, 0, 0, 0}  (W=1.0 lane 0)
    //   DP2 R1.x,     R0, c[0].xyxx + dirA-lit (lanes 0,1)
    //   DP2 R1.y,     R0, c[0].zwzz + dirB-lit (lanes 2,3)
    //   DP2 R1.z,     R0, c[0].yzzw + dirC-lit (lanes 1,2 — shifted)
    //   MOV R0.x,     c[0].xxxx + zeros        (uniform-patched)
    //   MUL R1.xyz,   R1.xyzw, c[0].xyzw + freq (scale dots)
    //   FENCBR
    //   MAD R0.xyz[END], -R0.xxxx, c[0].xyzw + phase, R1
    //                  (folds the Sub into MAD with NEGATE-on-src0)
    struct FpScaledDotsMinusLitMulUniformWrapBinding
    {
        FpScaledDotsMinusLitMulUniformBinding sub;
    };
    std::unordered_map<IRValueID, FpScaledDotsMinusLitMulUniformWrapBinding>
        valueToScaledDotsMinusLitMulUniWrap;

    // `vec4(scaled-or-bare 3-dot pack, 1.0)` — wraps either a bare
    // FpDot3LitPack or a FpDot3LitPackScaled into the float4 the
    // COLOR sink expects.  Lowers to 5 (no scale) or 6 (with scale)
    // instructions:
    //   MOV R0.zw, f[INPUT].<pack-cycle>             ; pack into Z/W
    //   DP2 R0.x, R0.zwzz, c[0].xyxx + lit_block_0   ; even lane (X)
    //   DP2 R0.y, R0.zwzz, c[0].zwzz + lit_block_2   ; odd lane (Y)
    //   DP2 R0.z, R0.zwzz, c[0].xyxx + lit_block_0   ; even lane (Z)
    //   [if hasScale]
    //   MUL R0.xyz, R0, c[0]      + scale_block      ; freq scale
    //   [end if]
    //   MOV R0.w [END], c[0].<W-swz> + {wOne layout}  ; W=1.0
    // The pack lives in R0.z/R0.w because lanes 0/1/2 are written by
    // the dots; literal block offset alternates by output-lane parity
    // (even→0,1 / odd→2,3).  W=1.0 layout depends on hasScale — the
    // bare wrap reads .xxxx from {1,0,0,0}; the scaled wrap reads
    // .wwww (encoded via .xyzw + lane 3) from {0,0,0,1.0}.
    struct FpDot3LitPackWrapBinding
    {
        FpVaryingPackBinding pack;
        float                literals[3][4] = {{0}};
        bool                 hasScale = false;
        float                scale[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };
    std::unordered_map<IRValueID, FpDot3LitPackWrapBinding>
        valueToDot3LitPackWrap;

    int nextTexUnit = 0;

    // Slot index for embedded-uniform globals.  Top-level uniforms
    // (Cg implicitly treats file-scope `float gFoo;` etc. as uniform)
    // get slots numbered after every entry parameter so the existing
    // `entryParamIndex`-based map stays unique across both spaces.
    // The container emit reverses the same numbering when it walks
    // module.globals.
    const unsigned firstGlobalSlot =
        static_cast<unsigned>(entry.parameters.size());
    std::unordered_map<std::string, unsigned> globalNameToFpUniformSlot;
    std::unordered_map<std::string, int>      globalNameToTexUnit;

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

    // Pre-populate valueToLiteralVec4 from any IRConstant of vec4 /
    // vec3 / vec2 type in the function's value map.  The IR builder's
    // const-fold pass (ir_builder.cpp `tryFoldVecConstruct`) emits
    // folded constants directly as IRConstants — bypassing the
    // VecConstruct case that historically populated this map.  We
    // mirror the entries here so all the existing literal-vec4 emit
    // paths (Select branches, MAD addends, FENCBR-MOV-c[0], etc.)
    // keep firing for folded constants.
    for (const auto& kv : entry.values)
    {
        if (!kv.second) continue;
        auto* c = dynamic_cast<IRConstant*>(kv.second.get());
        if (!c) continue;
        if (!std::holds_alternative<std::vector<float>>(c->value)) continue;
        const auto& comps = std::get<std::vector<float>>(c->value);
        if (comps.empty() || comps.size() > 4) continue;
        LiteralVec4 lv;
        for (size_t i = 0; i < comps.size(); ++i) lv.vals[i] = comps[i];
        valueToLiteralVec4[kv.first] = lv;
    }

    // Determine which entry parameters are actually consumed by the
    // IR.  A param is "referenced" iff its IR valueId appears as an
    // operand of any instruction in the function.  `out` / `inout`
    // params are always referenced (StoreOutput consumes them by
    // semantic, not as an operand).  Mirrors the reference compiler:
    // a varying or uniform that's declared but never read drops to
    // isReferenced=0 in the parameter table.
    {
        std::unordered_set<IRValueID> usedValueIds;
        for (const auto& blockPtr : entry.blocks)
        {
            if (!blockPtr) continue;
            for (const auto& instPtr : blockPtr->instructions)
            {
                if (!instPtr) continue;
                for (IRValueID id : instPtr->operands)
                    usedValueIds.insert(id);
            }
        }
        for (size_t pi = 0; pi < entry.parameters.size(); ++pi)
        {
            const auto& p = entry.parameters[pi];
            const bool isOut = (p.storage == StorageQualifier::Out ||
                                p.storage == StorageQualifier::InOut);
            if (isOut || usedValueIds.count(p.valueId))
                attrs.referencedParamIndices.insert(static_cast<unsigned>(pi));
        }
    }

    bool emittedSomething = false;

    // Two-pass architecture:
    //   Pass 0 — Analysis: walk all instructions, populate every
    //     binding map (valueToArith, valueToVaryingTexMul,
    //     valueToVecConstructTexMul, valueToCmp, etc.).
    //     Emit cases (Discard, StoreOutput) are skipped.
    //   Pass 1 — Emit: walk instructions again, only processing
    //     Discard and StoreOutput.  All binding maps are fully
    //     populated so the emit pass can make informed decisions
    //     (e.g. single-channel TEX output mask for discard-only
    //     tex results).
    for (int pass = 0; pass < 2; ++pass)
    {
    for (const auto& blockPtr : entry.blocks)
    {
        if (!blockPtr) continue;
        for (const auto& instPtr : blockPtr->instructions)
        {
            if (!instPtr) continue;
            const IRInstruction& inst = *instPtr;

            // Record types only during analysis pass; emit pass
            // only re-walks for Discard / StoreOutput.
            if (pass == 0 && inst.result != InvalidIRValue)
                valueToType[inst.result] = inst.resultType;

            // Analysis pass skips emit cases; emit pass skips
            // everything except Discard and StoreOutput.
            if (pass == 0 && inst.op == IROp::StoreOutput)
                continue;
            if (pass == 1 && inst.op != IROp::Discard &&
                              inst.op != IROp::StoreOutput)
                continue;

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

            case IROp::LoadUniform:
            {
                // File-scope uniform globals (`float gFoo;` etc., now
                // promoted to Uniform storage by the IR builder) need
                // the same hookup as entry-param uniforms: samplers go
                // to a tex unit, scalars/vectors go to embedded-const
                // slots.  Slot indices are deduped per global so a
                // repeated LoadUniform of the same name shares the slot.
                const IRGlobal* g = nullptr;
                for (const auto& gg : module.globals)
                {
                    if (gg.name == inst.targetName) { g = &gg; break; }
                }
                if (!g)
                {
                    out.diagnostics.push_back(
                        "nv40-fp: LoadUniform for unknown global '" + inst.targetName + "'");
                    return out;
                }
                const bool isSampler =
                    (g->type.baseType == IRType::Sampler2D ||
                     g->type.baseType == IRType::SamplerCube);
                if (isSampler)
                {
                    auto it = globalNameToTexUnit.find(g->name);
                    int tu;
                    if (it == globalNameToTexUnit.end())
                    {
                        tu = nextTexUnit++;
                        globalNameToTexUnit[g->name] = tu;
                    }
                    else
                    {
                        tu = it->second;
                    }
                    valueToTexUnit[inst.result] = tu;
                }
                else
                {
                    auto it = globalNameToFpUniformSlot.find(g->name);
                    unsigned slot;
                    if (it == globalNameToFpUniformSlot.end())
                    {
                        slot = firstGlobalSlot +
                               static_cast<unsigned>(globalNameToFpUniformSlot.size());
                        globalNameToFpUniformSlot[g->name] = slot;
                        attrs.embeddedUniforms.push_back({ slot, {} });
                    }
                    else
                    {
                        slot = it->second;
                    }
                    valueToFpUniform[inst.result] = slot;
                }
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

            case IROp::VecExtract:
            {
                // `extract vec[lane]` — same shape as a single-lane
                // VecShuffle.  Alias to a ScalarExtract through any
                // SrcMod / VecShuffle bridges so downstream consumers
                // (comparisons, scalar math, etc.) resolve to the
                // ultimate base-value lane.
                if (inst.operands.size() != 1) break;
                IRValueID base = inst.operands[0];
                int       lane = inst.componentIndex;
                // Walk through SrcMod aliases (identity-prefix
                // VecShuffle records).  No swizzle remap needed since
                // identity-prefix keeps lane indices unchanged.
                while (true)
                {
                    auto modIt = valueToMod.find(base);
                    if (modIt == valueToMod.end()) break;
                    if (modIt->second.absMod || modIt->second.negMod) break;
                    base = modIt->second.baseId;
                }
                ScalarExtract se;
                se.baseId = base;
                se.lane   = lane & 3;
                valueToScalarExtract[inst.result] = se;
                break;
            }

            case IROp::VecInsert:
            {
                if (inst.operands.size() < 2) break;
                VecInsertBinding vib;
                vib.vectorId = inst.operands[0];
                vib.scalarId = inst.operands[1];
                vib.lane     = inst.componentIndex;
                valueToVecInsert[inst.result] = vib;
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

                // LHS: scalar extract of either an entry-point varying
                // (`vcol.x > k`) or a tex2D / texCUBE / tex2Dproj
                // result (`tex.a > k`).  Either way, the cmp records
                // an LhsKind so the StoreOutput emit can dispatch the
                // right schedule.  A direct LoadUniform of a scalar
                // uniform (`if (gFunctionSwitch != 0.0)`) skips the
                // ScalarExtract step — the lane defaults to .x via the
                // c[0].xxxx swizzle in the emit.
                CmpBinding cb;
                if (auto seIt = valueToScalarExtract.find(inst.operands[0]);
                    seIt != valueToScalarExtract.end())
                {
                    if (auto inIt = valueToInputSrc.find(seIt->second.baseId);
                        inIt != valueToInputSrc.end())
                    {
                        cb.lhsKind = CmpBinding::LhsKind::Varying;
                    }
                    else if (auto txIt = valueToTex.find(seIt->second.baseId);
                             txIt != valueToTex.end())
                    {
                        cb.lhsKind  = CmpBinding::LhsKind::TexResult;
                        cb.lhsTexId = seIt->second.baseId;
                    }
                    else
                    {
                        break;  // unknown LHS shape — leave unlowered
                    }
                    cb.lhsBase = seIt->second.baseId;
                    cb.lhsLane = seIt->second.lane;
                }
                else if (auto uIt = valueToFpUniform.find(inst.operands[0]);
                         uIt != valueToFpUniform.end())
                {
                    cb.lhsKind      = CmpBinding::LhsKind::UniformScalar;
                    cb.lhsUniformId = inst.operands[0];
                }
                else
                {
                    break;  // unknown LHS shape
                }

                // RHS: either a float constant or an FP uniform.
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
                lastConditionalId = inst.result;
                break;
            }

            case IROp::Select:
            {
                if (inst.operands.size() < 3) break;
                SelectBinding sb;
                sb.cmpId           = inst.operands[0];
                sb.trueId          = inst.operands[1];
                sb.falseId         = inst.operands[2];
                sb.ifElseSchedule  = (inst.componentIndex == 1);
                valueToSelect[inst.result] = sb;
                break;
            }

            case IROp::PredCarry:
            {
                // Defer everything to the consuming StoreOutput's
                // chain emitter — at this point we just record the
                // shape (cond, default, inner op + args).  Carry
                // dst register allocation only makes sense once we
                // know the chain length, which we discover when the
                // StoreOutput pulls the last link.
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back(
                        "nv40-fp: PredCarry needs cond + default operand");
                    return out;
                }
                PredCarryBinding pcb;
                pcb.cmpId     = inst.operands[0];
                pcb.defaultId = inst.operands[1];
                pcb.innerOp   = inst.predOp;
                for (size_t i = 2; i < inst.operands.size(); ++i)
                    pcb.opArgs.push_back(inst.operands[i]);
                valueToPredCarry[inst.result] = pcb;
                break;
            }

            case IROp::LogicalAnd:
            {
                if (inst.operands.size() >= 2)
                {
                    // Record the compound condition for downstream lookup.
                    LogicalAndBinding lab;
                    lab.lhsId = inst.operands[0];
                    lab.rhsId = inst.operands[1];
                    valueToLogicalAnd[inst.result] = lab;
                    lastConditionalId = inst.result;
                }
                break;
            }

            case IROp::CondBranch:
            case IROp::Branch:
                // Control flow — handled by if-conversion; ignore here.
                break;

            case IROp::Discard:
            {
                // Analysis pass: record which condition gates this
                // discard so the emit pass can find it.
                if (pass == 0)
                {
                    if (lastConditionalId == InvalidIRValue)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: discard with no preceding condition");
                        return out;
                    }
                    discardToCond[&inst] = lastConditionalId;
                    break;
                }

                // Emit pass: emit conditional KIL.
                auto condIt = discardToCond.find(&inst);
                if (condIt == discardToCond.end())
                {
                    out.diagnostics.push_back(
                        "nv40-fp: discard condition not recorded in analysis pass");
                    return out;
                }

                IRValueID condId = condIt->second;

                // Resolve condition to leaf comparison IDs (flatten LogicalAnd).
                std::vector<IRValueID> cmpIds;
                {
                    auto laIt = valueToLogicalAnd.find(condId);
                    if (laIt != valueToLogicalAnd.end())
                        cmpIds = { laIt->second.lhsId, laIt->second.rhsId };
                    else
                        cmpIds = { condId };
                }

                struct ResolvedCmp {
                    const CmpBinding* bind = nullptr;
                    int ccLane = 0;
                };
                std::vector<ResolvedCmp> rcmps;
                for (size_t ci = 0; ci < cmpIds.size(); ++ci)
                {
                    auto it = valueToCmp.find(cmpIds[ci]);
                    if (it == valueToCmp.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: discard comparison #" +
                            std::to_string(ci) + " not resolved");
                        return out;
                    }
                    ResolvedCmp rc;
                    rc.bind = &it->second;
                    rc.ccLane = (cmpIds.size() == 1) ? 0 : (ci == 0) ? 0 : 3;
                    rcmps.push_back(rc);
                }

                const bool compound = (cmpIds.size() >= 2);

                const struct nvfx_reg none = nvfx_reg(NVFXSR_NONE, 0);

                // sce-cgc emits compound-discard comparisons in reverse
                // LogicalAnd order (rhs then lhs), so reverse our list
                // to match the reference ucode byte-for-byte.
                if (compound)
                    std::reverse(rcmps.begin(), rcmps.end());

                // Pre-pass: emit TEX for any tex-LHS comparisons
                // before any MOV / CMP instruction.  This matches
                // sce-cgc's schedule where TEX always comes first,
                // regardless of StoreOutput position relative to
                // the discard block.
                // Only apply when no TEX has been emitted yet; after
                // a prior TEX+discard sequence the uniform load (MOV)
                // should precede the next TEX (sce-cgc dual-discard
                // pattern).
                if (compound && emittedTexResults.empty())
                {
                    for (const auto& rc : rcmps)
                    {
                        if (rc.bind->lhsKind != CmpBinding::LhsKind::TexResult)
                            continue;
                        if (emittedTexResults.count(rc.bind->lhsTexId))
                            continue;
                        auto txIt = valueToTex.find(rc.bind->lhsTexId);
                        if (txIt == valueToTex.end()) continue;
                        auto sampIt = valueToTexUnit.find(
                            txIt->second.samplerId);
                        if (sampIt == valueToTexUnit.end()) continue;
                        IRValueID uvBase =
                            resolveSrcMods(txIt->second.uvId).baseId;
                        auto uvIt = valueToInputSrc.find(uvBase);
                        if (uvIt == valueToInputSrc.end()) continue;

                        const struct nvfx_reg uvReg =
                            nvfx_reg(NVFXSR_INPUT, uvIt->second);
                        const struct nvfx_reg none =
                            nvfx_reg(NVFXSR_NONE, 0);
                        struct nvfx_src texSrc0 = nvfx_src(
                            const_cast<struct nvfx_reg&>(uvReg));
                        struct nvfx_src texSrc1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src texSrc2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        const uint8_t texOpcode =
                            txIt->second.projective
                                ? NVFX_FP_OP_OPCODE_TXP
                                : NVFX_FP_OP_OPCODE_TEX;

                        struct nvfx_reg outReg =
                            nvfx_reg(NVFXSR_OUTPUT, 0);
                        struct nvfx_insn texInsn = nvfx_insn(
                            0, 0,
                            sampIt->second, -1,
                            const_cast<struct nvfx_reg&>(outReg),
                            NVFX_FP_MASK_ALL,
                            texSrc0, texSrc1, texSrc2);
                        texInsn.precision = FLOAT32;
                        if (txIt->second.cube)
                            texInsn.disable_pc = 1;
                        asm_.emit(texInsn, texOpcode);

                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(uvIt->second);
                        if (uvIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            uvIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            attrs.texCoordsInputMask |=
                                uint16_t{1} << (uvIt->second -
                                    NVFX_FP_OP_INPUT_SRC_TC(0));

                        emittedTexResults.insert(rc.bind->lhsTexId);
                    }
                }

                // Track emitted KIL count so subsequent discards
                // can use different register masks.
                static int discardEmitCount = 0;
                const bool isSubsequentDiscard = (discardEmitCount > 0);

                // Pre-pass: emit uniform loads (MOVR) for any
                // UniformScalar comparisons before any comparison
                // instruction, matching the reference schedule.
                if (compound)
                {
                    for (const auto& rc : rcmps)
                    {
                        if (rc.bind->lhsKind != CmpBinding::LhsKind::UniformScalar)
                            continue;
                        auto uIt = valueToFpUniform.find(
                            rc.bind->lhsUniformId);
                        if (uIt == valueToFpUniform.end()) continue;

                        const struct nvfx_reg r1Reg =
                            nvfx_reg(NVFXSR_TEMP, 1);
                        const struct nvfx_reg constReg =
                            nvfx_reg(NVFXSR_CONST, 0);
                        struct nvfx_src uS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        uS0.swz[0] = uS0.swz[1] =
                        uS0.swz[2] = uS0.swz[3] = 0;
                        struct nvfx_src uS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src uS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        const uint8_t movMask =
                            isSubsequentDiscard
                                ? NVFX_FP_MASK_W
                                : NVFX_FP_MASK_X;
                        struct nvfx_insn movU = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(r1Reg),
                            movMask, uS0, uS1, uS2);
                        movU.precision = FLOAT32;
                        asm_.emit(movU, NVFX_FP_OP_OPCODE_MOV);
                        const uint32_t off = asm_.currentByteSize();
                        for (auto& eu : attrs.embeddedUniforms)
                            if (eu.entryParamIndex == uIt->second)
                            { eu.ucodeByteOffsets.push_back(off); break; }
                        static const float zeros[4] = {
                            0.0f, 0.0f, 0.0f, 0.0f
                        };
                        asm_.appendConstBlock(zeros);
                    }
                }

                for (const auto& rc : rcmps)
                {
                    const CmpBinding& cb = *rc.bind;
                    const uint8_t laneMask =
                        static_cast<uint8_t>(1u << rc.ccLane);

                    switch (cb.lhsKind)
                    {
                    case CmpBinding::LhsKind::Varying:
                    {
                        auto inIt = valueToInputSrc.find(cb.lhsBase);
                        if (inIt == valueToInputSrc.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: discard CMP varying LHS unresolved");
                            return out;
                        }

                        // MOVH H2, f[input]
                        struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, 2);
                        hReg.is_fp16 = 1;
                        const struct nvfx_reg inReg =
                            nvfx_reg(NVFXSR_INPUT, inIt->second);
                        struct nvfx_src mvhS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(inReg));
                        if (cb.lhsLane != 0)
                            mvhS0.swz[0] = mvhS0.swz[1] =
                            mvhS0.swz[2] = mvhS0.swz[3] =
                                static_cast<uint8_t>(cb.lhsLane);
                        struct nvfx_src mvhS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src mvhS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn mvh = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(hReg),
                            NVFX_FP_MASK_ALL, mvhS0, mvhS1, mvhS2);
                        mvh.precision = FLOAT16;
                        asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(inIt->second);
                        if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            attrs.texCoordsInputMask |=
                                uint16_t{1} << (inIt->second -
                                                NVFX_FP_OP_INPUT_SRC_TC(0));

                        // For compound conditions: write to H2.<lane>
                        // (no cc_update); for simple: write to CC.
                        struct nvfx_reg cmpDst;
                        if (compound)
                        {
                            cmpDst = nvfx_reg(NVFXSR_TEMP, 2);
                            cmpDst.is_fp16 = 1;
                        }
                        else
                        {
                            cmpDst = nvfx_reg(NVFXSR_NONE, 0x3F);
                        }

                        struct nvfx_reg hSrc = nvfx_reg(NVFXSR_TEMP, 2);
                        hSrc.is_fp16 = 1;
                        struct nvfx_src cS0 = nvfx_src(hSrc);
                        if (cb.lhsLane != 0)
                            cS0.swz[0] = cS0.swz[1] =
                            cS0.swz[2] = cS0.swz[3] =
                                static_cast<uint8_t>(cb.lhsLane);

                        const struct nvfx_reg constReg =
                            nvfx_reg(NVFXSR_CONST, 0);
                        struct nvfx_src cS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        cS1.swz[0] = cS1.swz[1] =
                        cS1.swz[2] = cS1.swz[3] = 0;

                        struct nvfx_src cS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn cmpInsn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(cmpDst),
                            laneMask, cS0, cS1, cS2);
                        if (!compound)
                            cmpInsn.cc_update = 1;
                        asm_.emit(cmpInsn, cb.opcode);

                        if (cb.rhsKind == CmpBinding::RhsKind::Uniform)
                        {
                            const uint32_t off = asm_.currentByteSize();
                            auto rhsUniIt = valueToFpUniform.find(cb.rhsUniformId);
                            if (rhsUniIt != valueToFpUniform.end())
                                for (auto& eu : attrs.embeddedUniforms)
                                    if (eu.entryParamIndex == rhsUniIt->second)
                                    { eu.ucodeByteOffsets.push_back(off); break; }
                            static const float zeros[4] = {
                                0.0f, 0.0f, 0.0f, 0.0f
                            };
                            asm_.appendConstBlock(zeros);
                        }
                        else
                        {
                            const float lit[4] = {
                                cb.rhsLiteral, 0.0f, 0.0f, 0.0f
                            };
                            asm_.appendConstBlock(lit);
                        }

                        break;
                    }

                    case CmpBinding::LhsKind::UniformScalar:
                    {
                        auto uIt = valueToFpUniform.find(cb.lhsUniformId);
                        if (uIt == valueToFpUniform.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: discard CMP uniform LHS unresolved");
                            return out;
                        }

                        // MOVR R1.x already emitted in pre-pass for
                        // compound conditions; emit it here for simple.
                        const struct nvfx_reg r1Reg =
                            nvfx_reg(NVFXSR_TEMP, 1);
                        const struct nvfx_reg constReg =
                            nvfx_reg(NVFXSR_CONST, 0);
                        if (!compound)
                        {
                            struct nvfx_src uS0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                            uS0.swz[0] = uS0.swz[1] =
                            uS0.swz[2] = uS0.swz[3] = 0;
                            struct nvfx_src uS1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src uS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn movU = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(r1Reg),
                                NVFX_FP_MASK_X, uS0, uS1, uS2);
                            movU.precision = FLOAT32;
                            asm_.emit(movU, NVFX_FP_OP_OPCODE_MOV);
                            static const float zeros[4] = {
                                0.0f, 0.0f, 0.0f, 0.0f
                            };
                            asm_.appendConstBlock(zeros);
                        }

                        // Comparison: compound → H2.<lane>, simple → CC
                        struct nvfx_reg cmpDst;
                        if (compound)
                        {
                            cmpDst = nvfx_reg(NVFXSR_TEMP, 2);
                            cmpDst.is_fp16 = 1;
                        }
                        else
                        {
                            cmpDst = nvfx_reg(NVFXSR_NONE, 0x3F);
                        }

                        struct nvfx_src cS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(r1Reg));
                        if (compound && isSubsequentDiscard)
                        {
                            cS0.swz[0] = 3;
                            cS0.swz[1] = 3;
                            cS0.swz[2] = 3;
                            cS0.swz[3] = 3;
                        }
                        struct nvfx_src cS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        // For subsequent discards, smear .yyyy on
                        // the zero-const SRC1 to match the reference
                        // encoding.
                        const uint8_t zSwz = isSubsequentDiscard ? 1u : 0u;
                        cS1.swz[0] = zSwz;
                        cS1.swz[1] = zSwz;
                        cS1.swz[2] = zSwz;
                        cS1.swz[3] = zSwz;
                        struct nvfx_src cS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn cmpInsn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(cmpDst),
                            laneMask, cS0, cS1, cS2);
                        if (!compound)
                            cmpInsn.cc_update = 1;
                        asm_.emit(cmpInsn, cb.opcode);

                        if (cb.rhsKind == CmpBinding::RhsKind::Uniform)
                        {
                            static const float unifZeros[4] = {
                                0.0f, 0.0f, 0.0f, 0.0f
                            };
                            asm_.appendConstBlock(unifZeros);
                        }
                        else
                        {
                            const float lit[4] = {
                                cb.rhsLiteral, 0.0f, 0.0f, 0.0f
                            };
                            asm_.appendConstBlock(lit);
                        }

                        break;
                    }

                    case CmpBinding::LhsKind::TexResult:
                    {
                        // The tex result must be in R0 before the
                        // comparison reads it.  Emit TEX inline if
                        // not already emitted by an earlier handler.
                        if (!emittedTexResults.count(cb.lhsTexId))
                        {
                            auto txIt = valueToTex.find(cb.lhsTexId);
                            if (txIt != valueToTex.end())
                            {
                                auto sampIt = valueToTexUnit.find(
                                    txIt->second.samplerId);
                                if (sampIt != valueToTexUnit.end())
                                {
                                    IRValueID uvBase =
                                        resolveSrcMods(txIt->second.uvId).baseId;
                                    auto uvIt = valueToInputSrc.find(uvBase);
                                    if (uvIt != valueToInputSrc.end())
                                    {
                                        const struct nvfx_reg uvReg =
                                            nvfx_reg(NVFXSR_INPUT, uvIt->second);
                                        struct nvfx_src texSrc0 = nvfx_src(
                                            const_cast<struct nvfx_reg&>(uvReg));
                                        struct nvfx_src texSrc1 =
                                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                                        struct nvfx_src texSrc2 =
                                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                                        const uint8_t texOpcode =
                                            txIt->second.projective
                                                ? NVFX_FP_OP_OPCODE_TXP
                                                : NVFX_FP_OP_OPCODE_TEX;

                                        struct nvfx_reg outReg =
                                            nvfx_reg(NVFXSR_OUTPUT, 0);
                                        // For simple (non-compound) discards
                                        // where the tex result is only used for
                                        // the discard comparison, sce-cgc uses a
                                        // single-component output mask.  If the
                                        // tex is also consumed by a VecConstruct-
                                        // TexMul output, all channels are needed.
                                        uint8_t texMask = NVFX_FP_MASK_ALL;
                                        if (!compound)
                                        {
                                            bool usedForOutput = false;
                                            for (const auto& vctm : valueToVecConstructTexMul)
                                            {
                                                if (vctm.second.texBaseId == cb.lhsTexId)
                                                { usedForOutput = true; break; }
                                            }
                                            if (!usedForOutput)
                                            {
                                                static const uint8_t kLaneMasks[4] = {
                                                    NVFX_FP_MASK_X,
                                                    NVFX_FP_MASK_Y,
                                                    NVFX_FP_MASK_Z,
                                                    NVFX_FP_MASK_W
                                                };
                                                texMask = kLaneMasks[cb.lhsLane & 3];
                                            }
                                        }
                                        struct nvfx_insn texInsn = nvfx_insn(
                                            0, 0,
                                            sampIt->second, -1,
                                            const_cast<struct nvfx_reg&>(outReg),
                                            texMask,
                                            texSrc0, texSrc1, texSrc2);
                                        texInsn.precision = FLOAT32;
                                        if (txIt->second.cube)
                                            texInsn.disable_pc = 1;
                                        asm_.emit(texInsn, texOpcode);

                                        attrs.attributeInputMask |=
                                            fpAttrMaskBitForInputSrc(uvIt->second);
                                        if (uvIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                                            uvIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                                            attrs.texCoordsInputMask |=
                                                uint16_t{1} << (uvIt->second -
                                                    NVFX_FP_OP_INPUT_SRC_TC(0));
                                    }
                                }
                            }
                            emittedTexResults.insert(cb.lhsTexId);
                        }

                        // Emit the comparison using R0 as the source.
                        const struct nvfx_reg r0Reg =
                            nvfx_reg(NVFXSR_TEMP, 0);
                        const struct nvfx_reg constReg =
                            nvfx_reg(NVFXSR_CONST, 0);

                        // Compound → H2.<lane>, simple → CC
                        struct nvfx_reg cmpDst;
                        if (compound)
                        {
                            cmpDst = nvfx_reg(NVFXSR_TEMP, 2);
                            cmpDst.is_fp16 = 1;
                        }
                        else
                        {
                            cmpDst = nvfx_reg(NVFXSR_NONE, 0x3F);
                        }

                        struct nvfx_src cS0 = nvfx_src(
                            const_cast<struct nvfx_reg&>(r0Reg));
                        // For simple (non-compound) comparisons, smear
                        // the target lane across all swizzle positions
                        // so the CC.x comparison reads the correct source
                        // component (e.g. .wwww for alpha comparison).
                        if (!compound && cb.lhsLane != 0)
                            cS0.swz[0] = cS0.swz[1] =
                            cS0.swz[2] = cS0.swz[3] =
                                static_cast<uint8_t>(cb.lhsLane);

                        struct nvfx_src cS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        cS1.swz[0] = cS1.swz[1] =
                        cS1.swz[2] = cS1.swz[3] = 0;

                        struct nvfx_src cS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));

                        struct nvfx_insn cmpInsn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(cmpDst),
                            laneMask, cS0, cS1, cS2);
                        if (!compound)
                        {
                            cmpInsn.cc_update = 1;
                            cmpInsn.precision = 2; // FX12 matches sce-cgc
                        }
                        asm_.emit(cmpInsn, cb.opcode);

                        if (cb.rhsKind == CmpBinding::RhsKind::Uniform)
                        {
                            const uint32_t off = asm_.currentByteSize();
                            auto rhsUniIt = valueToFpUniform.find(cb.rhsUniformId);
                            if (rhsUniIt != valueToFpUniform.end())
                                for (auto& eu : attrs.embeddedUniforms)
                                    if (eu.entryParamIndex == rhsUniIt->second)
                                    { eu.ucodeByteOffsets.push_back(off); break; }
                            static const float zeros[4] = {
                                0.0f, 0.0f, 0.0f, 0.0f
                            };
                            asm_.appendConstBlock(zeros);
                        }
                        else
                        {
                            const float lit[4] = {
                                cb.rhsLiteral, 0.0f, 0.0f, 0.0f
                            };
                            asm_.appendConstBlock(lit);
                        }
                        break;
                    }

                    default:
                        out.diagnostics.push_back(
                            "nv40-fp: discard CMP LHS kind not supported");
                        return out;
                    }
                }

                // If compound (LogicalAnd): emit MULXC
                if (cmpIds.size() >= 2)
                {
                    const struct nvfx_reg ccDst =
                        nvfx_reg(NVFXSR_NONE, 0x3F);
                    struct nvfx_reg h2 = nvfx_reg(NVFXSR_TEMP, 2);
                    h2.is_fp16 = 1;
                    struct nvfx_src s0 = nvfx_src(h2);
                    s0.swz[0] = 0; s0.swz[1] = 1;
                    s0.swz[2] = 2; s0.swz[3] = 3;
                    struct nvfx_src s1 = nvfx_src(h2);
                    s1.swz[0] = s1.swz[1] =
                    s1.swz[2] = s1.swz[3] = 3;
                    struct nvfx_src s2 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mulxc = nvfx_insn(
                        0, 0, 0, -1,
                        const_cast<struct nvfx_reg&>(ccDst),
                        NVFX_FP_MASK_X, s0, s1, s2);
                    mulxc.cc_update = 1;
                    // sce-cgc encodes MULXC with precision=FX12(2)
                    mulxc.precision = 2;
                    asm_.emit(mulxc, NVFX_FP_OP_OPCODE_MUL);
                }

                // Emit KIL (NE.x)
                {
                    const struct nvfx_reg ccDst =
                        nvfx_reg(NVFXSR_NONE, 0x3F);
                    struct nvfx_src s0 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src s1 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src s2 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn kil = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(ccDst),
                        NVFX_FP_MASK_X | NVFX_FP_MASK_Y, s0, s1, s2);
                    kil.precision = 0;
                    kil.sat = 0;
                    kil.cc_update = 0;
                    kil.cc_update_reg = 0;
                    kil.cc_test = 0;
                    kil.cc_test_reg = 0;
                    kil.cc_cond = NVFX_COND_NE;
                    kil.cc_swz[0] = 0;
                    kil.cc_swz[1] = 0;
                    kil.cc_swz[2] = 0;
                    kil.cc_swz[3] = 0;
                    asm_.emit(kil, NVFX_FP_OP_OPCODE_KIL);
                }

                attrs.pixelKillCount += 1;
                ++discardEmitCount;
                emittedSomething = true;
                break;
            }

            case IROp::Dot:
            {
                if (inst.operands.size() < 2)
                {
                    out.diagnostics.push_back("nv40-fp: Dot with <2 operands");
                    return out;
                }

                // (folded literal vec, varying-pack) — record an
                // FpDotLitPackBinding and break out before the
                // existing (input, uniform) check fires.
                {
                    auto aLit  = valueToLiteralVec4.find(inst.operands[0]);
                    auto bLit  = valueToLiteralVec4.find(inst.operands[1]);
                    auto aPack = valueToVaryingPack.find(inst.operands[0]);
                    auto bPack = valueToVaryingPack.find(inst.operands[1]);
                    const FpVaryingPackBinding* pack = nullptr;
                    const LiteralVec4*          lit  = nullptr;
                    if (aLit  != valueToLiteralVec4.end()  &&
                        bPack != valueToVaryingPack.end())
                    {
                        lit  = &aLit->second;
                        pack = &bPack->second;
                    }
                    else if (bLit  != valueToLiteralVec4.end() &&
                             aPack != valueToVaryingPack.end())
                    {
                        lit  = &bLit->second;
                        pack = &aPack->second;
                    }
                    if (pack && lit)
                    {
                        FpDotLitPackBinding b;
                        b.pack = *pack;
                        for (int k = 0; k < 4; ++k) b.literal[k] = lit->vals[k];
                        valueToDotLitPack[inst.result] = b;
                        break;
                    }
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

                // Sub(FpDot3LitPackScaled, FpLitVecUniformScale) —
                // fold a scaled-dots minus lit×uniform shape into a
                // single combined binding.  Recognizer fires before
                // the generic arithmetic path.
                if (inst.op == IROp::Sub)
                {
                    auto aSd = valueToDot3LitPackScaled.find(a);
                    auto bLu = valueToLitVecUniformScale.find(b);
                    if (aSd != valueToDot3LitPackScaled.end() &&
                        bLu != valueToLitVecUniformScale.end())
                    {
                        FpScaledDotsMinusLitMulUniformBinding sb;
                        sb.scaledDots  = aSd->second;
                        sb.litTimesUni = bLu->second;
                        valueToScaledDotsMinusLitMulUni[inst.result] = sb;
                        break;
                    }
                }

                // Mul(literal_vec, uniform_scalar) — broadcast scale.
                // Records a binding that the downstream wrap can use
                // to emit the 3-instruction shape (uniform preload +
                // W=1.0 + final MUL).  Commutative.
                if (inst.op == IROp::Mul)
                {
                    auto isScalarUniform = [&](IRValueID id) -> bool
                    {
                        auto uIt = valueToFpUniform.find(id);
                        if (uIt == valueToFpUniform.end()) return false;
                        if (uIt->second >= entry.parameters.size()) return false;
                        return entry.parameters[uIt->second].type.vectorSize <= 1;
                    };
                    auto aLit = valueToLiteralVec4.find(a);
                    auto bLit = valueToLiteralVec4.find(b);
                    const LiteralVec4* lit = nullptr;
                    IRValueID uniId = 0;
                    if (aLit != valueToLiteralVec4.end() && isScalarUniform(b))
                    {
                        lit   = &aLit->second;
                        uniId = b;
                    }
                    else if (bLit != valueToLiteralVec4.end() && isScalarUniform(a))
                    {
                        lit   = &bLit->second;
                        uniId = a;
                    }
                    if (lit && uniId)
                    {
                        FpLitVecUniformScaleBinding bind;
                        for (int k = 0; k < 4; ++k) bind.literal[k] = lit->vals[k];
                        bind.uniformId = uniId;
                        bind.vecWidth  = inst.resultType.vectorSize;
                        valueToLitVecUniformScale[inst.result] = bind;
                        break;
                    }
                }

                // Mul(literal_vec, FpDot3LitPack) — record a scaled
                // 3-dot pack so a downstream `vec4(.., 1.0)` wrap can
                // emit a tail MUL with the scale literal block in
                // line.  Commutative.
                if (inst.op == IROp::Mul)
                {
                    auto aLit  = valueToLiteralVec4.find(a);
                    auto bLit  = valueToLiteralVec4.find(b);
                    auto aDot3 = valueToDot3LitPack.find(a);
                    auto bDot3 = valueToDot3LitPack.find(b);
                    const FpDot3LitPackBinding* dot3 = nullptr;
                    const LiteralVec4*          lit  = nullptr;
                    if (aLit != valueToLiteralVec4.end() &&
                        bDot3 != valueToDot3LitPack.end())
                    {
                        lit  = &aLit->second;
                        dot3 = &bDot3->second;
                    }
                    else if (bLit != valueToLiteralVec4.end() &&
                             aDot3 != valueToDot3LitPack.end())
                    {
                        lit  = &bLit->second;
                        dot3 = &aDot3->second;
                    }
                    if (dot3 && lit)
                    {
                        FpDot3LitPackScaledBinding b;
                        b.base = *dot3;
                        for (int k = 0; k < 4; ++k) b.scale[k] = lit->vals[k];
                        valueToDot3LitPackScaled[inst.result] = b;
                        break;
                    }
                }

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

                // std::fprintf(stderr, "DEBUG Arith: op=%s result=%%%u a=%%%u b=%%%u aIn=%s bIn=%s aUni=%s bUni=%s\n",
                //              inst.op == IROp::Mul ? "Mul" : inst.op == IROp::Add ? "Add" : "?",
                //              (unsigned)inst.result, (unsigned)ma.baseId, (unsigned)mb.baseId,
                //              aInput != valueToInputSrc.end() ? "yes" : "no",
                //              bInput != valueToInputSrc.end() ? "yes" : "no",
                //              aUniform != valueToFpUniform.end() ? "yes" : "no",
                //              bUniform != valueToFpUniform.end() ? "yes" : "no");

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
                else if (aInput != valueToInputSrc.end() && bInput == valueToInputSrc.end() &&
                         bUniform == valueToFpUniform.end() &&
                         inst.op == IROp::Mul &&
                         valueToTex.count(mb.baseId) == 0 &&
                         !(valueToScalarExtract.count(mb.baseId) &&
                           valueToTex.count(valueToScalarExtract[mb.baseId].baseId)))
                {
                    // (varying, temp) Mul — preload the varying into
                    // H2 via MOVH then emit MUL with the temp as SRC0
                    // and H2 as SRC1.  Used for post-discard output
                    // mixing like `pass_alpha * tex_alpha`.
                    // Exclude tex-sample results (handled separately
                    // by FpVaryingTexMulBinding for Select shapes).
                    bind.inputId    = ma.baseId;
                    bind.inputAbs   = ma.absMod;
                    bind.inputNeg   = ma.negMod;
                    // Reuse uniformId as the temp operand id.
                    bind.uniformId  = mb.baseId;
                    bind.uniformAbs = false;
                    bind.uniformNeg = false;
                    valueToArith[inst.result] = bind;
                    break;
                }
                else if (aInput == valueToInputSrc.end() && bInput != valueToInputSrc.end() &&
                         aUniform == valueToFpUniform.end() &&
                         inst.op == IROp::Mul &&
                         valueToTex.count(ma.baseId) == 0 &&
                         !(valueToScalarExtract.count(ma.baseId) &&
                           valueToTex.count(valueToScalarExtract[ma.baseId].baseId)))
                {
                    bind.inputId    = mb.baseId;
                    bind.inputAbs   = mb.absMod;
                    bind.inputNeg   = mb.negMod;
                    bind.uniformId  = ma.baseId;
                    bind.uniformAbs = false;
                    bind.uniformNeg = false;
                    valueToArith[inst.result] = bind;
                    break;
                }
                else
                {
                    // std::fprintf(stderr, "DEBUG Arith: entering else block, op=%s result=%%%u\n",
                    //              inst.op == IROp::Mul ? "Mul" : inst.op == IROp::Add ? "Add" : "?",
                    //              (unsigned)inst.result);
                    // Repeated-add fold: `Add(x, x)` where x is a
                    // direct varying — emit later as MOVH+MAD with
                    // multiplier (scale-1).  Extends through chained
                    // `Add(scale, x)` to grow the scale by 1 per link.
                    // Restricted to Add (Sub flips a sign; Mul/Min/Max
                    // don't have the linear-scale identity).
                    if (inst.op == IROp::Add)
                    {
                        // Shape A: extend an existing scale binding.
                        // `Add(scale, x)` where x is the same direct
                        // varying that the scale tracks (compared as
                        // SSA value-ID; emit-time will resolve to the
                        // INPUT_SRC code via valueToInputSrc).
                        // Full-vec4 path (lane == -1): capped at 64,
                        // emitter switches from MOVH+MAD (N<=3) to
                        // MOVH+MULR+chain+[FENCBR]+ADD/MAD (N>=4).
                        // Scalar-lane path (lane >= 0): capped at 3 —
                        // sce-cgc uses a different DP2R-based shape
                        // for N>=4 we haven't implemented yet.
                        constexpr int kMaxScalarLaneScale = 3;
                        constexpr int kMaxFullVec4Scale   = 64;
                        auto scaleCap = [](const FpScaleVaryingBinding& b) {
                            return b.lane >= 0 ? kMaxScalarLaneScale
                                               : kMaxFullVec4Scale;
                        };
                        if (valueToScale.count(ma.baseId) &&
                            valueToScale[ma.baseId].inputId == mb.baseId &&
                            valueToScale[ma.baseId].scale <
                                scaleCap(valueToScale[ma.baseId]))
                        {
                            FpScaleVaryingBinding b = valueToScale[ma.baseId];
                            b.scale += 1;
                            valueToScale[inst.result] = b;
                            break;
                        }
                        if (valueToScale.count(mb.baseId) &&
                            valueToScale[mb.baseId].inputId == ma.baseId &&
                            valueToScale[mb.baseId].scale <
                                scaleCap(valueToScale[mb.baseId]))
                        {
                            FpScaleVaryingBinding b = valueToScale[mb.baseId];
                            b.scale += 1;
                            valueToScale[inst.result] = b;
                            break;
                        }

                        // Shape B: seed a fresh scale binding for
                        // `Add(x, x)` where both operands resolve to
                        // the same direct varying.
                        if (ma.baseId == mb.baseId &&
                            valueToInputSrc.count(ma.baseId) &&
                            !ma.absMod && !ma.negMod &&
                            !mb.absMod && !mb.negMod)
                        {
                            FpScaleVaryingBinding b;
                            b.inputId = ma.baseId;
                            b.scale   = 2;
                            b.lane    = -1;  // full vec4
                            valueToScale[inst.result] = b;
                            break;
                        }

                        // Shape C: scalar-lane variant — `Add(v.x, v.x)`
                        // after CSE collapses identical ScalarExtract
                        // results.  Both operands must be the same
                        // ScalarExtract value-ID, the underlying
                        // varying must be a direct INPUT, and no
                        // abs/neg mods.  Lane is propagated to emit
                        // time so MOVH/MAD use the right OUTMASK.
                        if (ma.baseId == mb.baseId &&
                            valueToScalarExtract.count(ma.baseId) &&
                            !ma.absMod && !ma.negMod &&
                            !mb.absMod && !mb.negMod)
                        {
                            const auto& se = valueToScalarExtract[ma.baseId];
                            if (valueToInputSrc.count(se.baseId))
                            {
                                FpScaleVaryingBinding b;
                                b.inputId = se.baseId;
                                b.scale   = 2;
                                b.lane    = se.lane;
                                valueToScale[inst.result] = b;
                                break;
                            }
                        }

                        // Shape D: extend a scalar-lane scale binding
                        // when the next link is the same ScalarExtract
                        // value (or any ScalarExtract that resolves to
                        // the same varying + lane).
                        auto extendScalarLane = [&](IRValueID prevId,
                                                    IRValueID rhsId) -> bool
                        {
                            auto sIt = valueToScale.find(prevId);
                            if (sIt == valueToScale.end()) return false;
                            if (sIt->second.lane < 0)       return false;
                            if (sIt->second.scale >= kMaxScalarLaneScale) return false;
                            auto seIt = valueToScalarExtract.find(rhsId);
                            if (seIt == valueToScalarExtract.end()) return false;
                            if (seIt->second.baseId != sIt->second.inputId ||
                                seIt->second.lane   != sIt->second.lane)
                                return false;
                            FpScaleVaryingBinding b = sIt->second;
                            b.scale += 1;
                            valueToScale[inst.result] = b;
                            return true;
                        };
                        if (extendScalarLane(ma.baseId, mb.baseId)) break;
                        if (extendScalarLane(mb.baseId, ma.baseId)) break;
                    }

                    // Chain detection — only meaningful for Add today.
                    // Shape A: extend an existing chain: operand[0] is
                    //         already a chain binding, operand[1] is a
                    //         uniform.  Adds one Link.
                    // Shape B: promote an arith binding into a fresh
                    //         chain: operand[0] is a deferred Add
                    //         arith with (varying, uniform), operand[1]
                    //         is a uniform.  This becomes a 2-link
                    //         chain.
                    // Sub treated like Add via uniformNeg toggle so
                    // mixed `vcol + a - b + c` chains lower correctly
                    // once Sub is allowed at the IR-level chain check.
                    if (inst.op == IROp::Add || inst.op == IROp::Sub)
                    {
                        auto promoteOrExtend = [&](IRValueID prevId,
                                                   IRValueID rhsUniformId,
                                                   const SrcMod& rhsMod) -> bool
                        {
                            auto cIt = valueToChain.find(prevId);
                            if (cIt != valueToChain.end())
                            {
                                FpChainBinding ch = cIt->second;
                                FpChainBinding::Link link;
                                link.op         = FpArithOp::Add;
                                link.uniformId  = rhsUniformId;
                                link.uniformAbs = rhsMod.absMod;
                                link.uniformNeg = rhsMod.negMod;
                                if (inst.op == IROp::Sub)
                                    link.uniformNeg = !link.uniformNeg;
                                ch.links.push_back(link);
                                valueToChain[inst.result] = std::move(ch);
                                return true;
                            }
                            auto aIt = valueToArith.find(prevId);
                            if (aIt != valueToArith.end() &&
                                aIt->second.op == FpArithOp::Add)
                            {
                                const auto& prior = aIt->second;
                                FpChainBinding ch;
                                ch.rootInputId  = prior.inputId;
                                ch.rootInputAbs = prior.inputAbs;
                                ch.rootInputNeg = prior.inputNeg;
                                FpChainBinding::Link l0;
                                l0.op         = FpArithOp::Add;
                                l0.uniformId  = prior.uniformId;
                                l0.uniformAbs = prior.uniformAbs;
                                l0.uniformNeg = prior.uniformNeg;
                                ch.links.push_back(l0);
                                FpChainBinding::Link l1;
                                l1.op         = FpArithOp::Add;
                                l1.uniformId  = rhsUniformId;
                                l1.uniformAbs = rhsMod.absMod;
                                l1.uniformNeg = rhsMod.negMod;
                                if (inst.op == IROp::Sub)
                                    l1.uniformNeg = !l1.uniformNeg;
                                ch.links.push_back(l1);
                                valueToChain[inst.result] = std::move(ch);
                                return true;
                            }
                            return false;
                        };
                        // Either ordering — chain on left or right.
                        if (bUniform != valueToFpUniform.end() &&
                            promoteOrExtend(ma.baseId, mb.baseId, mb))
                            break;
                        if (aUniform != valueToFpUniform.end() &&
                            inst.op == IROp::Add &&
                            promoteOrExtend(mb.baseId, ma.baseId, ma))
                            break;
                    }

                    // Mul(varying, tex_sample_result) — the
                    // uniform-conditional THEN shape.  Record the pair
                    // and let the Select dispatch consume it; nothing
                    // else can lower this binding today.
                    // Also matches Mul(varying, scalar_extract(tex, lane))
                    // used in post-discard VecConstruct output mixing.
                    if (inst.op == IROp::Mul)
                    {
                        // std::fprintf(stderr, "DEBUG Arith: entering Mul(varying,tex) check, result=%%%u\n",
                        //              (unsigned)inst.result);
                        auto resolveTexBase = [&](IRValueID id) -> IRValueID {
                            auto seIt = valueToScalarExtract.find(id);
                            if (seIt != valueToScalarExtract.end())
                            {
                                if (valueToTex.count(seIt->second.baseId))
                                    return seIt->second.baseId;
                            }
                            if (valueToTex.count(id))
                                return id;
                            return 0;
                        };
                        // Resolve varying through scalar extracts:
                        // `shuffle vec4_input[lane]` produces a scalar
                        // that is only in valueToScalarExtract, not in
                        // valueToInputSrc (which maps the base vec4).
                        auto resolveVarying = [&](IRValueID id) -> bool {
                            if (valueToInputSrc.count(id)) return true;
                            auto seIt = valueToScalarExtract.find(id);
                            return seIt != valueToScalarExtract.end() &&
                                   valueToInputSrc.count(seIt->second.baseId);
                        };
                        auto aIsVar = resolveVarying(ma.baseId);
                        auto bIsVar = resolveVarying(mb.baseId);
                        auto aTexBase = resolveTexBase(ma.baseId);
                        auto bTexBase = resolveTexBase(mb.baseId);
                        // Resolve the varying base ID: if the operand
                        // is a scalar extract of a vec4 input, use the
                        // underlying input so emit-time valueToInputSrc
                        // lookup succeeds.
                        auto varyingBaseId = [&](IRValueID id) -> IRValueID {
                            auto seIt = valueToScalarExtract.find(id);
                            if (seIt != valueToScalarExtract.end() &&
                                valueToInputSrc.count(seIt->second.baseId))
                                return seIt->second.baseId;
                            return id;
                        };
                        // Resolve the scalar lane of the varying.
                        // Returns -1 if the varying is a scalar (not
                        // a lane extract), otherwise the lane index.
                        auto varyingLane = [&](IRValueID id) -> int {
                            auto seIt = valueToScalarExtract.find(id);
                            if (seIt != valueToScalarExtract.end() &&
                                valueToInputSrc.count(seIt->second.baseId))
                                return seIt->second.lane;
                            return -1;
                        };
                        if (aIsVar && bTexBase)
                        {
                            FpVaryingTexMulBinding tb;
                            tb.varyingId = varyingBaseId(ma.baseId);
                            tb.texId     = bTexBase;
                            tb.lane      = varyingLane(ma.baseId);
                            valueToVaryingTexMul[inst.result] = tb;
                            break;
                        }
                        if (bIsVar && aTexBase)
                        {
                            FpVaryingTexMulBinding tb;
                            tb.varyingId = varyingBaseId(mb.baseId);
                            tb.texId     = aTexBase;
                            tb.lane      = varyingLane(mb.baseId);
                            valueToVaryingTexMul[inst.result] = tb;
                            break;
                        }
                    }

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

            case IROp::Cos:
            case IROp::Sin:
            {
                // `cos(scalar_lane_of_varying)` / `sin(...)`.  Same
                // extract-then-scalar-op shape as RSQ/RCP, but the
                // NV40 FP COS / SIN opcodes (0x22 / 0x23) read the
                // varying directly without an MOVH preload.  Emit
                // path picks single-insn vs preload-then-op based on
                // Kind.
                if (inst.operands.size() != 1) break;
                auto seIt = valueToScalarExtract.find(inst.operands[0]);
                if (seIt == valueToScalarExtract.end()) break;

                FpScalarUnaryBinding bind;
                bind.kind    = (inst.op == IROp::Cos)
                    ? FpScalarUnaryBinding::Kind::Cos
                    : FpScalarUnaryBinding::Kind::Sin;
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
                // `normalize(vec3)` — input may be a direct varying
                // (existing path) OR an arith binding such as
                // `varying ± uniform` (new path).  The `wrapW1` flag
                // gets set later when VecConstruct sees this feeding
                // into `float4(nrm, 1)`.
                if (inst.operands.size() != 1) break;
                const SrcMod m = resolveSrcMods(inst.operands[0]);

                int lanes = 3;
                auto tyIt = valueToType.find(inst.operands[0]);
                if (tyIt != valueToType.end() && tyIt->second.vectorSize > 0)
                    lanes = tyIt->second.vectorSize;

                FpNormalizeBinding bind;
                bind.lanes = lanes;

                if (valueToInputSrc.find(m.baseId) != valueToInputSrc.end())
                {
                    bind.inputId = m.baseId;
                    valueToNormalize[inst.result] = bind;
                    break;
                }

                // Try to resolve the operand as an arith binding —
                // covers `normalize(varying - uniform)` etc.
                auto arithIt = valueToArith.find(m.baseId);
                if (arithIt != valueToArith.end() &&
                    (arithIt->second.op == FpArithOp::Add))
                {
                    const auto& a = arithIt->second;
                    bind.hasArith        = true;
                    bind.arithOp         = a.op;
                    bind.arithInputId    = a.inputId;
                    bind.arithUniformId  = a.uniformId;
                    bind.arithUniformNeg = a.uniformNeg;
                    valueToNormalize[inst.result] = bind;
                    break;
                }
                break;
            }

            case IROp::Reflect:
            {
                // `reflect(I, N)` — both operands must resolve to
                // varying inputs (possibly through identity-prefix
                // VecShuffle aliases).  See FpReflectBinding's docs
                // for the lowering shape.
                if (inst.operands.size() != 2) break;
                const SrcMod mI = resolveSrcMods(inst.operands[0]);
                const SrcMod mN = resolveSrcMods(inst.operands[1]);
                if (!valueToInputSrc.count(mI.baseId) ||
                    !valueToInputSrc.count(mN.baseId))
                    break;
                FpReflectBinding bind;
                bind.iId = mI.baseId;
                bind.nId = mN.baseId;
                valueToReflect[inst.result] = bind;
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

                // Shape A4b: `vec3(d1, d2, d3)` where each is a
                // distinct DotLitPack against the same varying-pack
                // base.  Pre-wrap binding consumed by Mul(literal,
                // this_vec3) or VecConstruct(this_vec3, 1.0).
                if (inst.operands.size() == 3)
                {
                    auto it0 = valueToDotLitPack.find(inst.operands[0]);
                    auto it1 = valueToDotLitPack.find(inst.operands[1]);
                    auto it2 = valueToDotLitPack.find(inst.operands[2]);
                    if (it0 != valueToDotLitPack.end() &&
                        it1 != valueToDotLitPack.end() &&
                        it2 != valueToDotLitPack.end() &&
                        it0->second.pack.baseId == it1->second.pack.baseId &&
                        it1->second.pack.baseId == it2->second.pack.baseId &&
                        it0->second.pack.width  == it1->second.pack.width  &&
                        it1->second.pack.width  == it2->second.pack.width)
                    {
                        FpDot3LitPackBinding b;
                        b.pack = it0->second.pack;
                        for (int k = 0; k < 4; ++k) b.literals[0][k] = it0->second.literal[k];
                        for (int k = 0; k < 4; ++k) b.literals[1][k] = it1->second.literal[k];
                        for (int k = 0; k < 4; ++k) b.literals[2][k] = it2->second.literal[k];
                        valueToDot3LitPack[inst.result] = b;
                        break;
                    }
                }

                // Shape A4e: `vec4(scaled-dots-minus-lit-mul-uniform,
                // 1.0)` — 2-operand wrap of the FragmentProgram-style
                // Sub binding.  Lowers to 9 instructions; see the
                // FpScaledDotsMinusLitMulUniformWrapBinding header.
                if (inst.operands.size() == 2)
                {
                    auto sIt =
                        valueToScaledDotsMinusLitMulUni.find(inst.operands[0]);
                    if (sIt != valueToScaledDotsMinusLitMulUni.end())
                    {
                        IRValue* wv = entry.getValue(inst.operands[1]);
                        auto* wc = dynamic_cast<IRConstant*>(wv);
                        if (wc && std::holds_alternative<float>(wc->value) &&
                            std::get<float>(wc->value) == 1.0f)
                        {
                            FpScaledDotsMinusLitMulUniformWrapBinding wb;
                            wb.sub = sIt->second;
                            valueToScaledDotsMinusLitMulUniWrap[inst.result] = wb;
                            break;
                        }
                    }
                }

                // Shape A4d: `vec4(literal_vec3 * uniform_scalar, 1.0)`
                // — 2-operand wrap of a FpLitVecUniformScaleBinding.
                if (inst.operands.size() == 2)
                {
                    auto luIt = valueToLitVecUniformScale.find(inst.operands[0]);
                    if (luIt != valueToLitVecUniformScale.end())
                    {
                        IRValue* wv = entry.getValue(inst.operands[1]);
                        auto* wc = dynamic_cast<IRConstant*>(wv);
                        if (wc && std::holds_alternative<float>(wc->value) &&
                            std::get<float>(wc->value) == 1.0f)
                        {
                            FpLitVecUniformScaleWrapBinding wb;
                            for (int k = 0; k < 4; ++k)
                                wb.literal[k] = luIt->second.literal[k];
                            wb.uniformId = luIt->second.uniformId;
                            wb.vecWidth  = luIt->second.vecWidth;
                            valueToLitVecUniformScaleWrap[inst.result] = wb;
                            break;
                        }
                    }
                }

                // Shape A4c: `vec4(scaled-or-bare 3-dot pack, 1.0)` —
                // 2-operand wrap.  Operand 0 is either an
                // FpDot3LitPack (no scale) or an FpDot3LitPackScaled.
                // Operand 1 must be the literal 1.0.
                if (inst.operands.size() == 2)
                {
                    auto bareIt   = valueToDot3LitPack.find(inst.operands[0]);
                    auto scaledIt = valueToDot3LitPackScaled.find(inst.operands[0]);
                    const bool isBare   = (bareIt   != valueToDot3LitPack.end());
                    const bool isScaled = (scaledIt != valueToDot3LitPackScaled.end());
                    if (isBare || isScaled)
                    {
                        IRValue* wv = entry.getValue(inst.operands[1]);
                        auto* wc = dynamic_cast<IRConstant*>(wv);
                        if (wc && std::holds_alternative<float>(wc->value) &&
                            std::get<float>(wc->value) == 1.0f)
                        {
                            FpDot3LitPackWrapBinding wb;
                            const FpDot3LitPackBinding& src =
                                isBare ? bareIt->second : scaledIt->second.base;
                            wb.pack = src.pack;
                            for (int r = 0; r < 3; ++r)
                                for (int k = 0; k < 4; ++k)
                                    wb.literals[r][k] = src.literals[r][k];
                            if (isScaled)
                            {
                                wb.hasScale = true;
                                for (int k = 0; k < 4; ++k)
                                    wb.scale[k] = scaledIt->second.scale[k];
                            }
                            valueToDot3LitPackWrap[inst.result] = wb;
                            break;
                        }
                    }
                }

                // Shape A2: `float4(reflect(I, N), 1.0f)` — same idea
                // as Shape A but for Reflect.  Aliases the
                // VecConstruct's result to the same reflect binding
                // with wrapW1=true; StoreOutput appends a tail MOV
                // R0.w = 1.0 + inline {1, 0, 0, 0} after the MAD.
                if (inst.operands.size() == 2)
                {
                    auto rIt = valueToReflect.find(inst.operands[0]);
                    if (rIt != valueToReflect.end())
                    {
                        IRValue* wv = entry.getValue(inst.operands[1]);
                        auto* wc = dynamic_cast<IRConstant*>(wv);
                        if (wc && std::holds_alternative<float>(wc->value) &&
                            std::get<float>(wc->value) == 1.0f)
                        {
                            FpReflectBinding bind = rIt->second;
                            bind.wrapW1 = true;
                            valueToReflect[inst.result] = bind;
                            break;
                        }
                    }
                }

                // Shape A3: `vec4(d, d, d, 1.0)` where d is a Dot
                // result that resolved as a literal × varying-pack —
                // alias the result to the same DotLitPack binding
                // with wrapW1=true.  StoreOutput then emits the
                // 3-instruction shape (pack MOV + W=1 MOV + DP2/END).
                if (inst.operands.size() == 4)
                {
                    auto dlpIt = valueToDotLitPack.find(inst.operands[0]);
                    if (dlpIt != valueToDotLitPack.end() &&
                        inst.operands[1] == inst.operands[0] &&
                        inst.operands[2] == inst.operands[0])
                    {
                        IRValue* wv = entry.getValue(inst.operands[3]);
                        auto* wc = dynamic_cast<IRConstant*>(wv);
                        if (wc && std::holds_alternative<float>(wc->value) &&
                            std::get<float>(wc->value) == 1.0f)
                        {
                            FpDotLitPackBinding bind = dlpIt->second;
                            bind.wrapW1 = true;
                            valueToDotLitPack[inst.result] = bind;
                            break;
                        }
                    }
                }

                // Shape A4: `vec4(d1, d2, d3, 1.0)` where d1/d2/d3 are
                // 3 distinct DotLitPacks sharing the same varying-pack
                // base.  Records FpDot3LitPackWrapBinding for the
                // 5-insn StoreOutput emit.
                if (inst.operands.size() == 4)
                {
                    auto it0 = valueToDotLitPack.find(inst.operands[0]);
                    auto it1 = valueToDotLitPack.find(inst.operands[1]);
                    auto it2 = valueToDotLitPack.find(inst.operands[2]);
                    if (it0 != valueToDotLitPack.end() &&
                        it1 != valueToDotLitPack.end() &&
                        it2 != valueToDotLitPack.end() &&
                        it0->second.pack.baseId == it1->second.pack.baseId &&
                        it1->second.pack.baseId == it2->second.pack.baseId &&
                        it0->second.pack.width  == it1->second.pack.width  &&
                        it1->second.pack.width  == it2->second.pack.width)
                    {
                        IRValue* wv = entry.getValue(inst.operands[3]);
                        auto* wc = dynamic_cast<IRConstant*>(wv);
                        if (wc && std::holds_alternative<float>(wc->value) &&
                            std::get<float>(wc->value) == 1.0f)
                        {
                            FpDot3LitPackWrapBinding b;
                            b.pack = it0->second.pack;
                            for (int k = 0; k < 4; ++k) b.literals[0][k] = it0->second.literal[k];
                            for (int k = 0; k < 4; ++k) b.literals[1][k] = it1->second.literal[k];
                            for (int k = 0; k < 4; ++k) b.literals[2][k] = it2->second.literal[k];
                            valueToDot3LitPackWrap[inst.result] = b;
                            break;
                        }
                    }
                }

                // Shape E: `vec2(varying.X, varying.Y)` (or wider) —
                // every operand is a scalar lane of the SAME entry-
                // point varying.  Recorded as a varying-pack so a
                // consumer (Dot, etc.) can fold the pack-MOV into
                // its own emit.  Plain VecConstruct fall-through
                // for non-pack shapes.
                if (inst.operands.size() >= 2 && inst.operands.size() <= 4)
                {
                    int  packLanes[4] = {0, 0, 0, 0};
                    IRValueID baseId  = 0;
                    bool      ok      = true;
                    for (size_t k = 0; k < inst.operands.size(); ++k)
                    {
                        auto seIt = valueToScalarExtract.find(inst.operands[k]);
                        if (seIt == valueToScalarExtract.end()) { ok = false; break; }
                        if (k == 0)
                        {
                            baseId = seIt->second.baseId;
                            if (valueToInputSrc.find(baseId) ==
                                valueToInputSrc.end())
                            { ok = false; break; }
                        }
                        else if (seIt->second.baseId != baseId)
                        {
                            ok = false; break;
                        }
                        packLanes[k] = seIt->second.lane;
                    }
                    if (ok)
                    {
                        FpVaryingPackBinding pb;
                        pb.baseId = baseId;
                        pb.width  = static_cast<int>(inst.operands.size());
                        for (int k = 0; k < 4; ++k) pb.lanes[k] = packLanes[k];
                        valueToVaryingPack[inst.result] = pb;
                        // Don't break — fall through so the all-const
                        // shape recognisers below still get a chance
                        // (won't apply, but keeps the dispatch tidy).
                    }
                }

                // Shape A5: `float4(tex.x, tex.y, tex.z, varying * tex.w)` —
                // post-discard output mix of tex-result identity lanes
                // plus one (varying, tex) Mul lane.  sce-cgc emits:
                //   MOVH H2.x, f[varying]; MOVR R0.xyz, R0; MULR R0.w, R0, H2.x
                if (inst.operands.size() == 4)
                {
                    // DEBUG VecConstruct: checking Shape A5
                    // std::fprintf(stderr, "DEBUG VecConstruct: checking Shape A5, result=%%%u\n",
                    //              (unsigned)inst.result);
                    // for (int k = 0; k < 4; ++k)
                    // {
                    //     auto seIt = valueToScalarExtract.find(inst.operands[k]);
                    //     auto vmIt = valueToVaryingTexMul.find(inst.operands[k]);
                    //     std::fprintf(stderr, "  op[%d]=%%%u se=%s vm=%s\n", k,
                    //                  (unsigned)inst.operands[k],
                    //                  seIt != valueToScalarExtract.end() ? "yes" : "no",
                    //                  vmIt != valueToVaryingTexMul.end() ? "yes" : "no");
                    // }
                    // Find the tex base — all identity lanes must share it.
                    IRValueID texBase = 0;
                    int mulLane = -1;
                    bool patternOk = true;
                    for (int k = 0; k < 4 && patternOk; ++k)
                    {
                        auto seIt = valueToScalarExtract.find(inst.operands[k]);
                        if (seIt != valueToScalarExtract.end() &&
                            seIt->second.lane == k)
                        {
                            // Identity lane — must be from a tex result
                            auto txIt = valueToTex.find(seIt->second.baseId);
                            if (txIt == valueToTex.end())
                                { patternOk = false; break; }
                            if (texBase == 0)
                                texBase = seIt->second.baseId;
                            else if (texBase != seIt->second.baseId)
                                { patternOk = false; break; }
                        }
                        else if (k == 3)
                        {
                            // w lane — may be the Mul (either captured
                            // as FpVaryingTexMulBinding or as (varying,temp)
                            // via valueToArith)
                            auto vmIt = valueToVaryingTexMul.find(inst.operands[k]);
                            if (vmIt != valueToVaryingTexMul.end())
                            {
                                if (texBase != 0 && vmIt->second.texId != texBase)
                                    { patternOk = false; break; }
                                texBase = vmIt->second.texId;
                                mulLane = k;
                            }
                            else
                            {
                                // Try (varying, temp) Mul via valueToArith
                                auto arIt = valueToArith.find(inst.operands[k]);
                                if (arIt == valueToArith.end() ||
                                    arIt->second.op != FpArithOp::Mul)
                                    { patternOk = false; break; }
                                // The temp operand (uniformId) must be
                                // a ScalarExtract from our tex base
                                auto seIt = valueToScalarExtract.find(
                                    arIt->second.uniformId);
                                if (seIt == valueToScalarExtract.end())
                                    { patternOk = false; break; }
                                auto txIt = valueToTex.find(seIt->second.baseId);
                                if (txIt == valueToTex.end())
                                    { patternOk = false; break; }
                                if (texBase != 0 &&
                                    seIt->second.baseId != texBase)
                                    { patternOk = false; break; }
                                texBase = seIt->second.baseId;
                                mulLane = k;
                            }
                        }
                        else
                        {
                            patternOk = false;
                        }
                    }
                    if (patternOk && mulLane == 3 && texBase != 0)
                    {
                        FpVecConstructTexMulBinding b;
                        b.texBaseId = texBase;
                        b.mulLane   = 3;
                        // Resolve varying: check FpVaryingTexMulBinding first,
                        // then valueToArith for (varying, temp) Mul.
                        auto vmIt = valueToVaryingTexMul.find(inst.operands[3]);
                        if (vmIt != valueToVaryingTexMul.end())
                        {
                            b.varyingId    = vmIt->second.varyingId;
                            b.varyingLane  = vmIt->second.lane;
                        }
                        else
                        {
                            auto arIt = valueToArith.find(inst.operands[3]);
                            b.varyingId = arIt->second.inputId;
                        }
                        // DEBUG: Shape A5 matched
                        // std::fprintf(stderr, "DEBUG VecConstruct: Shape A5 matched! tex=%%%u var=%%%u\n",
                        //              (unsigned)b.texBaseId, (unsigned)b.varyingId);
                        valueToVecConstructTexMul[inst.result] = b;
                        break;
                    }
                }

                // Shape B: `float4(k0, k1, k2, k3)` where every kN is
                // a scalar float constant.  Other VecConstruct shapes
                // (mixed input + const, narrower constructors) land
                // with the arithmetic pass.
                if (inst.operands.size() != 4) break;

                // Shape C: `float4(scale_scalar, k1, k2, k3)` —
                // exactly one operand is a scalar-lane scale binding,
                // the other three are float constants.  Captures the
                // sce-cgc accumulator-fold output shape (see
                // loop_static3_f).  Detect first so the all-const
                // path doesn't fire on mixed inputs.
                {
                    int scaleSlot = -1;
                    FpScaleVaryingBinding scaleBind;
                    float consts[4] = {0,0,0,0};
                    bool allOk = true;
                    for (int k = 0; k < 4; ++k)
                    {
                        const IRValueID op = inst.operands[k];
                        auto sIt = valueToScale.find(op);
                        if (sIt != valueToScale.end() && sIt->second.lane >= 0)
                        {
                            if (scaleSlot >= 0) { allOk = false; break; }
                            scaleSlot = k;
                            scaleBind = sIt->second;
                            continue;
                        }
                        IRValue* v = entry.getValue(op);
                        auto* c = dynamic_cast<IRConstant*>(v);
                        if (!c) { allOk = false; break; }
                        // Cg integer literals in a float4 constructor
                        // (e.g. `float4(acc, 0, 0, 1)`) reach the IR as
                        // int32_t constants; the front-end leaves the
                        // implicit int→float promotion to lowering.
                        if (std::holds_alternative<float>(c->value))
                            consts[k] = std::get<float>(c->value);
                        else if (std::holds_alternative<int32_t>(c->value))
                            consts[k] = static_cast<float>(std::get<int32_t>(c->value));
                        else { allOk = false; break; }
                    }
                    if (allOk && scaleSlot >= 0)
                    {
                        FpScaledLanesBinding b;
                        b.scale       = scaleBind;
                        b.scaledLane  = scaleSlot;
                        for (int k = 0; k < 4; ++k) b.consts[k] = consts[k];
                        valueToScaledLanes[inst.result] = b;
                        break;
                    }
                }

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

                // Peel VecInsert chains: each link overrides one lane
                // of the underlying vector with a scalar value.  We
                // walk inward (outer-most VecInsert is the LAST to
                // apply) and emit overrides in source order — the
                // reference compiler's pattern is "underlying producer
                // writes the full vec, then a per-lane MOV writes the
                // overridden lane(s) to the same destination".
                struct LaneOverride
                {
                    int       lane     = 0;
                    IRValueID scalarId = 0;
                };
                std::vector<LaneOverride> laneOverrides;
                while (true)
                {
                    auto viIt = valueToVecInsert.find(srcId);
                    if (viIt == valueToVecInsert.end()) break;
                    laneOverrides.push_back(
                        { viIt->second.lane, viIt->second.scalarId });
                    srcId = viIt->second.vectorId;
                }
                std::reverse(laneOverrides.begin(), laneOverrides.end());

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

                auto emitLaneOverrides = [&]()
                {
                    // For each override, emit a MOV writing only the
                    // overridden lane to dstReg, with a 16-byte inline
                    // const block holding {scalar, 0, 0, 0}.  SRC0 is
                    // c[0].xxxx (smeared) so the literal lane 0 reaches
                    // the destination's overridden lane.
                    for (const auto& lo : laneOverrides)
                    {
                        IRValue* scV = entry.getValue(lo.scalarId);
                        const auto* sc = dynamic_cast<const IRConstant*>(scV);
                        if (!sc || !std::holds_alternative<float>(sc->value))
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: VecInsert scalar must be a float "
                                "literal (other shapes land later)");
                            return false;
                        }
                        const float lit = std::get<float>(sc->value);

                        const struct nvfx_reg constReg =
                            nvfx_reg(NVFXSR_CONST, 0);
                        struct nvfx_src cs0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        // Smear lane 0 across all source lanes — only
                        // the destination's masked lane consumes it.
                        cs0.swz[0] = cs0.swz[1] = cs0.swz[2] = cs0.swz[3] = 0;
                        struct nvfx_src cs1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src cs2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));

                        const uint8_t laneMask =
                            static_cast<uint8_t>(1u << lo.lane);
                        struct nvfx_insn mov = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            laneMask, cs0, cs1, cs2);
                        mov.precision = dstPrecision;
                        asm_.emit(mov, NVFX_FP_OP_OPCODE_MOV);

                        const float litBlock[4] = { lit, 0.0f, 0.0f, 0.0f };
                        asm_.appendConstBlock(litBlock);
                    }
                    return true;
                };

                // tex2D result consumed by StoreOutput: emit TEX writing
                // straight to the output register (sce-cgc's pattern —
                // no temp, no follow-up MOV).
                auto texIt = valueToTex.find(srcId);
                if (texIt != valueToTex.end())
                {
                    // If the Discard handler already emitted this TEX
                    // inline before a comparison, skip the duplicate.
                    if (emittedTexResults.count(srcId))
                    {
                        // Lane overrides still need to be emitted.
                        if (!emitLaneOverrides()) return out;
                        break;
                    }

                    auto sampIt = valueToTexUnit.find(texIt->second.samplerId);
                    if (sampIt == valueToTexUnit.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: TexSample sampler did not resolve to a uniform sampler");
                        return out;
                    }
                    // Resolve UV through identity-prefix VecShuffle
                    // aliases: `texCUBE(s, worldPos.xyz)` records the
                    // shuffle in valueToMod with baseId pointing at the
                    // underlying varying (worldPos), and the TEX reads
                    // the full input regardless of the swizzle.
                    IRValueID uvBase = resolveSrcMods(texIt->second.uvId).baseId;
                    auto uvIt = valueToInputSrc.find(uvBase);
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

                    // Narrow the TEX's writemask to only the lanes
                    // NOT overridden by trailing VecInsert MOVs — the
                    // reference compiler folds the lane override into
                    // the producer's writemask so the TEX doesn't waste
                    // a write to a lane that's about to be clobbered.
                    uint8_t producerMask = NVFX_FP_MASK_ALL;
                    for (const auto& lo : laneOverrides)
                        producerMask &= static_cast<uint8_t>(
                            ~(1u << lo.lane));

                    struct nvfx_insn in = nvfx_insn(
                        saturate ? 1 : 0, 0,
                        sampIt->second,  // tex_unit — packed into hw[0] bits 17-20
                        -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        producerMask,
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

                    // Mark emitted so the Discard handler doesn't
                    // emit a duplicate TEX for the same tex result.
                    emittedTexResults.insert(srcId);

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
                    if (!emitLaneOverrides()) return out;
                    break;
                }

                // Multi-instruction predicated chain (PredCarry).
                // Lowering for `if (cond) { r = OP1; r = OP2; ... }`
                // with explicit default before the if statement.
                //
                // sce-cgc shape (probed against if_then_2add /
                // if_then_3insn / fenc_R0R1_n4 / n5):
                //
                //   MOVH H<i>, f[lhs]                      (LHS varying → H-temp)
                //   SGTRC RC.x, H<i>, {cmpConst,...}.x
                //   <preload of any varying read by the chain>
                //   MOVR Rdst[0], src(default)             (carry default)
                //   <OP1>R Rdst[0](NE.x), srcs_1
                //   MOVR Rdst[1], Rdst[0]                  (carry running)
                //   <OP2>R Rdst[1](NE.x), srcs_2
                //   ...
                //   MOVR Rdst[N-1], Rdst[N-2]              (carry running)
                //   FENCBR                                 (iff N >= 2)
                //   <OPN>R Rdst[N-1](NE.x), srcs_N      ; PROGRAM_END
                //
                // Register allocation: the chain alternates R0 / R1
                // with the LAST link writing R0.  The LHS-promote
                // register is H0 when N is even (first dst is R1,
                // R0 not touched until link 2) or H2 when N is odd
                // (first dst is R0, would alias H0).  See KNOWN_ISSUES
                // for the no-FENCBR / preload-displaced register case.
                auto pcIt = valueToPredCarry.find(srcId);
                if (pcIt != valueToPredCarry.end())
                {
                    // Walk backwards through the chain (each link's
                    // defaultId points to the prior link or to the
                    // initial default).  Build the chain in
                    // first-to-last order.
                    std::vector<IRValueID> chainIds;
                    IRValueID curr = srcId;
                    while (true)
                    {
                        chainIds.push_back(curr);
                        auto it = valueToPredCarry.find(curr);
                        if (it == valueToPredCarry.end()) break;
                        IRValueID nextDefault = it->second.defaultId;
                        if (valueToPredCarry.find(nextDefault) ==
                            valueToPredCarry.end())
                            break;          // hit the initial default
                        curr = nextDefault;
                    }
                    std::reverse(chainIds.begin(), chainIds.end());
                    const int N = static_cast<int>(chainIds.size());
                    if (N < 1)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: PredCarry chain walked to empty");
                        return out;
                    }

                    const PredCarryBinding& firstBind = valueToPredCarry[chainIds[0]];
                    IRValueID condId        = firstBind.cmpId;
                    IRValueID initialDefault = firstBind.defaultId;

                    auto cmpIt = valueToCmp.find(condId);
                    if (cmpIt == valueToCmp.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: PredCarry needs CmpGt/CmpGe/... binding");
                        return out;
                    }
                    const CmpBinding& cb = cmpIt->second;
                    if (cb.rhsKind != CmpBinding::RhsKind::Literal)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: PredCarry only handles literal-RHS comparisons today");
                        return out;
                    }
                    auto lhsInIt = valueToInputSrc.find(cb.lhsBase);
                    if (lhsInIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: PredCarry compare LHS must be a varying scalar");
                        return out;
                    }

                    // Allocate dst regs: alternate R0/R1 ending at R0.
                    std::vector<int> dstRegs(N);
                    for (int i = 0; i < N; ++i)
                        dstRegs[N - 1 - i] = (i % 2 == 0) ? 0 : 1;

                    // LHS-promote H index — choose so it doesn't
                    // alias the first dst (which is read as the
                    // carry source for link 0).
                    const int hIdx = (N % 2 == 1) ? 2 : 0;

                    // Initial default's source register: must be
                    // resolvable to either INPUT (varying) or a
                    // promoted H-temp.  For if-only-with-default
                    // shape the default is the value stored to the
                    // semantic before the cmp — almost always the
                    // same varying we're about to MOVH into H<hIdx>.
                    auto defaultInIt = valueToInputSrc.find(initialDefault);
                    const bool defaultIsLhsVarying =
                        (defaultInIt != valueToInputSrc.end() &&
                         defaultInIt->second == lhsInIt->second);
                    if (!defaultIsLhsVarying)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: PredCarry default must currently equal the LHS varying");
                        return out;
                    }

                    // ── MOVH H<hIdx>, f[lhs_input] ──
                    struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, hIdx);
                    hReg.is_fp16 = 1;
                    const struct nvfx_reg lhsInReg =
                        nvfx_reg(NVFXSR_INPUT, lhsInIt->second);
                    struct nvfx_src mvhS0 =
                        nvfx_src(const_cast<struct nvfx_reg&>(lhsInReg));
                    struct nvfx_src mvhS1 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1, hReg,
                        NVFX_FP_MASK_ALL, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(lhsInIt->second);
                    if (lhsInIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        lhsInIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (lhsInIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    // ── SGTRC RC.x, H<hIdx>, {cmpConst,...}.x  + inline const ──
                    const struct nvfx_reg ccDst = nvfx_reg(NVFXSR_NONE, 0x3F);
                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    struct nvfx_reg hSrc = nvfx_reg(NVFXSR_TEMP, hIdx);
                    hSrc.is_fp16 = 1;
                    struct nvfx_src cS0 = nvfx_src(hSrc);
                    if (cb.lhsLane != 0)
                        cS0.swz[0] = cS0.swz[1] = cS0.swz[2] = cS0.swz[3] =
                            static_cast<uint8_t>(cb.lhsLane);
                    struct nvfx_src cS1 =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    cS1.swz[0] = cS1.swz[1] = cS1.swz[2] = cS1.swz[3] = 0;
                    struct nvfx_src cS2 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn cmpInsn = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(ccDst),
                        NVFX_FP_MASK_X, cS0, cS1, cS2);
                    cmpInsn.cc_update = 1;
                    asm_.emit(cmpInsn, cb.opcode);
                    const float cmpInline[4] = { cb.rhsLiteral, 0.0f, 0.0f, 0.0f };
                    asm_.appendConstBlock(cmpInline);

                    // Decide which non-LHS varyings need preloading
                    // into R-temps.  For the chain to fit the
                    // "always FENCBR before last" pattern we use one
                    // preload register: R2.  Each unique varying
                    // operand (other than the LHS varying we just
                    // promoted to H) gets a single MOVR R2 preload.
                    //
                    // Today's restriction (matches our current test
                    // surface): all chain links share the same
                    // single non-LHS varying.  Multiple distinct
                    // non-LHS varyings would need an R-temp per
                    // varying + an allocator we don't have yet.
                    int preloadVaryingInput = -1;     // INPUT_SRC index, -1 = none
                    IRValueID preloadVaryingId = InvalidIRValue;
                    for (int i = 0; i < N; ++i)
                    {
                        const auto& bind = valueToPredCarry[chainIds[i]];
                        for (IRValueID arg : bind.opArgs)
                        {
                            if (valueToPredCarry.count(arg)) continue;     // prior pred result
                            auto inIt = valueToInputSrc.find(arg);
                            if (inIt == valueToInputSrc.end()) continue;
                            if (inIt->second == lhsInIt->second) continue; // LHS in H-temp
                            if (preloadVaryingInput < 0)
                            {
                                preloadVaryingInput = inIt->second;
                                preloadVaryingId    = arg;
                            }
                            else if (preloadVaryingInput != inIt->second)
                            {
                                out.diagnostics.push_back(
                                    "nv40-fp: PredCarry chain references multiple "
                                    "non-LHS varyings — single-preload allocator only");
                                return out;
                            }
                        }
                    }

                    constexpr int kPreloadReg = 2;
                    if (preloadVaryingInput >= 0)
                    {
                        const struct nvfx_reg preDst = nvfx_reg(NVFXSR_TEMP, kPreloadReg);
                        const struct nvfx_reg preIn  = nvfx_reg(NVFXSR_INPUT, preloadVaryingInput);
                        struct nvfx_src preS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(preIn));
                        struct nvfx_src preS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src preS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn preInsn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(preDst),
                            NVFX_FP_MASK_ALL, preS0, preS1, preS2);
                        preInsn.precision = FLOAT32;
                        asm_.emit(preInsn, NVFX_FP_OP_OPCODE_MOV);

                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(preloadVaryingInput);
                        if (preloadVaryingInput >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            preloadVaryingInput <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = preloadVaryingInput - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoordsInputMask |= uint16_t{1} << n;
                            // sce-cgc clears the texCoords2D bit for any
                            // TC read full-width via direct INPUT (vs the
                            // MOVH-promote path which carries .xyzw on
                            // its OUTMASK already).  Matches the
                            // varying-Select handlers above.
                            attrs.texCoords2D &= ~(uint16_t{1} << n);
                        }
                    }

                    // ── Emit each chain link: carry-MOVR + predicated OP ──
                    auto regSourceFor = [&](IRValueID id, struct nvfx_reg& outReg) -> bool
                    {
                        // PredCarry result: lives in its dst R-temp.
                        if (auto pIt = predCarryDstReg.find(id);
                            pIt != predCarryDstReg.end())
                        {
                            outReg = nvfx_reg(NVFXSR_TEMP, pIt->second);
                            return true;
                        }
                        // Initial-default (= LHS varying): in H<hIdx>.
                        if (id == initialDefault)
                        {
                            outReg = nvfx_reg(NVFXSR_TEMP, hIdx);
                            outReg.is_fp16 = 1;
                            return true;
                        }
                        // Preloaded non-LHS varying: in R2.
                        if (id == preloadVaryingId)
                        {
                            outReg = nvfx_reg(NVFXSR_TEMP, kPreloadReg);
                            return true;
                        }
                        // Direct LHS varying not promoted (shouldn't
                        // happen given the default-equals-LHS check
                        // above, but be safe): treat as H<hIdx>.
                        if (auto inIt = valueToInputSrc.find(id);
                            inIt != valueToInputSrc.end() &&
                            inIt->second == lhsInIt->second)
                        {
                            outReg = nvfx_reg(NVFXSR_TEMP, hIdx);
                            outReg.is_fp16 = 1;
                            return true;
                        }
                        return false;
                    };

                    for (int i = 0; i < N; ++i)
                    {
                        const PredCarryBinding& bind = valueToPredCarry[chainIds[i]];
                        const int dst = dstRegs[i];

                        // Source for the carry MOVR: either the prior
                        // link's dst (i > 0) or the initial default
                        // (i == 0, which always equals the LHS varying
                        // in H<hIdx>).
                        struct nvfx_reg carrySrc;
                        if (i == 0)
                        {
                            carrySrc = nvfx_reg(NVFXSR_TEMP, hIdx);
                            carrySrc.is_fp16 = 1;
                        }
                        else
                        {
                            carrySrc = nvfx_reg(NVFXSR_TEMP, dstRegs[i - 1]);
                        }

                        // ── MOVR Rdst, carrySrc ──
                        const struct nvfx_reg dstReg2 = nvfx_reg(NVFXSR_TEMP, dst);
                        struct nvfx_src cS0c = nvfx_src(carrySrc);
                        struct nvfx_src cS1c =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src cS2c =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn carryInsn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg2),
                            NVFX_FP_MASK_ALL, cS0c, cS1c, cS2c);
                        carryInsn.precision = FLOAT32;
                        asm_.emit(carryInsn, NVFX_FP_OP_OPCODE_MOV);

                        // FENCBR before the LAST predicated OP if
                        // chain has >= 2 links (matches sce-cgc's
                        // pattern for R0/R1-alternation chains).
                        const bool isLast = (i == N - 1);
                        if (isLast && N >= 2)
                            asm_.emitFencbr();

                        // ── <OP> Rdst(NE.x), op_args ──
                        // Resolve op args to nvfx_src.  Today: 2 args
                        // (Add/Sub/Mul); 3 for Mad (deferred).
                        if (bind.opArgs.size() < 2 || bind.opArgs.size() > 3)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: PredCarry inner op needs 2-3 args");
                            return out;
                        }

                        // Canonicalisation: ADD's two sources get
                        // reordered so the PRELOADED-or-INPUT side is
                        // src0 and the CARRY side (initial default OR
                        // prior pred result) is src1.  MUL keeps the
                        // original IR operand order — sce-cgc doesn't
                        // canonicalise it the same way (probed against
                        // if_then_3insn pred 1 / pred 3 which keep
                        // `MUL carry, preload` as written).  Sub is
                        // modelled as ADD with src1.negate; we don't
                        // canonicalise it (the swap would also flip the
                        // sign).
                        std::vector<IRValueID> orderedArgs = bind.opArgs;
                        auto isCarryArg = [&](IRValueID id) {
                            if (id == initialDefault) return true;
                            return predCarryDstReg.count(id) != 0;
                        };
                        if (bind.innerOp == IROp::Add &&
                            orderedArgs.size() == 2 &&
                            isCarryArg(orderedArgs[0]) &&
                            !isCarryArg(orderedArgs[1]))
                        {
                            std::swap(orderedArgs[0], orderedArgs[1]);
                        }

                        struct nvfx_reg src0Reg, src1Reg, src2Reg = nvfx_reg(NVFXSR_NONE, 0);
                        if (!regSourceFor(orderedArgs[0], src0Reg))
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: PredCarry op arg 0 unresolved");
                            return out;
                        }
                        if (!regSourceFor(orderedArgs[1], src1Reg))
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: PredCarry op arg 1 unresolved");
                            return out;
                        }
                        if (orderedArgs.size() == 3 &&
                            !regSourceFor(orderedArgs[2], src2Reg))
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: PredCarry op arg 2 unresolved");
                            return out;
                        }

                        struct nvfx_src oS0 = nvfx_src(src0Reg);
                        struct nvfx_src oS1 = nvfx_src(src1Reg);
                        struct nvfx_src oS2 = (bind.opArgs.size() == 3)
                            ? nvfx_src(src2Reg)
                            : nvfx_src(const_cast<struct nvfx_reg&>(none));

                        uint8_t opcode = NVFX_FP_OP_OPCODE_MOV;
                        switch (bind.innerOp)
                        {
                        case IROp::Add: opcode = NVFX_FP_OP_OPCODE_ADD; break;
                        case IROp::Sub: opcode = NVFX_FP_OP_OPCODE_ADD; oS1.negate = 1; break;
                        case IROp::Mul: opcode = NVFX_FP_OP_OPCODE_MUL; break;
                        case IROp::Mad: opcode = NVFX_FP_OP_OPCODE_MAD; break;
                        default:
                            out.diagnostics.push_back(
                                "nv40-fp: PredCarry inner op not yet supported");
                            return out;
                        }

                        struct nvfx_insn predInsn = nvfx_insn(
                            (isLast && saturate) ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg2),
                            NVFX_FP_MASK_ALL, oS0, oS1, oS2);
                        predInsn.precision = isLast ? dstPrecision : FLOAT32;
                        predInsn.cc_test  = 1;
                        predInsn.cc_cond  = NVFX_FP_OP_COND_NE;
                        predInsn.cc_swz[0] = 0;
                        predInsn.cc_swz[1] = 0;
                        predInsn.cc_swz[2] = 0;
                        predInsn.cc_swz[3] = 0;
                        asm_.emit(predInsn, opcode);

                        predCarryDstReg[chainIds[i]] = dst;
                    }

                    // The final predicated OP wrote to R0 (which IS
                    // the COLOR0 output for fragment shaders).  No
                    // separate stout instruction needed — markEnd()
                    // will stamp PROGRAM_END on whichever instruction
                    // landed last (the predicated final OP).
                    emittedSomething = true;
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

                    // Tex-result-LHS Select schedule (alpha-mask shape):
                    //   `if (tex.<lane> CMP scalar) trueBr = literal_vec4(0)
                    //    else                       falseBr = varying`
                    //
                    // Reference compiler lowers this to 3 instructions
                    // (verified byte-for-byte on the alpha-mask probe):
                    //
                    //   TEX  R1.xyzw, f[TC<m>], TEX<m>
                    //   SLE  [OUT_NONE | COND_WRITE | OUTMASK=X]
                    //         R1.<lane>{smeared}, c[0].xxxx
                    //         + inline {scalar, 0, 0, 0}
                    //   MOV(EQ.xxxx) [END] R0.xyzw, f[falseBranch]
                    //
                    // The literal trueBr is *elided*: R0 starts at
                    // (0,0,0,0) per FP convention, and the conditional
                    // MOV only fires when the cmp was FALSE — so when
                    // it doesn't fire, R0 stays at the zero default,
                    // matching the source's literal-vec4(0,0,0,0)
                    // TRUE-branch.
                    if (cb.lhsKind == CmpBinding::LhsKind::TexResult)
                    {
                        // Verify trueBr is the implicit zero default.
                        auto isLitZero = [&](IRValueID id) -> bool
                        {
                            auto it = valueToLiteralVec4.find(id);
                            if (it == valueToLiteralVec4.end()) return false;
                            return it->second.vals[0] == 0.0f &&
                                   it->second.vals[1] == 0.0f &&
                                   it->second.vals[2] == 0.0f &&
                                   it->second.vals[3] == 0.0f;
                        };
                        auto findVarying = [&](IRValueID id) -> int
                        {
                            auto it = valueToInputSrc.find(id);
                            return it == valueToInputSrc.end() ? -1 : it->second;
                        };
                        // Walk a VecInsert chain that fully rebuilds an
                        // underlying varying lane-by-lane.  Returns the
                        // varying's input-src index, or -1 if the chain
                        // doesn't have that exact shape.  Used to detect
                        // the alpha_mask split-write source pattern
                        // (`oColor.rgb = color.rgb; oColor.a = color.a;`)
                        // — the IR ends up rebuilding `color` lane-by-
                        // lane, which is logically equivalent to a direct
                        // varying read but the reference compiler emits
                        // a different ucode shape (MOVH preload + 3 cond
                        // MOVs instead of a single cond MOV).
                        auto resolveVecInsertChainToBaseVarying =
                            [&](IRValueID id) -> int {
                            IRValueID laneScalarId[4] = {0,0,0,0};
                            bool      laneOverridden[4] = {false,false,false,false};
                            IRValueID curr = id;
                            while (true)
                            {
                                auto viIt = valueToVecInsert.find(curr);
                                if (viIt == valueToVecInsert.end()) break;
                                const int lane = viIt->second.lane;
                                if (!laneOverridden[lane])
                                {
                                    laneOverridden[lane] = true;
                                    laneScalarId[lane] = viIt->second.scalarId;
                                }
                                curr = viIt->second.vectorId;
                            }
                            for (int i = 0; i < 4; ++i)
                                if (!laneOverridden[i]) return -1;
                            int sharedInput = -1;
                            for (int i = 0; i < 4; ++i)
                            {
                                auto seIt = valueToScalarExtract.find(
                                    laneScalarId[i]);
                                if (seIt == valueToScalarExtract.end())
                                    return -1;
                                if (seIt->second.lane != i) return -1;
                                auto inIt = valueToInputSrc.find(
                                    seIt->second.baseId);
                                if (inIt == valueToInputSrc.end()) return -1;
                                if (sharedInput < 0) sharedInput = inIt->second;
                                else if (inIt->second != sharedInput) return -1;
                            }
                            return sharedInput;
                        };
                        if (!isLitZero(sb.trueId))
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select tex-LHS: TRUE branch must be literal "
                                "vec4(0,0,0,0) (elided as the R0 default) — non-zero "
                                "literal not yet supported");
                            return out;
                        }
                        int falseInputSrc = findVarying(sb.falseId);
                        bool splitWriteShape = false;
                        if (falseInputSrc < 0)
                        {
                            falseInputSrc =
                                resolveVecInsertChainToBaseVarying(sb.falseId);
                            if (falseInputSrc >= 0) splitWriteShape = true;
                        }
                        if (falseInputSrc < 0)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select tex-LHS: FALSE branch must be a "
                                "varying input today");
                            return out;
                        }
                        if (cb.rhsKind != CmpBinding::RhsKind::Literal)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select tex-LHS: RHS must be a scalar literal today");
                            return out;
                        }
                        if (cb.opcode != NVFX_FP_OP_OPCODE_SLE)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select tex-LHS: only cmple ↔ SLE wired today");
                            return out;
                        }

                        auto txIt    = valueToTex.find(cb.lhsTexId);
                        auto sampIt  = (txIt != valueToTex.end())
                            ? valueToTexUnit.find(txIt->second.samplerId)
                            : valueToTexUnit.end();
                        auto uvInIt  = (txIt != valueToTex.end())
                            ? valueToInputSrc.find(txIt->second.uvId)
                            : valueToInputSrc.end();
                        if (txIt == valueToTex.end() ||
                            sampIt == valueToTexUnit.end() ||
                            uvInIt == valueToInputSrc.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select tex-LHS: tex sampler / UV / unit "
                                "binding did not resolve");
                            return out;
                        }

                        // ----- Inst 1: TEX R1, f[TC<m>], TEX<m> -----
                        struct nvfx_reg r1Dst = nvfx_reg(NVFXSR_TEMP, 1);
                        const struct nvfx_reg uvReg =
                            nvfx_reg(NVFXSR_INPUT, uvInIt->second);
                        struct nvfx_src texS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(uvReg));
                        struct nvfx_src texS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src texS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        // Only the lane consumed by the SLE compare needs
                        // to be written — the reference compiler narrows
                        // OUTMASK accordingly so the other lanes of R1
                        // stay undefined (they're never read).
                        const uint32_t texMask =
                            uint32_t{1} << static_cast<uint32_t>(cb.lhsLane);
                        struct nvfx_insn texIn = nvfx_insn(
                            0, 0, sampIt->second, -1,
                            r1Dst, texMask,
                            texS0, texS1, texS2);
                        if (txIt->second.cube) texIn.disable_pc = 1;
                        const uint8_t texOpcode = txIt->second.projective
                            ? NVFX_FP_OP_OPCODE_TXP
                            : NVFX_FP_OP_OPCODE_TEX;
                        asm_.emit(texIn, texOpcode);

                        // Tex-coord 2D bit STAYS SET — TEXCOORD<m> is
                        // consumed as a 2D tex-sample uv, not a
                        // 4-component varying read.
                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(uvInIt->second);
                        if (uvInIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            uvInIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = uvInIt->second - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoordsInputMask |= uint16_t{1} << n;
                            if (txIt->second.projective || txIt->second.cube)
                                attrs.texCoords2D &= ~(uint16_t{1} << n);
                        }

                        // For the split-write shape, the reference compiler
                        // preloads the FALSE-branch varying into a half-
                        // precision register pair via MOVH, then uses it
                        // as the source for the conditional per-lane MOVs.
                        // For the simple shape, no preload is needed —
                        // the varying is read directly in the cond MOV.
                        struct nvfx_reg h2Dst = nvfx_reg(NVFXSR_TEMP, 2);
                        h2Dst.is_fp16 = 1;
                        if (splitWriteShape)
                        {
                            // ----- Inst 2: MOVH H2.xyzw, f[falseBr] -----
                            const struct nvfx_reg falseInReg =
                                nvfx_reg(NVFXSR_INPUT, falseInputSrc);
                            struct nvfx_src mvhS0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(falseInReg));
                            struct nvfx_src mvhS1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src mvhS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn mvh = nvfx_insn(
                                0, 0, -1, -1, h2Dst,
                                NVFX_FP_MASK_ALL, mvhS0, mvhS1, mvhS2);
                            mvh.precision = FLOAT16;
                            asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                            attrs.attributeInputMask |=
                                fpAttrMaskBitForInputSrc(falseInputSrc);
                            if (falseInputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                                falseInputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            {
                                const int n =
                                    falseInputSrc - NVFX_FP_OP_INPUT_SRC_TC(0);
                                attrs.texCoordsInputMask |= uint16_t{1} << n;
                                attrs.texCoords2D &= ~(uint16_t{1} << n);
                            }
                        }

                        // ----- SLE-CC R1.<lane>, c[0].xxxx -----
                        const struct nvfx_reg ccDst =
                            nvfx_reg(NVFXSR_NONE, 0x3F);
                        const struct nvfx_reg constReg =
                            nvfx_reg(NVFXSR_CONST, 0);
                        struct nvfx_src cmpS0 = nvfx_src(r1Dst);
                        if (splitWriteShape)
                        {
                            // Identity swizzle .xyzw — the reference's
                            // SLE for split-write writes mask=.<lhsLane>
                            // and reads src0 with identity .xyzw (only
                            // the masked lane is meaningful).
                            cmpS0.swz[0] = 0; cmpS0.swz[1] = 1;
                            cmpS0.swz[2] = 2; cmpS0.swz[3] = 3;
                        }
                        else
                        {
                            // Smear lhsLane across all components — the
                            // simple shape writes mask=.x, so src0's lane 0
                            // is consumed.
                            cmpS0.swz[0] = cmpS0.swz[1] =
                            cmpS0.swz[2] = cmpS0.swz[3] =
                                static_cast<uint8_t>(cb.lhsLane);
                        }
                        struct nvfx_src cmpS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        cmpS1.swz[0] = cmpS1.swz[1] =
                        cmpS1.swz[2] = cmpS1.swz[3] = 0;  // .xxxx
                        struct nvfx_src cmpS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        // The simple shape writes the CC at lane X (mask
                        // .x); the split-write shape writes at the
                        // lhsLane bit (mask .<lhsLane>).
                        const uint32_t cmpMask =
                            splitWriteShape
                                ? (uint32_t{1} << static_cast<uint32_t>(cb.lhsLane))
                                : NVFX_FP_MASK_X;
                        struct nvfx_insn cmpIn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(ccDst),
                            cmpMask, cmpS0, cmpS1, cmpS2);
                        cmpIn.cc_update = 1;  // COND_WRITE_ENABLE
                        asm_.emit(cmpIn, cb.opcode);
                        const float cmpInline[4] =
                            { cb.rhsLiteral, 0.0f, 0.0f, 0.0f };
                        asm_.appendConstBlock(cmpInline);

                        if (splitWriteShape)
                        {
                            // The reference compiler emits 3 MOV insns
                            // for the split-write shape, all reading the
                            // half-precision color preload at H2:
                            //   1. MOV(EQ.<lhs>) R0.<lhs>, H2          (alpha lane)
                            //   2. MOV(TR)        R0.<lhs>, R0          (no-op slot)
                            //   3. MOV(EQ.<lhs>) R0.<other>, H2 [END]  (RGB lanes)
                            // The alpha lane and RGB lanes split because
                            // the source had `oColor.rgb = ...; oColor.a
                            // = ...;` separately.  COND_EQ fires when CC
                            // == 0, i.e. when SLE wrote 0, i.e. when the
                            // LE was FALSE (the "else" arm — alpha > 0.5).
                            const uint32_t lhsMask =
                                uint32_t{1} << static_cast<uint32_t>(cb.lhsLane);
                            const uint32_t otherMask =
                                NVFX_FP_MASK_ALL & ~lhsMask;
                            const uint8_t lhsSwz =
                                static_cast<uint8_t>(cb.lhsLane);

                            // Inst 4: MOV(EQ.<lhs>) R0.<lhs>, H2.xyzw
                            {
                                struct nvfx_src s0 = nvfx_src(h2Dst);
                                struct nvfx_src s1 =
                                    nvfx_src(const_cast<struct nvfx_reg&>(none));
                                struct nvfx_src s2 =
                                    nvfx_src(const_cast<struct nvfx_reg&>(none));
                                struct nvfx_insn in = nvfx_insn(
                                    0, 0, -1, -1,
                                    const_cast<struct nvfx_reg&>(dstReg),
                                    lhsMask, s0, s1, s2);
                                in.cc_test  = 1;
                                in.cc_cond  = NVFX_FP_OP_COND_EQ;
                                in.cc_swz[0] = in.cc_swz[1] =
                                in.cc_swz[2] = in.cc_swz[3] = lhsSwz;
                                asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                            }
                            // Inst 5: MOV(TR) R0.<lhs>, R0.xyzw — no-op
                            // slot the reference compiler emits between
                            // the alpha-lane and RGB-lane writes.  Source
                            // is R0 (default identity swizzle), cc_test
                            // disabled (always executes), cc_swz default.
                            {
                                struct nvfx_reg r0Src = nvfx_reg(NVFXSR_TEMP, 0);
                                struct nvfx_src s0 = nvfx_src(r0Src);
                                struct nvfx_src s1 =
                                    nvfx_src(const_cast<struct nvfx_reg&>(none));
                                struct nvfx_src s2 =
                                    nvfx_src(const_cast<struct nvfx_reg&>(none));
                                struct nvfx_insn in = nvfx_insn(
                                    0, 0, -1, -1,
                                    const_cast<struct nvfx_reg&>(dstReg),
                                    lhsMask, s0, s1, s2);
                                asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                            }
                            // Inst 6: MOV(EQ.<lhs>)[END] R0.<other>, H2
                            {
                                struct nvfx_src s0 = nvfx_src(h2Dst);
                                struct nvfx_src s1 =
                                    nvfx_src(const_cast<struct nvfx_reg&>(none));
                                struct nvfx_src s2 =
                                    nvfx_src(const_cast<struct nvfx_reg&>(none));
                                struct nvfx_insn in = nvfx_insn(
                                    0, 0, -1, -1,
                                    const_cast<struct nvfx_reg&>(dstReg),
                                    otherMask, s0, s1, s2);
                                in.cc_test  = 1;
                                in.cc_cond  = NVFX_FP_OP_COND_EQ;
                                in.cc_swz[0] = in.cc_swz[1] =
                                in.cc_swz[2] = in.cc_swz[3] = lhsSwz;
                                asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                            }
                        }
                        else
                        {
                            // Simple shape: single MOV(EQ.xxxx) R0, f[falseBr].
                            const struct nvfx_reg movInReg =
                                nvfx_reg(NVFXSR_INPUT, falseInputSrc);
                            struct nvfx_src movS0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(movInReg));
                            struct nvfx_src movS1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src movS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn movIn = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_ALL, movS0, movS1, movS2);
                            movIn.cc_test  = 1;
                            movIn.cc_cond  = NVFX_FP_OP_COND_EQ;
                            movIn.cc_swz[0] = movIn.cc_swz[1] =
                            movIn.cc_swz[2] = movIn.cc_swz[3] = 0;
                            asm_.emit(movIn, NVFX_FP_OP_OPCODE_MOV);

                            attrs.attributeInputMask |=
                                fpAttrMaskBitForInputSrc(falseInputSrc);
                            if (falseInputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                                falseInputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            {
                                const int n =
                                    falseInputSrc - NVFX_FP_OP_INPUT_SRC_TC(0);
                                attrs.texCoordsInputMask |= uint16_t{1} << n;
                                attrs.texCoords2D &= ~(uint16_t{1} << n);
                            }
                        }

                        emittedSomething = true;
                        break;
                    }

                    // UniformScalar-LHS Select: `if (uniformFloat != 0.0)`.
                    // Older SDK Water sample's SimpleFragment.cg uses
                    // this shape with a uniform `float gFunctionSwitch;`
                    // gating an arithmetic update of a vec4 local.  The
                    // reference compiler doesn't emit IF/ELSE/ENDIF —
                    // it lowers to a CC-based blend:
                    //
                    //   TEX  R1, f[TC<m>], TEX<m>     ; sample
                    //   MOVH H4, f[<varying>]          ; preload default
                    //   MOV?C(<lane>) RC, c[uni].x     ; cmp + 16B zeros
                    //   MOVR R0, H4                    ; unconditional default
                    //   MULR(NE.<lane>) R0, H4, R1 END ; conditional THEN
                    //
                    // Constraints today:
                    //   - cmp opcode is SNE (`!= 0`); cmpne resolves
                    //     to NE cc_cond on the conditional MUL.
                    //   - trueBr is `Mul(varying, tex_sample)` — recorded
                    //     via valueToVaryingTexMul.
                    //   - falseBr is the same varying as the Mul's
                    //     varying operand (so the default move + cond
                    //     mul both source from H4).
                    //   - rhs of cmpne must be float literal 0.0.
                    if (cb.lhsKind == CmpBinding::LhsKind::UniformScalar)
                    {
                        if (cb.opcode != NVFX_FP_OP_OPCODE_SNE)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select uniform-LHS: only `!= 0.0` "
                                "(cmpne) wired today");
                            return out;
                        }
                        if (cb.rhsKind != CmpBinding::RhsKind::Literal ||
                            cb.rhsLiteral != 0.0f)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select uniform-LHS: RHS must be "
                                "the literal 0.0");
                            return out;
                        }

                        auto vmIt = valueToVaryingTexMul.find(sb.trueId);
                        if (vmIt == valueToVaryingTexMul.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select uniform-LHS: TRUE branch "
                                "must be Mul(varying, tex_sample)");
                            return out;
                        }
                        if (sb.falseId != vmIt->second.varyingId)
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select uniform-LHS: FALSE branch "
                                "must equal the Mul's varying operand");
                            return out;
                        }

                        auto vIn = valueToInputSrc.find(vmIt->second.varyingId);
                        auto txIt = valueToTex.find(vmIt->second.texId);
                        if (vIn == valueToInputSrc.end() ||
                            txIt == valueToTex.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select uniform-LHS: tex / varying "
                                "operand bindings did not resolve");
                            return out;
                        }
                        auto sampIt = valueToTexUnit.find(txIt->second.samplerId);
                        IRValueID uvBase = resolveSrcMods(txIt->second.uvId).baseId;
                        auto uvIn = valueToInputSrc.find(uvBase);
                        if (sampIt == valueToTexUnit.end() ||
                            uvIn == valueToInputSrc.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select uniform-LHS: tex sampler / UV "
                                "did not resolve");
                            return out;
                        }
                        auto cmpUniIt = valueToFpUniform.find(cb.lhsUniformId);
                        if (cmpUniIt == valueToFpUniform.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: Select uniform-LHS: cmp uniform slot "
                                "did not resolve");
                            return out;
                        }

                        // Narrow the conditional MUL's writemask to skip
                        // any lanes that VecInsert overrides.  When
                        // there's a lane override, we also pick the cmp
                        // CC lane to match — the reference compiler
                        // chooses the FIRST lane NOT in the conditional
                        // op's writemask (so the CC bit lives on a
                        // "free" lane that nothing else reads).  Falls
                        // back to lane X when every lane is active.
                        uint8_t condWriteMask = NVFX_FP_MASK_ALL;
                        for (const auto& lo : laneOverrides)
                            condWriteMask &= static_cast<uint8_t>(~(1u << lo.lane));
                        int cmpLane = 0;
                        for (int i = 0; i < 4; ++i)
                            if (!(condWriteMask & (1u << i))) { cmpLane = i; break; }
                        const uint8_t cmpMask = static_cast<uint8_t>(1u << cmpLane);

                        const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                        const struct nvfx_reg ccDst = nvfx_reg(NVFXSR_NONE, 0x3F);
                        const struct nvfx_reg r1Dst = nvfx_reg(NVFXSR_TEMP, 1);

                        const uint8_t texOpcode = txIt->second.projective
                            ? NVFX_FP_OP_OPCODE_TXP
                            : NVFX_FP_OP_OPCODE_TEX;
                        const struct nvfx_reg uvReg =
                            nvfx_reg(NVFXSR_INPUT, uvIn->second);
                        const struct nvfx_reg varReg =
                            nvfx_reg(NVFXSR_INPUT, vIn->second);

                        auto emitCmp = [&]()
                        {
                            // MOV?C(<cmpMask>, FX12) RC, c[0].xxxx + 16-byte
                            // zero block runtime-patched with the uniform.
                            struct nvfx_src s0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                            s0.swz[0] = s0.swz[1] = s0.swz[2] = s0.swz[3] = 0;
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(ccDst),
                                cmpMask, s0, s1, s2);
                            in.cc_update = 1;
                            in.precision = FIXED12;
                            asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);

                            const uint32_t off = asm_.currentByteSize();
                            for (auto& eu : attrs.embeddedUniforms)
                                if (eu.entryParamIndex == cmpUniIt->second)
                                { eu.ucodeByteOffsets.push_back(off); break; }
                            static const float zerosBlock[4] = { 0, 0, 0, 0 };
                            asm_.appendConstBlock(zerosBlock);
                        };

                        auto recordVaryingAttr = [&](int inputSrc, bool moreThan2Lanes)
                        {
                            attrs.attributeInputMask |=
                                fpAttrMaskBitForInputSrc(inputSrc);
                            if (inputSrc >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                                inputSrc <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            {
                                const int n = inputSrc - NVFX_FP_OP_INPUT_SRC_TC(0);
                                attrs.texCoordsInputMask |= uint16_t{1} << n;
                                // The reference compiler clears the
                                // texCoords2D bit when a TEXCOORD<N>
                                // varying is read with more than 2
                                // lanes — same mechanism as projective
                                // (TXP) / samplerCUBE consumption,
                                // signalling that the input isn't a
                                // pure 2D u/v.
                                if (moreThan2Lanes)
                                    attrs.texCoords2D &= ~(uint16_t{1} << n);
                            }
                        };

                        if (laneOverrides.empty())
                        {
                            // Bare uniform-cond shape (5 insts).  No
                            // lane override means R0 is free for the
                            // default + cond mul; fp16 H-register
                            // backup keeps the varying around at
                            // half precision.
                            //
                            // ── Inst 1: TEX R1, f[<uv>], TEX<unit> ──
                            struct nvfx_src texS0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(uvReg));
                            struct nvfx_src texS1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src texS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn texIn = nvfx_insn(
                                0, 0, sampIt->second, -1,
                                const_cast<struct nvfx_reg&>(r1Dst),
                                NVFX_FP_MASK_ALL,
                                texS0, texS1, texS2);
                            if (txIt->second.cube) texIn.disable_pc = 1;
                            asm_.emit(texIn, texOpcode);
                            emittedSomething = true;
                            recordVaryingAttr(uvIn->second, false);
                            if (txIt->second.projective || txIt->second.cube)
                                attrs.texCoords2D &= ~(uint16_t{1} <<
                                    (uvIn->second - NVFX_FP_OP_INPUT_SRC_TC(0)));

                            // ── Inst 2: MOVH H4, f[<varying>] ──
                            struct nvfx_reg h4Dst = nvfx_reg(NVFXSR_TEMP, 4);
                            h4Dst.is_fp16 = 1;
                            struct nvfx_src mvhS0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(varReg));
                            struct nvfx_src mvhS1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src mvhS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn mvh = nvfx_insn(
                                0, 0, -1, -1,
                                h4Dst, NVFX_FP_MASK_ALL,
                                mvhS0, mvhS1, mvhS2);
                            mvh.precision = FLOAT16;
                            asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);
                            recordVaryingAttr(vIn->second, true);

                            // ── Inst 3: cmp + 16B zero ──
                            emitCmp();

                            // ── Inst 4: MOVR R0, H4 ──
                            struct nvfx_src defS0 = nvfx_src(h4Dst);
                            struct nvfx_src defS1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src defS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn defMov = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_ALL, defS0, defS1, defS2);
                            defMov.precision = dstPrecision;
                            asm_.emit(defMov, NVFX_FP_OP_OPCODE_MOV);

                            // ── Inst 5: MULR(NE.<lane>) R0, H4, R1 ──
                            struct nvfx_src mulS0 = nvfx_src(h4Dst);
                            struct nvfx_src mulS1 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r1Dst));
                            struct nvfx_src mulS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn mulIn = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_ALL, mulS0, mulS1, mulS2);
                            mulIn.precision = dstPrecision;
                            mulIn.cc_test  = 1;
                            mulIn.cc_cond  = NVFX_COND_NE;
                            mulIn.cc_swz[0] = mulIn.cc_swz[1] =
                            mulIn.cc_swz[2] = mulIn.cc_swz[3] =
                                static_cast<uint8_t>(cmpLane);
                            asm_.emit(mulIn, NVFX_FP_OP_OPCODE_MUL);
                            break;
                        }

                        // Lane-override shape (6 insts) — `c.a = 1.0;`
                        // after the if-block forces a different shape:
                        // the conditional MUL must NOT touch the
                        // override lane (W), so its writemask narrows
                        // to ~lane.  The varying gets backed up in an
                        // R-register (no fp16 promotion) so the cond
                        // MUL can read it after R0's W lane has been
                        // overwritten.  Cmp lane = the override lane
                        // (W) so the CC bit lives on a free lane.
                        //
                        //   MOV?C(<cmpMask>) RC, c[0]      ; cmp first
                        //   MOVR R2.<active>, f[<varying>] ; backup
                        //   MOVR R0.<active>, R2.<active>  ; default
                        //   MOVR R0.<override>, c[0]       ; lane override
                        //   TEX  R1.<active>, f[<uv>]      ; sample
                        //   MULR(NE.<lane>) R0, R2, R1 END ; cond mul
                        const struct nvfx_reg r2Dst = nvfx_reg(NVFXSR_TEMP, 2);

                        // ── Inst 1: cmp first ──
                        emitCmp();
                        emittedSomething = true;

                        // ── Inst 2: MOVR R2.<active>, f[<varying>] ──
                        {
                            struct nvfx_src s0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(varReg));
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(r2Dst),
                                condWriteMask, s0, s1, s2);
                            asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                            recordVaryingAttr(vIn->second, true);
                        }

                        // ── Inst 3: MOVR R0.<active>, R2 ──
                        {
                            struct nvfx_src s0 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r2Dst));
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                condWriteMask, s0, s1, s2);
                            in.precision = dstPrecision;
                            asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                        }

                        // ── Inst 4: lane-override MOV(s) ──
                        if (!emitLaneOverrides()) return out;

                        // ── Inst 5: TEX R1.<active>, f[<uv>] ──
                        {
                            struct nvfx_src s0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(uvReg));
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, sampIt->second, -1,
                                const_cast<struct nvfx_reg&>(r1Dst),
                                condWriteMask, s0, s1, s2);
                            if (txIt->second.cube) in.disable_pc = 1;
                            asm_.emit(in, texOpcode);
                            recordVaryingAttr(uvIn->second, false);
                            if (txIt->second.projective || txIt->second.cube)
                                attrs.texCoords2D &= ~(uint16_t{1} <<
                                    (uvIn->second - NVFX_FP_OP_INPUT_SRC_TC(0)));
                        }

                        // ── Inst 6: MULR(NE.<lane>) R0.<active>, R2, R1 END ──
                        {
                            struct nvfx_src s0 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r2Dst));
                            struct nvfx_src s1 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r1Dst));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                condWriteMask, s0, s1, s2);
                            in.precision = dstPrecision;
                            in.cc_test  = 1;
                            in.cc_cond  = NVFX_COND_NE;
                            in.cc_swz[0] = in.cc_swz[1] =
                            in.cc_swz[2] = in.cc_swz[3] =
                                static_cast<uint8_t>(cmpLane);
                            asm_.emit(in, NVFX_FP_OP_OPCODE_MUL);
                        }
                        break;
                    }

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
                    // (preload) MOV vs the conditional MOV.  Three
                    // ordering constraints, in priority order:
                    //   1. Varyings can't be preloaded (they route via
                    //      SRC0=INPUT, no inline block).  If the true
                    //      branch is a varying and the false isn't,
                    //      swap so the varying rides the conditional
                    //      slot and flip EQ → NE to match semantics.
                    //   2. If the Select was synthesized from an
                    //      if-else statement (componentIndex=1 set by
                    //      nv40_if_convert), match sce-cgc's if-else
                    //      schedule: preload false, conditional NE-
                    //      write true.  Ternary expressions keep the
                    //      "preload true" default.
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
                    else if (sb.ifElseSchedule)
                    {
                        preloadBr     = falseBr;
                        conditionalBr = trueBr;
                        condCode      = NVFX_FP_OP_COND_NE;
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

                // dot(literal_vec, varying-pack) — wrapped in
                // float4(d, d, d, 1.0).  3-instruction shape:
                //   MOV R0.<pack-mask>, f[INPUT].<pack-swizzle>
                //   MOV R0.w, c[0].xxxx       <inline {1, 0, 0, 0}>
                //   DPN R0.xyz [END], R0.xyzw, c[0].<lit-swizzle>
                //                              <inline literal block>
                // Where DPN is DP2/DP3/DP4 driven by pack width.
                // Source swizzle on the DP's c[0] reads the literal
                // lanes back in pack order — for width-2 that's .xyxx
                // (X→0.97, Y→0.242, Z/W replicate lane 0).
                auto dlpIt = valueToDotLitPack.find(srcId);
                if (dlpIt != valueToDotLitPack.end() && dlpIt->second.wrapW1)
                {
                    const FpDotLitPackBinding& bind = dlpIt->second;
                    auto inputIt = valueToInputSrc.find(bind.pack.baseId);
                    if (inputIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: dot-lit-pack varying did not resolve");
                        return out;
                    }
                    const int inputSrcCode = inputIt->second;
                    const int packWidth    = bind.pack.width;

                    const struct nvfx_reg tempR0 = nvfx_reg(NVFXSR_TEMP, 0);
                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);

                    // --- Insn 1: MOV R0.<mask>, f[INPUT].<pack-swz> ---
                    uint8_t packMask = 0;
                    for (int k = 0; k < packWidth; ++k) packMask |= (1u << k);

                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inputSrcCode);
                    struct nvfx_src packSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    // Pack lanes fill the matching slots; trailing
                    // slots stay identity (X=0,Y=1,Z=2,W=3).  Matches
                    // the reference compiler's `.xzzw` shape for a
                    // width-2 pack of lanes {0, 2}: SWZ_X=0(X),
                    // SWZ_Y=2(Z), SWZ_Z stays 2(Z) (default), SWZ_W
                    // stays 3(W) (default).  The dst mask hides
                    // whatever the trailing slots resolve to.
                    for (int k = 0; k < packWidth; ++k)
                        packSrc.swz[k] = static_cast<uint8_t>(bind.pack.lanes[k]);

                    struct nvfx_src none0 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn movPack = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        packMask, packSrc, none0, none0);
                    movPack.precision = FLOAT32;
                    asm_.emit(movPack, NVFX_FP_OP_OPCODE_MOV);

                    // --- Insn 2: MOV R0.w, c[0].xxxx + {1, 0, 0, 0} ---
                    struct nvfx_src cOneSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    cOneSrc.swz[0] = cOneSrc.swz[1] =
                    cOneSrc.swz[2] = cOneSrc.swz[3] = 0;
                    struct nvfx_insn movW = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        NVFX_FP_MASK_W, cOneSrc, none0, none0);
                    movW.precision = FLOAT32;
                    asm_.emit(movW, NVFX_FP_OP_OPCODE_MOV);
                    const float wOneBlock[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
                    asm_.appendConstBlock(wOneBlock);

                    // --- Insn 3: DPn R0.xyz [END], R0.xyzw,
                    //                              c[0].<lit-swz> ---
                    struct nvfx_src tempSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(tempR0));
                    // identity .xyzw on the pack temp (default)

                    struct nvfx_src litSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    // Literal swizzle: lane k of the dot's literal
                    // operand is at lane k of the inline block; the
                    // dot reads each pack-active lane in order.  For
                    // width=2 → .xyxx (X→0.97, Y→0.242, Z/W=X).
                    for (int k = 0; k < 4; ++k)
                    {
                        litSrc.swz[k] = (k < packWidth)
                            ? static_cast<uint8_t>(k) : 0;
                    }

                    uint8_t dotOpcode = NVFX_FP_OP_OPCODE_DP2;
                    if      (packWidth == 3) dotOpcode = NVFX_FP_OP_OPCODE_DP3;
                    else if (packWidth >= 4) dotOpcode = NVFX_FP_OP_OPCODE_DP4;

                    // dst mask = .xyz so the W lane stays at the 1.0
                    // we wrote in insn 2.
                    struct nvfx_insn dotInsn = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z,
                        tempSrc, litSrc, none0);
                    dotInsn.precision = dstPrecision;
                    asm_.emit(dotInsn, dotOpcode);

                    // Inline literal block — pack pads to 4 floats.
                    const float litBlock[4] = {
                        bind.literal[0], bind.literal[1],
                        bind.literal[2], bind.literal[3] };
                    asm_.appendConstBlock(litBlock);

                    // Track the varying input bits.  When the pack
                    // reads lane Z or W of a TEXCOORD<N>, clear the
                    // texCoords2D bit — the coord is being used as
                    // 3D/4D, mirroring the cube/projective handling
                    // elsewhere.
                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(inputSrcCode);
                    if (inputSrcCode >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inputSrcCode <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        const int n = inputSrcCode - NVFX_FP_OP_INPUT_SRC_TC(0);
                        attrs.texCoordsInputMask |= uint16_t{1} << n;
                        for (int k = 0; k < packWidth; ++k)
                        {
                            if (bind.pack.lanes[k] >= 2)
                            {
                                attrs.texCoords2D &= ~(uint16_t{1} << n);
                                break;
                            }
                        }
                    }

                    emittedSomething = true;
                    break;
                }

                // 3 planar dots wrapped float4(d1, d2, d3, 1.0) —
                // shares one varying-pack across all three.  See the
                // FpDot3LitPackWrapBinding header for the 5-insn
                // shape.  Even output lane parity → block at offset
                // (0,1) with .xyxx swizzle.  Odd → offset (2,3) with
                // .zwzz.
                auto d3It = valueToDot3LitPackWrap.find(srcId);
                if (d3It != valueToDot3LitPackWrap.end())
                {
                    const FpDot3LitPackWrapBinding& bind = d3It->second;
                    auto inputIt = valueToInputSrc.find(bind.pack.baseId);
                    if (inputIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: dot3-lit-pack varying did not resolve");
                        return out;
                    }
                    const int inputSrcCode = inputIt->second;
                    const int packWidth    = bind.pack.width;

                    const struct nvfx_reg tempR0   = nvfx_reg(NVFXSR_TEMP, 0);
                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inputSrcCode);
                    struct nvfx_src none0 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));

                    // --- Insn 1: MOV R0.zw, f[INPUT].<cyclic> ---
                    // Cyclic pack: each swizzle slot reads the pack
                    // lane at (slot mod packWidth).  For width=2 with
                    // pack=[0,2] this gives .xzxz.
                    struct nvfx_src packSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    for (int k = 0; k < 4; ++k)
                    {
                        const int laneIdx = bind.pack.lanes[k % packWidth];
                        packSrc.swz[k] = static_cast<uint8_t>(laneIdx);
                    }
                    struct nvfx_insn movPack = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        NVFX_FP_MASK_Z | NVFX_FP_MASK_W,
                        packSrc, none0, none0);
                    movPack.precision = FLOAT32;
                    asm_.emit(movPack, NVFX_FP_OP_OPCODE_MOV);

                    // --- Insns 2-4: 3 DP2/3/4s into R0.x, R0.y, R0.z ---
                    uint8_t dotOpcode = NVFX_FP_OP_OPCODE_DP2;
                    if      (packWidth == 3) dotOpcode = NVFX_FP_OP_OPCODE_DP3;
                    else if (packWidth >= 4) dotOpcode = NVFX_FP_OP_OPCODE_DP4;

                    // src0 reads the pack temp R0 with .zwzz — slots
                    // 0/1 hold the pack data (Z/W of R0); slots 2/3
                    // replicate the pack-lane-0 position (Z) to match
                    // the reference compiler's filler.
                    auto dotSrc0 = [&]() {
                        struct nvfx_src s =
                            nvfx_src(const_cast<struct nvfx_reg&>(tempR0));
                        s.swz[0] = 2;  // Z
                        s.swz[1] = 3;  // W
                        s.swz[2] = 2;  // Z
                        s.swz[3] = 2;  // Z
                        return s;
                    };

                    auto emitDotLane = [&](int outLane, const float* lit)
                    {
                        const bool offsetEven = ((outLane & 1) == 0);
                        struct nvfx_src tempSrc = dotSrc0();
                        struct nvfx_src litSrc  =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        if (offsetEven)
                        {
                            litSrc.swz[0] = 0;  // X
                            litSrc.swz[1] = 1;  // Y
                            litSrc.swz[2] = 0;  // X
                            litSrc.swz[3] = 0;  // X
                        }
                        else
                        {
                            litSrc.swz[0] = 2;  // Z
                            litSrc.swz[1] = 3;  // W
                            litSrc.swz[2] = 2;  // Z
                            litSrc.swz[3] = 2;  // Z
                        }
                        const uint8_t mask =
                            static_cast<uint8_t>(1u << outLane);
                        struct nvfx_insn dotInsn = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            mask, tempSrc, litSrc, none0);
                        dotInsn.precision = dstPrecision;
                        asm_.emit(dotInsn, dotOpcode);

                        float block[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                        if (offsetEven)
                        {
                            block[0] = lit[0];
                            block[1] = lit[1];
                        }
                        else
                        {
                            block[2] = lit[0];
                            block[3] = lit[1];
                        }
                        asm_.appendConstBlock(block);
                    };

                    emitDotLane(0, bind.literals[0]);
                    emitDotLane(1, bind.literals[1]);
                    emitDotLane(2, bind.literals[2]);

                    // --- Optional Insn 5: MUL R0.xyz, R0, c[0].xyzw +
                    //                       {scale.x, scale.y, scale.z, 0}
                    // Folds the `freq * Length` step from the kitchen-
                    // sink water FP shader into a single MUL with an
                    // inline literal block.
                    if (bind.hasScale)
                    {
                        struct nvfx_src tempIdent =
                            nvfx_src(const_cast<struct nvfx_reg&>(tempR0));
                        // Identity .xyzw on R0.
                        struct nvfx_src scaleConst =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        // Identity .xyzw on c[0] reading lanes 0..3.

                        struct nvfx_insn mulInsn = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z,
                            tempIdent, scaleConst, none0);
                        mulInsn.precision = dstPrecision;
                        asm_.emit(mulInsn, NVFX_FP_OP_OPCODE_MUL);
                        const float scaleBlock[4] = {
                            bind.scale[0], bind.scale[1],
                            bind.scale[2], 0.0f };
                        asm_.appendConstBlock(scaleBlock);
                    }

                    // --- Final MOV R0.w [END], c[0].<W> + wOne block ---
                    // Bare wrap: c[0].xxxx with {1, 0, 0, 0}.
                    // Scaled wrap: c[0].xyzw (reads .w=1.0) with
                    //              {0, 0, 0, 1.0}.  Reference layout
                    //              choice tracks the scale insn's
                    //              identity-swizzle convention.
                    struct nvfx_src cOneSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    if (bind.hasScale)
                    {
                        // Identity .xyzw — reads block lane 3 (W) for
                        // the masked .w write.  Note that swz[3] = 3
                        // is the default initialised value so we just
                        // leave the swizzle alone.
                    }
                    else
                    {
                        cOneSrc.swz[0] = cOneSrc.swz[1] =
                        cOneSrc.swz[2] = cOneSrc.swz[3] = 0;
                    }
                    struct nvfx_insn movW = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_W, cOneSrc, none0, none0);
                    movW.precision = dstPrecision;
                    asm_.emit(movW, NVFX_FP_OP_OPCODE_MOV);
                    const float wOneBlockBare[4]   = { 1.0f, 0.0f, 0.0f, 0.0f };
                    const float wOneBlockScaled[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    asm_.appendConstBlock(bind.hasScale ? wOneBlockScaled
                                                        : wOneBlockBare);

                    // Track varying-input bits + clear texCoords2D.
                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(inputSrcCode);
                    if (inputSrcCode >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inputSrcCode <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        const int n = inputSrcCode - NVFX_FP_OP_INPUT_SRC_TC(0);
                        attrs.texCoordsInputMask |= uint16_t{1} << n;
                        for (int k = 0; k < packWidth; ++k)
                        {
                            if (bind.pack.lanes[k] >= 2)
                            {
                                attrs.texCoords2D &= ~(uint16_t{1} << n);
                                break;
                            }
                        }
                    }

                    emittedSomething = true;
                    break;
                }

                // `vec4(scaled-dots-minus-lit-mul-uniform, 1.0)` —
                // 9-instruction shape combining a 3-dot pack, a
                // literal vec3 scale on the dots, a literal vec3
                // multiplied by a uniform scalar, and the SUB folded
                // into a final MAD with NEGATE.  Routes the dots to
                // R1 so R0 can carry the uniform preload + W=1.0 +
                // final MAD chain without conflict.
                auto sdmIt = valueToScaledDotsMinusLitMulUniWrap.find(srcId);
                if (sdmIt != valueToScaledDotsMinusLitMulUniWrap.end())
                {
                    const FpScaledDotsMinusLitMulUniformBinding& sb =
                        sdmIt->second.sub;
                    const FpDot3LitPackBinding& base = sb.scaledDots.base;
                    auto packInputIt = valueToInputSrc.find(base.pack.baseId);
                    if (packInputIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scaled-dots-sub: pack varying did not resolve");
                        return out;
                    }
                    auto uniIt = valueToFpUniform.find(sb.litTimesUni.uniformId);
                    if (uniIt == valueToFpUniform.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scaled-dots-sub: uniform did not resolve");
                        return out;
                    }
                    const int inputSrcCode = packInputIt->second;
                    const int packWidth    = base.pack.width;

                    const struct nvfx_reg tempR0   = nvfx_reg(NVFXSR_TEMP, 0);
                    const struct nvfx_reg tempR1   = nvfx_reg(NVFXSR_TEMP, 1);
                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inputSrcCode);
                    struct nvfx_src none0 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));

                    // --- Insn 1: MOV R0.<pack-mask>, f[INPUT].<pack-swz> ---
                    // Single-dot-style pack into R0.xy.  Trailing
                    // src swizzle slots stay identity.
                    uint8_t packMask = 0;
                    for (int k = 0; k < packWidth; ++k) packMask |= (1u << k);
                    struct nvfx_src packSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    for (int k = 0; k < packWidth; ++k)
                        packSrc.swz[k] = static_cast<uint8_t>(base.pack.lanes[k]);
                    struct nvfx_insn movPack = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        packMask, packSrc, none0, none0);
                    movPack.precision = FLOAT32;
                    asm_.emit(movPack, NVFX_FP_OP_OPCODE_MOV);

                    // --- Insn 2: MOV R0.w, c[0].xxxx + {1, 0, 0, 0} ---
                    // W=1.0 at lane 0 — rule 4 (default) since prev
                    // is a varying-only MOV (no c[0] swizzle to match
                    // or avoid) and the next non-FENCBR insn is the
                    // first DP2 (c[0].xyxx, neither .xxxx nor .xyzw).
                    struct nvfx_src cXSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    cXSrc.swz[0] = cXSrc.swz[1] =
                    cXSrc.swz[2] = cXSrc.swz[3] = 0;
                    struct nvfx_insn movW = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        NVFX_FP_MASK_W, cXSrc, none0, none0);
                    movW.precision = FLOAT32;
                    asm_.emit(movW, NVFX_FP_OP_OPCODE_MOV);
                    const float wOneBlock[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
                    asm_.appendConstBlock(wOneBlock);

                    // --- Insns 3-5: 3 DP2s into R1.x, R1.y, R1.z ---
                    // Per-DP literal block layout for THIS shape:
                    //   dirA → R1.x, swizzle .xyxx, block (lit.x, lit.y, 0, 0)
                    //   dirB → R1.y, swizzle .zwzz, block (0, 0, lit.x, lit.y)
                    //   dirC → R1.z, swizzle .yzzw, block (0, lit.x, lit.y, 0)
                    //                                ^^^^ shifted by 1 lane
                    //                                vs. the simpler 3-dot
                    //                                wrap; reverse-engineered
                    //                                from the reference output
                    //                                for this exact shape.
                    auto packTempSrc = [&]() {
                        struct nvfx_src s =
                            nvfx_src(const_cast<struct nvfx_reg&>(tempR0));
                        // Identity .xyzw — DP2 reads the pack via
                        // R0.x and R0.y.
                        return s;
                    };
                    auto emitDot = [&](int outLane, const float* lit,
                                       int swzStart) {
                        struct nvfx_src tempSrc = packTempSrc();
                        struct nvfx_src litSrc =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        // swzStart = block lane where lit.x lives.
                        // Slot 0 reads lit.x, slot 1 reads lit.y; slots
                        // 2/3 trail.
                        litSrc.swz[0] = static_cast<uint8_t>(swzStart);
                        litSrc.swz[1] = static_cast<uint8_t>(swzStart + 1);
                        // Slot 2: depends on shape — for the .xyxx and
                        // .yzzw cases sce-cgc replicates lane 0 of the
                        // swizzle (slot 0); for .zwzz it replicates Z.
                        // The pattern: trailing slots replicate lane
                        // (swzStart + (slot - 2) mod 2).  For starts 0
                        // and 1 we replicate slot 0 / slot 1; for start
                        // 2 we replicate slot 0 (Z).
                        if (swzStart == 2)
                        {
                            // .zwzz pattern
                            litSrc.swz[2] = 2;  // Z
                            litSrc.swz[3] = 2;  // Z
                        }
                        else
                        {
                            // .xyxx (start=0) or .yzzw (start=1) — slot 2
                            // replicates start; slot 3 replicates
                            // (start + 2) for the .yzzw case (which is
                            // identity-W) or start for .xyxx.
                            litSrc.swz[2] = static_cast<uint8_t>(
                                (swzStart == 0) ? 0 : (swzStart + 1));
                            litSrc.swz[3] = static_cast<uint8_t>(
                                (swzStart == 0) ? 0 : (swzStart + 2));
                        }

                        const uint8_t mask =
                            static_cast<uint8_t>(1u << outLane);
                        struct nvfx_insn dotInsn = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(tempR1),
                            mask, tempSrc, litSrc, none0);
                        dotInsn.precision = FLOAT32;
                        asm_.emit(dotInsn, NVFX_FP_OP_OPCODE_DP2);

                        float block[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                        block[swzStart]     = lit[0];
                        block[swzStart + 1] = lit[1];
                        asm_.appendConstBlock(block);
                    };
                    // Hardcoded per-output-lane swizzle starts for THIS
                    // 3-dot composition.
                    emitDot(0, base.literals[0], 0);  // dirA → .xyxx, lanes 0,1
                    emitDot(1, base.literals[1], 2);  // dirB → .zwzz, lanes 2,3
                    emitDot(2, base.literals[2], 1);  // dirC → .yzzw, lanes 1,2

                    // --- Insn 6: MOV R0.x, c[0].xxxx + zeros (uniform) ---
                    struct nvfx_insn movUni = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        NVFX_FP_MASK_X, cXSrc, none0, none0);
                    movUni.precision = FLOAT32;
                    asm_.emit(movUni, NVFX_FP_OP_OPCODE_MOV);
                    const uint32_t uniOffset = asm_.currentByteSize();
                    for (auto& eu : attrs.embeddedUniforms)
                    {
                        if (eu.entryParamIndex == uniIt->second)
                        {
                            eu.ucodeByteOffsets.push_back(uniOffset);
                            break;
                        }
                    }
                    static const float zerosBlock[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    asm_.appendConstBlock(zerosBlock);

                    // --- Insn 7: MUL R1.xyz, R1.xyzw, c[0].xyzw + freq ---
                    struct nvfx_src r1IdSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(tempR1));
                    // Identity swizzle (default).
                    struct nvfx_src freqSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    // Identity (default) reads c[0].xyzw.
                    struct nvfx_insn mulFreq = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR1),
                        NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z,
                        r1IdSrc, freqSrc, none0);
                    mulFreq.precision = FLOAT32;
                    asm_.emit(mulFreq, NVFX_FP_OP_OPCODE_MUL);
                    const float freqBlock[4] = {
                        sb.scaledDots.scale[0],
                        sb.scaledDots.scale[1],
                        sb.scaledDots.scale[2], 0.0f };
                    asm_.appendConstBlock(freqBlock);

                    // --- Insn 8: FENCBR ---
                    asm_.emitFencbr();

                    // --- Insn 9: MAD R0.xyz [END],
                    //                       -R0.xxxx, c[0].xyzw + phase, R1 ---
                    // Folds the Sub: -gTime * phase + (freq*Length)
                    //              = freq*Length - phase*gTime.
                    struct nvfx_src negR0X =
                        nvfx_src(const_cast<struct nvfx_reg&>(tempR0));
                    negR0X.swz[0] = negR0X.swz[1] =
                    negR0X.swz[2] = negR0X.swz[3] = 0;
                    negR0X.negate = 1;

                    struct nvfx_src phaseSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    // Identity .xyzw (default).
                    struct nvfx_src r1Src2 =
                        nvfx_src(const_cast<struct nvfx_reg&>(tempR1));
                    // Identity .xyzw on R1.

                    struct nvfx_insn madFinal = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z,
                        negR0X, phaseSrc, r1Src2);
                    madFinal.precision = dstPrecision;
                    asm_.emit(madFinal, NVFX_FP_OP_OPCODE_MAD);
                    const float phaseBlock[4] = {
                        sb.litTimesUni.literal[0],
                        sb.litTimesUni.literal[1],
                        sb.litTimesUni.literal[2], 0.0f };
                    asm_.appendConstBlock(phaseBlock);

                    // Track varying-input bits; clear texCoords2D
                    // when any pack lane is Z or W.
                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(inputSrcCode);
                    if (inputSrcCode >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inputSrcCode <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        const int n = inputSrcCode - NVFX_FP_OP_INPUT_SRC_TC(0);
                        attrs.texCoordsInputMask |= uint16_t{1} << n;
                        for (int k = 0; k < packWidth; ++k)
                        {
                            if (base.pack.lanes[k] >= 2)
                            {
                                attrs.texCoords2D &= ~(uint16_t{1} << n);
                                break;
                            }
                        }
                    }

                    emittedSomething = true;
                    break;
                }

                // `vec4(literal_vec * uniform_scalar, 1.0)` — 3-insn
                // shape with the W=1.0 placement chosen by the
                // function-wide literal-pool rule (prev c[0].xxxx
                // → avoid → lane 1 / .yyyy / {0, 1.0, 0, 0}).  The
                // uniform preload's swizzle (.xxxx) drives the rule.
                auto luwIt = valueToLitVecUniformScaleWrap.find(srcId);
                if (luwIt != valueToLitVecUniformScaleWrap.end())
                {
                    const FpLitVecUniformScaleWrapBinding& bind =
                        luwIt->second;
                    auto uIt = valueToFpUniform.find(bind.uniformId);
                    if (uIt == valueToFpUniform.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: lit-vec×uniform: uniform did not resolve");
                        return out;
                    }

                    const struct nvfx_reg tempR0   = nvfx_reg(NVFXSR_TEMP, 0);
                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    struct nvfx_src none0 =
                        nvfx_src(const_cast<struct nvfx_reg&>(none));

                    // --- Insn 1: MOV R0.x, c[0].xxxx + zeros ---
                    // Uniform preload at lane 0 of an inline block;
                    // the runtime patches the actual uniform value in.
                    struct nvfx_src ufSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    ufSrc.swz[0] = ufSrc.swz[1] =
                    ufSrc.swz[2] = ufSrc.swz[3] = 0;
                    struct nvfx_insn movUni = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        NVFX_FP_MASK_X, ufSrc, none0, none0);
                    movUni.precision = FLOAT32;
                    asm_.emit(movUni, NVFX_FP_OP_OPCODE_MOV);

                    const uint32_t uniOffset = asm_.currentByteSize();
                    for (auto& eu : attrs.embeddedUniforms)
                    {
                        if (eu.entryParamIndex == uIt->second)
                        {
                            eu.ucodeByteOffsets.push_back(uniOffset);
                            break;
                        }
                    }
                    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    asm_.appendConstBlock(zeros);

                    // --- Insn 2: MOV R0.w, c[0].yyyy + {0, 1, 0, 0} ---
                    // W=1.0 at lane 1 — the rule fires because the
                    // PREVIOUS insn (uniform preload above) reads
                    // c[0].xxxx, so we use .yyyy to dodge the .xxxx
                    // collision the reference compiler avoids.
                    struct nvfx_src cYSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    cYSrc.swz[0] = cYSrc.swz[1] =
                    cYSrc.swz[2] = cYSrc.swz[3] = 1;
                    struct nvfx_insn movW = nvfx_insn(
                        0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(tempR0),
                        NVFX_FP_MASK_W, cYSrc, none0, none0);
                    movW.precision = FLOAT32;
                    asm_.emit(movW, NVFX_FP_OP_OPCODE_MOV);
                    const float wOneBlock[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
                    asm_.appendConstBlock(wOneBlock);

                    // --- Insn 3: MUL R0.xyz [END], R0.xxxx,
                    //                                c[0].xyzw + lit ---
                    struct nvfx_src tempBSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(tempR0));
                    tempBSrc.swz[0] = tempBSrc.swz[1] =
                    tempBSrc.swz[2] = tempBSrc.swz[3] = 0;
                    struct nvfx_src litSrc =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    // Identity .xyzw on c[0] (default).

                    struct nvfx_insn mulInsn = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z,
                        tempBSrc, litSrc, none0);
                    mulInsn.precision = dstPrecision;
                    asm_.emit(mulInsn, NVFX_FP_OP_OPCODE_MUL);

                    const float litBlock[4] = {
                        bind.literal[0], bind.literal[1],
                        bind.literal[2], 0.0f };
                    asm_.appendConstBlock(litBlock);

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

                    // COS / SIN read the varying directly — single
                    // instruction with src swizzle = (lane, lane,
                    // lane, lane) so the scalar op picks the right
                    // input component.  No MOVH preload.  Probed
                    // against `color = cos(vcol.x);` (1-insn shape).
                    if (bind.kind == FpScalarUnaryBinding::Kind::Cos ||
                        bind.kind == FpScalarUnaryBinding::Kind::Sin)
                    {
                        const struct nvfx_reg inputReg =
                            nvfx_reg(NVFXSR_INPUT, inIt->second);
                        struct nvfx_src s0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                        if (bind.lane != 0)
                        {
                            // Splay the single input lane across all
                            // four src slots — only swz[0] matters
                            // for the scalar function, but matching
                            // the reference compiler's full splay
                            // keeps the encoding byte-identical.
                            s0.swz[0] = s0.swz[1] =
                            s0.swz[2] = s0.swz[3] =
                                static_cast<uint8_t>(bind.lane);
                        }
                        struct nvfx_src s1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src s2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn in = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_ALL, s0, s1, s2);
                        in.precision = dstPrecision;
                        const uint8_t opcode =
                            (bind.kind == FpScalarUnaryBinding::Kind::Cos)
                                ? NVFX_FP_OP_OPCODE_COS
                                : NVFX_FP_OP_OPCODE_SIN;
                        asm_.emit(in, opcode);

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

                    // Arith-input shape (`normalize(varying ± uniform)`):
                    //   ADDR R0.xyz, f[varying], ±c[uniform] + 16B zeros
                    //   DP3R R0.w,   R0,           R0
                    //   DIVRSQR R0.xyz, R0, R0.wwww [END]
                    if (bind.hasArith)
                    {
                        auto inIt = valueToInputSrc.find(bind.arithInputId);
                        if (inIt == valueToInputSrc.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: normalize(arith): varying op failed to resolve");
                            return out;
                        }
                        auto uIt = valueToFpUniform.find(bind.arithUniformId);
                        if (uIt == valueToFpUniform.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: normalize(arith): uniform op failed to resolve");
                            return out;
                        }

                        const struct nvfx_reg r0a = nvfx_reg(NVFXSR_TEMP, 0);
                        const struct nvfx_reg inReg =
                            nvfx_reg(NVFXSR_INPUT, inIt->second);
                        const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                        const uint8_t arithMask =
                            (bind.lanes >= 4) ? NVFX_FP_MASK_ALL :
                            (bind.lanes == 3) ? uint8_t(NVFX_FP_MASK_X |
                                                       NVFX_FP_MASK_Y |
                                                       NVFX_FP_MASK_Z) :
                            (bind.lanes == 2) ? uint8_t(NVFX_FP_MASK_X |
                                                       NVFX_FP_MASK_Y) :
                                                 NVFX_FP_MASK_X;

                        // ── Inst 1: ADD R0, f[varying], ±c[uniform] ──
                        {
                            struct nvfx_src s0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(inReg));
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                            s1.negate = bind.arithUniformNeg ? 1 : 0;
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(r0a),
                                arithMask, s0, s1, s2);
                            asm_.emit(in, NVFX_FP_OP_OPCODE_ADD);

                            const uint32_t off = asm_.currentByteSize();
                            for (auto& eu : attrs.embeddedUniforms)
                                if (eu.entryParamIndex == uIt->second)
                                { eu.ucodeByteOffsets.push_back(off); break; }
                            static const float zerosBlock[4] = { 0, 0, 0, 0 };
                            asm_.appendConstBlock(zerosBlock);

                            attrs.attributeInputMask |=
                                fpAttrMaskBitForInputSrc(inIt->second);
                            if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                                inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            {
                                const int n = inIt->second -
                                    NVFX_FP_OP_INPUT_SRC_TC(0);
                                attrs.texCoordsInputMask |= uint16_t{1} << n;
                                if (bind.lanes > 2)
                                    attrs.texCoords2D &= ~(uint16_t{1} << n);
                            }
                        }

                        // ── Inst 2: DP3 R0.w, R0, R0 ──
                        {
                            struct nvfx_src s0 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r0a));
                            struct nvfx_src s1 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r0a));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            const uint8_t dpOp =
                                (bind.lanes >= 4) ? NVFX_FP_OP_OPCODE_DP4
                                                  : NVFX_FP_OP_OPCODE_DP3;
                            struct nvfx_insn in = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(r0a),
                                NVFX_FP_MASK_W, s0, s1, s2);
                            asm_.emit(in, dpOp);
                        }

                        // wrapW1 — same MOV R0.w = c[0].x literal
                        // pattern as the direct-varying path.
                        if (bind.wrapW1)
                        {
                            struct nvfx_src wS0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                            wS0.swz[0] = wS0.swz[1] =
                            wS0.swz[2] = wS0.swz[3] = 0;
                            struct nvfx_src wS1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src wS2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn wIn = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(r0a),
                                NVFX_FP_MASK_W, wS0, wS1, wS2);
                            asm_.emit(wIn, NVFX_FP_OP_OPCODE_MOV);
                            const float oneXyzw[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
                            asm_.appendConstBlock(oneXyzw);
                        }

                        // ── Inst 3: DIVRSQR R0.<active>, R0, R0.wwww ──
                        {
                            struct nvfx_src s0 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r0a));
                            struct nvfx_src s1 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r0a));
                            s1.swz[0] = s1.swz[1] =
                            s1.swz[2] = s1.swz[3] = 3;  // wwww
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn in = nvfx_insn(
                                saturate ? 1 : 0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                arithMask, s0, s1, s2);
                            in.precision = dstPrecision;
                            asm_.emit(in, NVFX_FP_OP_OPCODE_DIVRSQ_NV40RSX);
                        }

                        emittedSomething = true;
                        break;
                    }

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

                // `reflect(I, N)` — verified byte-exact against the
                // reference compiler on a 2-varying probe.  Five
                // instructions plus an optional VecConstruct lane-
                // override tail when wrapped in `float4(reflect(...),
                // 1.0)`:
                //   MOVR R0.xyz, f[N]
                //   MOVR R1.xyz, f[I]
                //   DP3R(2X) R0.w, R0, R1            ; scale=2X folds the *2
                //   FENCBR
                //   MADR R0.xyz, -R0, R0.wwww, R1   ; I - 2*dot(N,I)*N
                // emitLaneOverrides() at the end of StoreOutput
                // appends the W=1.0 MOV when the consuming
                // VecInsert chain demands it.
                auto refIt = valueToReflect.find(srcId);
                if (refIt != valueToReflect.end())
                {
                    const auto& bind = refIt->second;
                    auto nIn = valueToInputSrc.find(bind.nId);
                    auto iIn = valueToInputSrc.find(bind.iId);
                    if (nIn == valueToInputSrc.end() ||
                        iIn == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: reflect() operand bindings did not resolve");
                        return out;
                    }

                    const struct nvfx_reg r0 = nvfx_reg(NVFXSR_TEMP, 0);
                    const struct nvfx_reg r1 = nvfx_reg(NVFXSR_TEMP, 1);
                    const struct nvfx_reg nReg =
                        nvfx_reg(NVFXSR_INPUT, nIn->second);
                    const struct nvfx_reg iReg =
                        nvfx_reg(NVFXSR_INPUT, iIn->second);

                    // ── Inst 1: MOVR R0.xyz, f[N] ──
                    {
                        struct nvfx_src s0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(nReg));
                        struct nvfx_src s1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src s2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn in = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(r0),
                            uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y |
                                    NVFX_FP_MASK_Z),
                            s0, s1, s2);
                        asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(nIn->second);
                        if (nIn->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            nIn->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = nIn->second - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoordsInputMask |= uint16_t{1} << n;
                            attrs.texCoords2D &= ~(uint16_t{1} << n);
                        }
                    }

                    // ── Inst 2: MOVR R1.xyz, f[I] ──
                    {
                        struct nvfx_src s0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(iReg));
                        struct nvfx_src s1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src s2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn in = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(r1),
                            uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y |
                                    NVFX_FP_MASK_Z),
                            s0, s1, s2);
                        asm_.emit(in, NVFX_FP_OP_OPCODE_MOV);
                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(iIn->second);
                        if (iIn->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            iIn->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                        {
                            const int n = iIn->second - NVFX_FP_OP_INPUT_SRC_TC(0);
                            attrs.texCoordsInputMask |= uint16_t{1} << n;
                            attrs.texCoords2D &= ~(uint16_t{1} << n);
                        }
                    }

                    // ── Inst 3: DP3R(2X) R0.w, R0, R1 ──
                    // DST_SCALE_2X folds the constant `2 *` from the
                    // `2.0 * dot(N, I)` term in the reflect formula.
                    {
                        struct nvfx_src s0 = nvfx_src(
                            const_cast<struct nvfx_reg&>(r0));
                        struct nvfx_src s1 = nvfx_src(
                            const_cast<struct nvfx_reg&>(r1));
                        struct nvfx_src s2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn in = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(r0),
                            NVFX_FP_MASK_W, s0, s1, s2);
                        in.scale = NVFX_FP_OP_DST_SCALE_2X;
                        asm_.emit(in, NVFX_FP_OP_OPCODE_DP3);
                    }

                    // ── Inst 4: FENCBR ──
                    asm_.emitFencbr();

                    // ── Inst 5: MADR R0.xyz, -R0, R0.wwww, R1 ──
                    // Computes I - 2*dot(N,I)*N lane-by-lane.
                    {
                        struct nvfx_src s0 = nvfx_src(
                            const_cast<struct nvfx_reg&>(r0));
                        s0.negate = 1;
                        struct nvfx_src s1 = nvfx_src(
                            const_cast<struct nvfx_reg&>(r0));
                        s1.swz[0] = s1.swz[1] = s1.swz[2] = s1.swz[3] = 3; // wwww
                        struct nvfx_src s2 = nvfx_src(
                            const_cast<struct nvfx_reg&>(r1));
                        struct nvfx_insn in = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            uint8_t(NVFX_FP_MASK_X | NVFX_FP_MASK_Y |
                                    NVFX_FP_MASK_Z),
                            s0, s1, s2);
                        in.precision = dstPrecision;
                        asm_.emit(in, NVFX_FP_OP_OPCODE_MAD);
                    }

                    // VecConstruct(reflect, 1.0) tail: MOV R0.w =
                    // c[0].x with inline {1.0, 0, 0, 0}.  Same shape
                    // as the Normalize wrapW1 path.
                    if (bind.wrapW1)
                    {
                        const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                        struct nvfx_src wS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        wS0.swz[0] = wS0.swz[1] = wS0.swz[2] = wS0.swz[3] = 0;
                        struct nvfx_src wS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src wS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn wIn = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_W, wS0, wS1, wS2);
                        wIn.precision = dstPrecision;
                        asm_.emit(wIn, NVFX_FP_OP_OPCODE_MOV);

                        const float oneXyzw[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
                        asm_.appendConstBlock(oneXyzw);
                    }

                    if (!emitLaneOverrides()) return out;
                    emittedSomething = true;
                    break;
                }

                // `float4(scale_scalar, k1, k2, k3)` lane-elision —
                // shape depends on scaledLane (the lane the MAD writes):
                //
                //   scaledLane=X (0):  3 insts MOVH + MOVR + MAD[END]    (or 2 insts if all-zero)
                //   scaledLane=W (3):  3 insts MOVH + MAD + MOVR[END]    (no FENCBR)
                //   scaledLane=Y (1):  4 insts MOVH + MAD + FENCBR + MOVR[END]
                //   scaledLane=Z (2):  4 insts MOVH + MAD + FENCBR + MOVR[END]
                //
                // Const packing for scaledLane != 0 (verified against
                // sce-cgc probes for 2-3 unique values): walk the
                // non-scaled dst lanes in order, each value gets the
                // next free inline slot (X, Y, Z) on first sight; the
                // swz position at the *scaled* lane re-uses the slot
                // that the next non-scaled lane picked (or W slot 3
                // when the scaled lane is W and no "next" exists).
                //
                // 2-unique scaledLane=W has a "shift by 1" allocation
                // sce-cgc uses (inline.X is filler, real values land
                // in slots 1..2) — not yet implemented; bails out.
                auto slIt = valueToScaledLanes.find(srcId);
                if (slIt != valueToScaledLanes.end() &&
                    slIt->second.scale.lane >= 0 &&
                    slIt->second.scale.scale >= 2 &&
                    slIt->second.scaledLane != 0)
                {
                    const auto& sl = slIt->second;
                    const auto& sb = sl.scale;
                    auto inIt = valueToInputSrc.find(sb.inputId);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scaled-lanes input failed to resolve");
                        return out;
                    }

                    // Pack non-scaled dst lanes (skip scaledLane) in
                    // order; record per-dst-lane swz position.
                    float inlinePack[4] = {0,0,0,0};
                    int   swzPos[4]     = {0,0,0,0};
                    int   slotCount     = 0;
                    bool  packOk        = true;
                    int   firstNonScaledAfterScaled = -1;
                    for (int dstLane = 0; dstLane < 4; ++dstLane)
                    {
                        if (dstLane == sl.scaledLane) continue;
                        const float v = sl.consts[dstLane];
                        int slot = -1;
                        for (int s = 0; s < slotCount; ++s)
                            if (inlinePack[s] == v) { slot = s; break; }
                        if (slot < 0)
                        {
                            if (slotCount >= 3) { packOk = false; break; }
                            slot = slotCount++;
                            inlinePack[slot] = v;
                        }
                        swzPos[dstLane] = slot;
                        if (firstNonScaledAfterScaled < 0 &&
                            dstLane > sl.scaledLane)
                            firstNonScaledAfterScaled = dstLane;
                    }
                    if (!packOk)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scaled-lanes (scaledLane != 0) needs ≤ 3 "
                            "unique const values across non-scaled lanes");
                        return out;
                    }

                    // 2-unique scaledLane=W has a shift-by-1 allocation
                    // sce-cgc uses; we don't implement it yet.  3-unique
                    // is fine because every slot gets used naturally.
                    if (sl.scaledLane == 3 && slotCount < 3)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scaled-lanes scaledLane=W with < 3 "
                            "unique values not yet supported (sce-cgc shift-by-1)");
                        return out;
                    }

                    // Swz at the scaled-lane position: matches the swz
                    // of the first non-scaled lane > scaledLane (i.e.
                    // the lane that "comes next").  For scaledLane=W,
                    // there's no "next" — sce-cgc uses W (slot 3 = filler).
                    if (sl.scaledLane == 3)
                        swzPos[sl.scaledLane] = 3;
                    else if (firstNonScaledAfterScaled >= 0)
                        swzPos[sl.scaledLane] = swzPos[firstNonScaledAfterScaled];

                    // Per-scaledLane shape selection:
                    //   X  (0) — handled by the scaledLane=0 branch below
                    //   Y  (1) — MOVH H0 → MAD → FENCBR → MOVR[END]
                    //   Z  (2) — MOVH H1 → MOVR → FENCBR → MAD[END]
                    //   W  (3) — MOVH H0 → MAD → MOVR[END]   (no FENCBR)
                    const int  hRegIdx     = (sl.scaledLane == 2) ? 1 : 0;
                    const bool needsFencbr = (sl.scaledLane == 1 || sl.scaledLane == 2);
                    const bool madFirst    = (sl.scaledLane != 2);

                    // ── Inst 1: MOVH H<idx>.<lane>, f[input] ──
                    struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, hRegIdx);
                    hReg.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t laneMask = uint8_t(1 << sb.lane);
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1, hReg,
                        laneMask, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);

                    // Closure-style: emit MAD R0.<scaledLane> = H<idx> * c.xxxx + H<idx>.
                    // SRC0/SRC2 must broadcast .xxxx — the only preloaded H lane.
                    auto emitMad = [&]() {
                        struct nvfx_src madS0 = nvfx_src(hReg);
                        madS0.swz[0] = madS0.swz[1] = madS0.swz[2] = madS0.swz[3] = 0;
                        struct nvfx_src madS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        madS1.swz[0] = madS1.swz[1] = madS1.swz[2] = madS1.swz[3] = 0;
                        struct nvfx_src madS2 = nvfx_src(hReg);
                        madS2.swz[0] = madS2.swz[1] = madS2.swz[2] = madS2.swz[3] = 0;
                        const uint8_t scaledMask = uint8_t(1 << sl.scaledLane);
                        struct nvfx_insn mad = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            scaledMask, madS0, madS1, madS2);
                        mad.precision = dstPrecision;
                        asm_.emit(mad, NVFX_FP_OP_OPCODE_MAD);
                        const float scaleInline[4] = {
                            static_cast<float>(sb.scale - 1), 0.0f, 0.0f, 0.0f
                        };
                        asm_.appendConstBlock(scaleInline);
                    };
                    auto emitMovr = [&]() {
                        struct nvfx_src constMovS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        constMovS0.swz[0] = (uint8_t)swzPos[0];
                        constMovS0.swz[1] = (uint8_t)swzPos[1];
                        constMovS0.swz[2] = (uint8_t)swzPos[2];
                        constMovS0.swz[3] = (uint8_t)swzPos[3];
                        struct nvfx_src constMovS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src constMovS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        const uint8_t constMask =
                            uint8_t(NVFX_FP_MASK_ALL ^ (1 << sl.scaledLane));
                        struct nvfx_insn constMov = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            constMask, constMovS0, constMovS1, constMovS2);
                        constMov.precision = dstPrecision;
                        asm_.emit(constMov, NVFX_FP_OP_OPCODE_MOV);
                        asm_.appendConstBlock(inlinePack);
                    };

                    if (madFirst)
                    {
                        emitMad();
                        if (needsFencbr) asm_.emitFencbr();
                        emitMovr();
                    }
                    else
                    {
                        emitMovr();
                        if (needsFencbr) asm_.emitFencbr();
                        emitMad();
                    }

                    emittedSomething = true;
                    break;
                }

                // `float4(scale_scalar, k1, k2, k3)` lane-elision (scaledLane=0
                // path) — sce-cgc accumulator-fold output shape.  Either
                // 2 or 3 instructions depending on whether the non-scaled
                // lanes are all zero:
                //
                //   MOVH H0.<scaledLane>, f[input]
                //   [MOVR R0.<const_mask>, c[0].<swz> + inline]      # skipped if all consts == 0
                //   MAD R0.<scaledLane> [END], H0, c[0].xxxx, H0 + inline(scale-1, 0, 0, 0)
                //
                // Const packing follows sce-cgc's canonical order
                // (verified against ten byte-probes): walk the
                // non-scaled lanes in order; each value gets the next
                // free inline slot (X, Y, Z) on first sight, with
                // later occurrences re-using the prior slot.  This
                // gives:
                //   1 unique:  inline=(v,0,0,0), swz=.xxxx
                //   2 unique:  inline=(u0,u1,0,0), swz mixes X / Y
                //   3 unique:  inline=(u0,u1,u2,0), swz mixes X / Y / Z
                if (slIt != valueToScaledLanes.end() &&
                    slIt->second.scaledLane == 0 &&
                    slIt->second.scale.lane >= 0 &&
                    slIt->second.scale.scale >= 2)
                {
                    const auto& sl = slIt->second;
                    const auto& sb = sl.scale;
                    auto inIt = valueToInputSrc.find(sb.inputId);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scaled-lanes input failed to resolve");
                        return out;
                    }

                    // Pack non-scaled-lane consts into ≤ 3 inline
                    // slots, recording the per-dst-lane swz position.
                    float    inlinePack[4] = {0,0,0,0};
                    int      swzPos[4]     = {0,0,0,0};
                    int      slotCount     = 0;
                    bool     allZero       = true;
                    bool     packOk        = true;
                    for (int dstLane = 1; dstLane < 4; ++dstLane)
                    {
                        const float v = sl.consts[dstLane];
                        if (v != 0.0f) allZero = false;
                        int slot = -1;
                        for (int s = 0; s < slotCount; ++s)
                            if (inlinePack[s] == v) { slot = s; break; }
                        if (slot < 0)
                        {
                            if (slotCount >= 3) { packOk = false; break; }
                            slot = slotCount++;
                            inlinePack[slot] = v;
                        }
                        swzPos[dstLane] = slot;
                    }
                    if (!packOk)
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scaled-lanes needs ≤ 3 unique const values "
                            "across non-scaled lanes");
                        return out;
                    }

                    // ── Inst 1: MOVH H0.<lane>, f[input] ──
                    struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, 0);
                    hReg.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    const uint8_t laneMask = uint8_t(1 << sb.lane);  // X→1, Y→2, Z→4, W→8
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1, hReg,
                        laneMask, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);

                    // ── Inst 2 (skipped when allZero): MOVR R0.<const_mask>, c[0].<swz> + inline ──
                    if (!allZero)
                    {
                        struct nvfx_src constMovS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        constMovS0.swz[0] = 0;            // dst.x unused (mask omits .x)
                        constMovS0.swz[1] = (uint8_t)swzPos[1];
                        constMovS0.swz[2] = (uint8_t)swzPos[2];
                        constMovS0.swz[3] = (uint8_t)swzPos[3];
                        struct nvfx_src constMovS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src constMovS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                        const uint8_t constMask = uint8_t(NVFX_FP_MASK_ALL ^ (1 << sl.scaledLane));
                        struct nvfx_insn constMov = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            constMask, constMovS0, constMovS1, constMovS2);
                        constMov.precision = dstPrecision;
                        asm_.emit(constMov, NVFX_FP_OP_OPCODE_MOV);
                        asm_.appendConstBlock(inlinePack);
                    }

                    // ── Final inst [END]: MAD R0.<scaledLane>, H0, c[0].xxxx, H0 ──
                    struct nvfx_src madS0 = nvfx_src(hReg);   // H0.xyzw
                    struct nvfx_src madS1 =
                        nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                    madS1.swz[0] = madS1.swz[1] = madS1.swz[2] = madS1.swz[3] = 0;  // .xxxx
                    struct nvfx_src madS2 = nvfx_src(hReg);   // H0.xyzw
                    const uint8_t scaledMask = uint8_t(1 << sl.scaledLane);
                    struct nvfx_insn mad = nvfx_insn(
                        saturate ? 1 : 0, 0, -1, -1,
                        const_cast<struct nvfx_reg&>(dstReg),
                        scaledMask, madS0, madS1, madS2);
                    mad.precision = dstPrecision;
                    asm_.emit(mad, NVFX_FP_OP_OPCODE_MAD);

                    const float scaleInline[4] = {
                        static_cast<float>(sb.scale - 1), 0.0f, 0.0f, 0.0f
                    };
                    asm_.appendConstBlock(scaleInline);

                    emittedSomething = true;
                    break;
                }

                // Repeated-add fold: `vcol + vcol + ... (N copies)`.
                //
                // sce-cgc uses two distinct shapes depending on N:
                //
                //   N=2,3 (short shape):
                //     MOVH H0, f[input]
                //     MAD  R0 [END], H0, c[0].xxxx, H0       + inline (N-1, 0, 0, 0)
                //
                //   N>=4 (long shape — verified via probes
                //   /tmp/rsx-cg-chain-probe/pent_n{4..12}_f.fpo):
                //     MOVH H0, f[input]
                //     MUL  R1, H0, c[0].xxxx                 + inline (2, 0, 0, 0)
                //     [MAD R1, H0, c[0].xxxx, R1            + inline (2, 0, 0, 0)]  × (K-1)
                //     [FENCBR]                               -- iff K is odd
                //     ADD  R0 [END], H0, R1                            (if N odd)
                //   OR
                //     MAD  R0 [END], H0, c[0].xxxx, R1      + inline (2, 0, 0, 0)  (if N even)
                //
                //   where K = (N - 1) / 2 (chain steps that double R1
                //   from 2v to 2K·v).  The final ADD adds 1·v, the
                //   final MAD adds 2·v, giving N·v overall.  FENCBR
                //   appears between the chain and the final write iff
                //   K is odd (a back-end pipeline-stall pattern; see
                //   probes for the exact rule).
                auto scIt = valueToScale.find(srcId);
                if (scIt != valueToScale.end() && scIt->second.scale >= 2)
                {
                    const auto& sb = scIt->second;
                    auto inIt = valueToInputSrc.find(sb.inputId);
                    if (inIt == valueToInputSrc.end())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: scale-fold input failed to resolve");
                        return out;
                    }

                    // MOVH H0.xyzw, f[input]
                    struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, 0);
                    hReg.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1, hReg,
                        NVFX_FP_MASK_ALL, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    const struct nvfx_reg r1Reg    = nvfx_reg(NVFXSR_TEMP, 1);

                    if (sb.scale <= 3)
                    {
                        // Short shape: single MAD R0 = H0 * (N-1) + H0.
                        struct nvfx_src madS0 = nvfx_src(hReg);
                        struct nvfx_src madS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        // SRC1 reads c[0].xxxx — broadcasts lane X of
                        // the inline literal block (which holds
                        // scale-1 in lane X, padded with zeros).
                        // nvfx_src() defaults to .xyzw identity, so
                        // override explicitly.
                        madS1.swz[0] = madS1.swz[1] = madS1.swz[2] = madS1.swz[3] = 0;
                        struct nvfx_src madS2 = nvfx_src(hReg);
                        struct nvfx_insn mad = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_ALL,
                            madS0, madS1, madS2);
                        mad.precision = dstPrecision;
                        asm_.emit(mad, NVFX_FP_OP_OPCODE_MAD);

                        const float lit[4] = {
                            static_cast<float>(sb.scale - 1),
                            0.0f, 0.0f, 0.0f
                        };
                        asm_.appendConstBlock(lit);
                    }
                    else
                    {
                        // Long shape (N >= 4): chain MUL/MAD into R1
                        // doubling each step, then a final ADD or MAD
                        // onto R0 that contributes the leftover 1 or
                        // 2 copies of H0.
                        const int N = sb.scale;
                        const int K = (N - 1) / 2;     // chain length
                        const bool needFencbr = (K % 2) == 1;
                        const bool finalIsMad = (N % 2) == 0;
                        static const float twoLit[4] = { 2.0f, 0.0f, 0.0f, 0.0f };

                        // Helper: emit one R1 chain step (MUL on first,
                        // MAD on subsequent) with the inline {2,0,0,0}
                        // multiplier at c[0].xxxx.
                        auto emitChainStep = [&](bool isFirst)
                        {
                            struct nvfx_src s0 = nvfx_src(hReg);
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                            s1.swz[0] = s1.swz[1] = s1.swz[2] = s1.swz[3] = 0;
                            struct nvfx_src s2 = isFirst
                                ? nvfx_src(const_cast<struct nvfx_reg&>(none))
                                : nvfx_src(const_cast<struct nvfx_reg&>(r1Reg));
                            struct nvfx_insn step = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(r1Reg),
                                NVFX_FP_MASK_ALL, s0, s1, s2);
                            step.precision = FLOAT32;
                            asm_.emit(step, isFirst ? NVFX_FP_OP_OPCODE_MUL
                                                    : NVFX_FP_OP_OPCODE_MAD);
                            asm_.appendConstBlock(twoLit);
                        };

                        for (int i = 0; i < K; ++i)
                            emitChainStep(/*isFirst=*/i == 0);

                        if (needFencbr)
                            asm_.emitFencbr();

                        // Final write to R0 — last instruction.
                        if (finalIsMad)
                        {
                            // MAD R0 [END], H0, c[0].xxxx, R1 + inline {2,0,0,0}
                            struct nvfx_src s0 = nvfx_src(hReg);
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                            s1.swz[0] = s1.swz[1] = s1.swz[2] = s1.swz[3] = 0;
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(r1Reg));
                            struct nvfx_insn mad = nvfx_insn(
                                saturate ? 1 : 0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_ALL, s0, s1, s2);
                            mad.precision = dstPrecision;
                            asm_.emit(mad, NVFX_FP_OP_OPCODE_MAD);
                            asm_.appendConstBlock(twoLit);
                        }
                        else
                        {
                            // ADD R0 [END], H0, R1 — no inline block.
                            struct nvfx_src s0 = nvfx_src(hReg);
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(r1Reg));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn add = nvfx_insn(
                                saturate ? 1 : 0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_ALL, s0, s1, s2);
                            add.precision = dstPrecision;
                            asm_.emit(add, NVFX_FP_OP_OPCODE_ADD);
                        }
                    }

                    emittedSomething = true;
                    break;
                }

                // N-ary chain `vcol + a + b + c` — preload root
                // varying into H0 once, then a sequence of ADDR
                // R0,(prev),c[inline] linking through R0.  Each link
                // emits its own 16-byte zero block (uniform patch slot)
                // exactly like the single-arith path; markEnd() stamps
                // PROGRAM_END on whichever ADDR ends up last.
                auto chIt = valueToChain.find(srcId);
                if (chIt != valueToChain.end())
                {
                    const auto& chain = chIt->second;
                    auto inIt = valueToInputSrc.find(chain.rootInputId);
                    if (inIt == valueToInputSrc.end() || chain.links.empty())
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: chain root failed to resolve");
                        return out;
                    }

                    // Resolve all uniform IDs up front so we bail
                    // before emitting anything if any link is broken.
                    std::vector<unsigned> linkUniformParam;
                    linkUniformParam.reserve(chain.links.size());
                    for (const auto& link : chain.links)
                    {
                        auto uIt = valueToFpUniform.find(link.uniformId);
                        if (uIt == valueToFpUniform.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: chain link uniform failed to resolve");
                            return out;
                        }
                        linkUniformParam.push_back(uIt->second);
                    }

                    // MOVH H0.xyzw, f[input]
                    struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, 0);
                    hReg.is_fp16 = 1;
                    const struct nvfx_reg inputReg =
                        nvfx_reg(NVFXSR_INPUT, inIt->second);
                    struct nvfx_src mvhS0 = nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                    mvhS0.abs    = chain.rootInputAbs ? 1 : 0;
                    mvhS0.negate = chain.rootInputNeg ? 1 : 0;
                    struct nvfx_src mvhS1 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_src mvhS2 = nvfx_src(const_cast<struct nvfx_reg&>(none));
                    struct nvfx_insn mvh = nvfx_insn(
                        0, 0, -1, -1, hReg,
                        NVFX_FP_MASK_ALL, mvhS0, mvhS1, mvhS2);
                    mvh.precision = FLOAT16;
                    asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                    attrs.attributeInputMask |=
                        fpAttrMaskBitForInputSrc(inIt->second);
                    if (inIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                        inIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                    {
                        attrs.texCoordsInputMask |=
                            uint16_t{1} << (inIt->second - NVFX_FP_OP_INPUT_SRC_TC(0));
                    }

                    // ADDR R0, (H0 first iter / R0 thereafter), c[inline]
                    const struct nvfx_reg constReg = nvfx_reg(NVFXSR_CONST, 0);
                    const struct nvfx_reg r0Full   = nvfx_reg(NVFXSR_TEMP, 0);

                    for (size_t i = 0; i < chain.links.size(); ++i)
                    {
                        const auto& link = chain.links[i];

                        // First iter reads H0; later iters read R0 (the
                        // running accumulator).  Either way SRC0 is
                        // TEMP — which canonicalises ADD as
                        // (TEMP, CONST), reverse of the lone-add
                        // (CONST, INPUT) order verified in add_f.
                        struct nvfx_reg accReg = (i == 0) ? hReg : r0Full;
                        struct nvfx_src srcAcc = nvfx_src(accReg);
                        struct nvfx_src srcCnst =
                            nvfx_src(const_cast<struct nvfx_reg&>(constReg));
                        srcCnst.abs    = link.uniformAbs ? 1 : 0;
                        srcCnst.negate = link.uniformNeg ? 1 : 0;
                        struct nvfx_src srcUnused =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));

                        const bool isLast = (i + 1 == chain.links.size());
                        // Saturate is a StoreOutput-side modifier that
                        // only the final write should carry.
                        struct nvfx_insn add = nvfx_insn(
                            (isLast && saturate) ? 1 : 0,
                            0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_ALL,
                            srcAcc, srcCnst, srcUnused);
                        add.precision = isLast ? dstPrecision : FLOAT32;
                        asm_.emit(add, NVFX_FP_OP_OPCODE_ADD);

                        const uint32_t ucodeByteOffset = asm_.currentByteSize();
                        for (auto& eu : attrs.embeddedUniforms)
                        {
                            if (eu.entryParamIndex == linkUniformParam[i])
                            {
                                eu.ucodeByteOffsets.push_back(ucodeByteOffset);
                                break;
                            }
                        }
                        static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        asm_.appendConstBlock(zeros);
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

                    // Detect (varying, temp) Mul pattern: uniformId
                    // was repurposed to store the temp operand, so
                    // it won't be in valueToFpUniform.
                    const bool isVaryingTempMul =
                        (bind.op == FpArithOp::Mul &&
                         iIt != valueToInputSrc.end() &&
                         uIt == valueToFpUniform.end());

                    if (!isVaryingTempMul &&
                        (iIt == valueToInputSrc.end() ||
                         uIt == valueToFpUniform.end()))
                    {
                        out.diagnostics.push_back(
                            "nv40-fp: arithmetic operand failed to resolve");
                        return out;
                    }

                    if (isVaryingTempMul)
                    {
                        // (varying, temp) Mul — reference compiler
                        // shape: MOVH H2.x, f[input]; MUL dst, temp, H2.x
                        struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, 2);
                        hReg.is_fp16 = 1;
                        const struct nvfx_reg inputReg =
                            nvfx_reg(NVFXSR_INPUT, iIt->second);
                        struct nvfx_src mvhS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                        mvhS0.abs    = bind.inputAbs ? 1 : 0;
                        mvhS0.negate = bind.inputNeg ? 1 : 0;
                        struct nvfx_src mvhS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src mvhS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn mvh = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(hReg),
                            NVFX_FP_MASK_X, mvhS0, mvhS1, mvhS2);
                        mvh.precision = FLOAT16;
                        asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(iIt->second);
                        if (iIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            iIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            attrs.texCoordsInputMask |=
                                uint16_t{1} << (iIt->second -
                                                NVFX_FP_OP_INPUT_SRC_TC(0));

                        // MUL dst, temp(R0), H2.x
                        const struct nvfx_reg tempReg =
                            nvfx_reg(NVFXSR_TEMP, 0);
                        struct nvfx_src s0 = nvfx_src(
                            const_cast<struct nvfx_reg&>(tempReg));
                        struct nvfx_src s1 = nvfx_src(hReg);
                        s1.swz[0] = s1.swz[1] = s1.swz[2] = s1.swz[3] = 0;
                        struct nvfx_src s2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));

                        struct nvfx_insn in = nvfx_insn(
                            saturate ? 1 : 0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(dstReg),
                            NVFX_FP_MASK_W, s0, s1, s2);
                        in.precision = dstPrecision;
                        asm_.emit(in, NVFX_FP_OP_OPCODE_MUL);
                    }
                    else
                    {

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
                    }  // end else (uniform arithmetic)

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

                // Post-discard VecConstructTexMul: float4(tex.xyz, varying * tex.w)
                // Lowers to TEX R0; MOVH H2.x, f[varying]; MOVR R0.xyz, R0; MULR R0.w, R0, H2.x
                {
                    auto vctmIt = valueToVecConstructTexMul.find(srcId);
                    // std::fprintf(stderr, "DEBUG StoreOutput: srcId=%%%u vctm=%s\n",
                    //              (unsigned)srcId,
                    //              vctmIt != valueToVecConstructTexMul.end() ? "yes" : "no");
                    if (vctmIt != valueToVecConstructTexMul.end())
                    {
                        const auto& b = vctmIt->second;

                        // Emit TEX for the tex result (if not already
                        // emitted by the Discard handler)
                        if (!emittedTexResults.count(b.texBaseId))
                        {
                            auto texIt = valueToTex.find(b.texBaseId);
                            if (texIt == valueToTex.end())
                            {
                                out.diagnostics.push_back(
                                    "nv40-fp: VecConstructTexMul tex base not resolved");
                                return out;
                            }
                            auto sampIt = valueToTexUnit.find(texIt->second.samplerId);
                            if (sampIt == valueToTexUnit.end())
                            {
                                out.diagnostics.push_back(
                                    "nv40-fp: VecConstructTexMul sampler not resolved");
                                return out;
                            }
                            IRValueID uvBase = resolveSrcMods(texIt->second.uvId).baseId;
                            auto uvIt = valueToInputSrc.find(uvBase);
                            if (uvIt == valueToInputSrc.end())
                            {
                                out.diagnostics.push_back(
                                    "nv40-fp: VecConstructTexMul UV not resolved");
                                return out;
                            }

                            const struct nvfx_reg uvReg =
                                nvfx_reg(NVFXSR_INPUT, uvIt->second);
                            struct nvfx_src texSrc0 =
                                nvfx_src(const_cast<struct nvfx_reg&>(uvReg));
                            struct nvfx_src texSrc1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src texSrc2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));

                            const uint8_t texOpcode =
                                texIt->second.projective
                                    ? NVFX_FP_OP_OPCODE_TXP
                                    : NVFX_FP_OP_OPCODE_TEX;

                            struct nvfx_insn texInsn = nvfx_insn(
                                0, 0,
                                sampIt->second, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_ALL,
                                texSrc0, texSrc1, texSrc2);
                            texInsn.precision = dstPrecision;
                            if (texIt->second.cube)
                                texInsn.disable_pc = 1;
                            asm_.emit(texInsn, texOpcode);

                            attrs.attributeInputMask |=
                                fpAttrMaskBitForInputSrc(uvIt->second);
                            if (uvIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                                uvIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                                attrs.texCoordsInputMask |=
                                    uint16_t{1} << (uvIt->second -
                                                    NVFX_FP_OP_INPUT_SRC_TC(0));

                            emittedTexResults.insert(b.texBaseId);
                        }

                        // Resolve varying input
                        auto viIt = valueToInputSrc.find(b.varyingId);
                        if (viIt == valueToInputSrc.end())
                        {
                            out.diagnostics.push_back(
                                "nv40-fp: VecConstructTexMul varying not resolved");
                            return out;
                        }

                        // MOVH H2.x, f[varying]
                        struct nvfx_reg hReg = nvfx_reg(NVFXSR_TEMP, 2);
                        hReg.is_fp16 = 1;
                        const struct nvfx_reg inputReg =
                            nvfx_reg(NVFXSR_INPUT, viIt->second);
                        struct nvfx_src mvhS0 =
                            nvfx_src(const_cast<struct nvfx_reg&>(inputReg));
                        struct nvfx_src mvhS1 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_src mvhS2 =
                            nvfx_src(const_cast<struct nvfx_reg&>(none));
                        struct nvfx_insn mvh = nvfx_insn(
                            0, 0, -1, -1,
                            const_cast<struct nvfx_reg&>(hReg),
                            b.varyingLane >= 0
                                ? NVFX_FP_MASK_W
                                : NVFX_FP_MASK_X,
                            mvhS0, mvhS1, mvhS2);
                        mvh.precision = FLOAT16;
                        asm_.emit(mvh, NVFX_FP_OP_OPCODE_MOV);

                        attrs.attributeInputMask |=
                            fpAttrMaskBitForInputSrc(viIt->second);
                        if (viIt->second >= NVFX_FP_OP_INPUT_SRC_TC(0) &&
                            viIt->second <= NVFX_FP_OP_INPUT_SRC_TC(7))
                            attrs.texCoordsInputMask |=
                                uint16_t{1} << (viIt->second -
                                                NVFX_FP_OP_INPUT_SRC_TC(0));

                        // When the varying is a scalar lane of a vec4
                        // input, emit MULR before MOVR to match the
                        // reference schedule.
                        const bool mulrFirst = (b.varyingLane >= 0);

                        if (mulrFirst)
                        {
                            // MULR R0.w, R0, H2.<varying_lane>
                            const struct nvfx_reg r0Reg =
                                nvfx_reg(NVFXSR_TEMP, 0);
                            struct nvfx_src s0 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r0Reg));
                            struct nvfx_src s1 = nvfx_src(hReg);
                            // Identity swizzle: W→W reads H2.w
                            // which holds the varying value loaded
                            // by MOVH with MASK_W.
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn mul = nvfx_insn(
                                saturate ? 1 : 0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_W, s0, s1, s2);
                            mul.precision = dstPrecision;
                            asm_.emit(mul, NVFX_FP_OP_OPCODE_MUL);
                        }

                        // MOVR R0.xyz, R0 — identity to preserve tex xyz
                        {
                            const struct nvfx_reg r0Reg =
                                nvfx_reg(NVFXSR_TEMP, 0);
                            struct nvfx_src s0 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r0Reg));
                            struct nvfx_src s1 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn mov = nvfx_insn(
                                0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_X | NVFX_FP_MASK_Y | NVFX_FP_MASK_Z,
                                s0, s1, s2);
                            mov.precision = dstPrecision;
                            asm_.emit(mov, NVFX_FP_OP_OPCODE_MOV);
                        }

                        if (!mulrFirst)
                        {
                            // MULR R0.w, R0, H2.x (scalar varying case)
                            const struct nvfx_reg r0Reg =
                                nvfx_reg(NVFXSR_TEMP, 0);
                            struct nvfx_src s0 = nvfx_src(
                                const_cast<struct nvfx_reg&>(r0Reg));
                            struct nvfx_src s1 = nvfx_src(hReg);
                            s1.swz[0] = s1.swz[1] =
                            s1.swz[2] = s1.swz[3] = 0;
                            struct nvfx_src s2 =
                                nvfx_src(const_cast<struct nvfx_reg&>(none));
                            struct nvfx_insn mul = nvfx_insn(
                                saturate ? 1 : 0, 0, -1, -1,
                                const_cast<struct nvfx_reg&>(dstReg),
                                NVFX_FP_MASK_W, s0, s1, s2);
                            mul.precision = dstPrecision;
                            asm_.emit(mul, NVFX_FP_OP_OPCODE_MUL);
                        }

                        emittedSomething = true;
                        break;
                    }
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
                    const int n = it->second - NVFX_FP_OP_INPUT_SRC_TC(0);
                    attrs.texCoordsInputMask |= uint16_t{1} << n;
                    // 4-component varying read of TEXCOORDn — sce-cgc
                    // clears the 2D bit (bit only stays set when the
                    // slot is consumed by a non-projective 2D tex2D).
                    attrs.texCoords2D &= ~(uint16_t{1} << n);
                }
                break;
            }

            case IROp::Return:
                break;

            default:
                out.diagnostics.push_back(
                    "nv40-fp: unsupported IR op #" +
                    std::to_string(static_cast<int>(inst.op)));
                return out;
            }
        }
    }
    }  // pass loop

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
    // more full-precision temps.  fp16 H registers pack two per hw slot
    // (H0/H1→R0, H2/H3→R1, …) and are tracked correctly by emitDst.
    const int regs = asm_.numTempRegs();
    if (regs > attrs.registerCount)
        attrs.registerCount = static_cast<uint8_t>(regs);

    if (attrsOut)
        *attrsOut = attrs;
    return out;
}

}  // namespace nv40::detail
