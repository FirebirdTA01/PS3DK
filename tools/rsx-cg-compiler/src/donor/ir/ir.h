#pragma once

#include "types.h"
#include "ast.h"
#include "semantic.h"  // For ShaderStage
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <variant>
#include <optional>

// Forward declarations
class IRValue;
class IRInstruction;
class IRBasicBlock;
class IRFunction;
class IRModule;

// ============================================================================
// IR Types
// ============================================================================

// IR-level type representation (simpler than CgType)
enum class IRType
{
    Void,
    Bool,
    Int32,
    UInt32,
    Float32,
    Float16,

    // Vector types (component count encoded separately)
    Vec2,
    Vec3,
    Vec4,

    // Matrix types
    Mat2x2,
    Mat3x3,
    Mat4x4,

    // Sampler types
    Sampler1D,
    Sampler2D,
    Sampler3D,
    SamplerRect,
    SamplerCube,

    // Pointer type (for memory operations)
    Ptr
};

// A sampler binds a TEXTURE UNIT and never a constant slot.  The FP
// emitter and the container writer both number their file-scope globals
// on this predicate and must agree, or the two numberings drift and each
// global's metadata is attached to a different global's parameter.  They
// disagreed on SamplerRect - the emitter counted it, the container did
// not - so it lives here now rather than being spelled out at each of the
// four sites that need it (t_f5f750ff).
inline bool isSamplerIRType(IRType t)
{
    return t == IRType::Sampler1D ||
           t == IRType::Sampler2D ||
           t == IRType::Sampler3D ||
           t == IRType::SamplerRect ||
           t == IRType::SamplerCube;
}

struct IRTypeInfo
{
    IRType baseType = IRType::Void;
    int vectorSize = 1;         // 1 for scalar, 2-4 for vectors
    int matrixRows = 0;         // For matrices
    int matrixCols = 0;
    IRType elementType = IRType::Float32;  // For vectors/matrices
    int arraySize = 0;              // 0 for non-array, >0 for fixed-size arrays

    bool isScalar() const { return vectorSize == 1 && matrixRows == 0 && arraySize == 0; }
    bool isVector() const { return vectorSize > 1 && matrixRows == 0; }
    bool isMatrix() const { return matrixRows > 0; }
    bool isArray() const { return arraySize > 0; }

    int componentCount() const {
        if (matrixRows > 0) return matrixRows * matrixCols;
        return vectorSize;
    }

    std::string toString() const;

    static IRTypeInfo fromCgType(const CgType& cgType);
    static IRTypeInfo Void() { return {IRType::Void, 1, 0, 0}; }
    static IRTypeInfo Bool() { return {IRType::Bool, 1, 0, 0}; }
    static IRTypeInfo Int() { return {IRType::Int32, 1, 0, 0}; }
    static IRTypeInfo UInt() { return {IRType::UInt32, 1, 0, 0}; }
    static IRTypeInfo Float() { return {IRType::Float32, 1, 0, 0}; }
    static IRTypeInfo Half() { return {IRType::Float16, 1, 0, 0}; }
    static IRTypeInfo Float2() { return {IRType::Vec2, 2, 0, 0, IRType::Float32}; }
    static IRTypeInfo Float3() { return {IRType::Vec3, 3, 0, 0, IRType::Float32}; }
    static IRTypeInfo Float4() { return {IRType::Vec4, 4, 0, 0, IRType::Float32}; }
    static IRTypeInfo Float4x4() { return {IRType::Mat4x4, 1, 4, 4, IRType::Float32}; }
};

// ============================================================================
// IR Operations
// ============================================================================

enum class IROp
{
    // Constants
    Const,          // Constant value
    Undef,          // Undefined value

    // Arithmetic (scalar and vector)
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,            // Unary negation
    Mad,            // Multiply-add: a * b + c

    // Bitwise operations
    And,
    Or,
    Xor,
    Not,
    Shl,            // Shift left
    Shr,            // Shift right (arithmetic)
    UShr,           // Shift right (logical)

    // Comparison (returns bool/bvec)
    CmpEq,
    CmpNe,
    CmpLt,
    CmpLe,
    CmpGt,
    CmpGe,

    // Logical operations
    LogicalAnd,
    LogicalOr,
    LogicalNot,

    // Type conversions
    IntToFloat,
    FloatToInt,
    FloatToHalf,
    HalfToFloat,
    Bitcast,

    // Vector operations
    VecConstruct,   // Construct vector from scalars
    VecExtract,     // Extract scalar from vector (with index)
    VecInsert,      // Insert scalar into vector
    VecShuffle,     // Swizzle/shuffle components

    // Math functions
    Abs,
    Sign,
    Floor,
    Ceil,
    Frac,
    Round,
    Trunc,
    Min,
    Max,
    Clamp,
    Saturate,
    Ddx,
    Ddy,
    // Pack/unpack builtins (t_23f9d1a6): one NV40 instruction each,
    // fragment-only.  A pack reads a vector and yields one float whose bits
    // hold the packed lanes; an unpack reads that float.
    PackHalf2,
    UnpackHalf2,
    PackUByte4,
    UnpackUByte4,
    PackByte4,
    UnpackByte4,
    PackUShort2,
    UnpackUShort2,
    Lerp,
    Step,
    SmoothStep,
    Lit,

    // Trigonometric
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,

    // Exponential
    Pow,
    Exp,
    Exp2,
    Log,
    Log2,
    Log10,
    Sqrt,
    RSqrt,          // 1/sqrt(x)

    // Vector math
    Dot,
    Cross,
    Length,
    Distance,
    Normalize,
    Reflect,
    Refract,
    FaceForward,

    // Matrix operations
    MatMul,         // Matrix multiplication
    MatVecMul,      // Matrix * vector
    VecMatMul,      // Vector * matrix (row vector)
    Transpose,
    MatConstruct,   // Construct matrix from row/column vectors

    // Control flow
    Phi,            // SSA phi node
    Select,         // Ternary select (condition ? a : b)
    Branch,         // Unconditional branch
    CondBranch,     // Conditional branch
    Return,
    Discard,        // Fragment shader discard

    // Memory operations
    Load,
    Store,

    // Shader I/O
    LoadAttribute,  // Load vertex attribute
    LoadUniform,    // Load uniform value
    LoadVarying,    // Load varying (fragment shader input)
    StoreOutput,    // Store shader output
    StoreVarying,   // Store varying (vertex shader output)

    // Texture operations
    TexSample,      // tex2D, texCUBE
    TexSampleLod,   // tex2Dlod
    TexSampleGrad,  // tex2Dgrad
    TexSampleProj,  // tex2Dproj
    TexSampleBias,  // tex2Dbias
    TexFetch,       // texelFetch

    // Function calls
    Call,           // User function call

    // Special
    Nop,            // No operation
    Comment,        // Debug comment (not emitted)

    // Predicated execution — emitted by nv40_if_convert for multi-
    // instruction THEN blocks of an if-only diamond with explicit
    // default value.  Each PredCarry represents one THEN-block step
    // and lowers to two NV40 FP instructions: an unconditional
    // MOVR carrying the default into the chain dst, then the inner
    // op (encoded in `predOp`) re-executed against `dst(NE.x)` so
    // the conditional only commits when the predicate fires.
    //
    // Operand layout: [cond, defaultVal, op_arg0, op_arg1, ...].
    // `predOp` carries the inner op (Add / Mul / Mad / etc.) that
    // would have run unconditionally inside the THEN block.
    PredCarry
};

const char* irOpToString(IROp op);

// ============================================================================
// IR Values
// ============================================================================

// Unique ID for IR values
using IRValueID = uint32_t;
constexpr IRValueID InvalidIRValue = 0;

// Base class for all IR values
class IRValue
{
public:
    IRValueID id;
    IRTypeInfo type;
    std::string name;       // Optional debug name

    IRValue(IRValueID i, const IRTypeInfo& t, const std::string& n = "")
        : id(i), type(t), name(n) {}
    virtual ~IRValue() = default;

    std::string toString() const;
};

// Constant value
class IRConstant : public IRValue
{
public:
    std::variant<bool, int32_t, uint32_t, float, std::vector<float>> value;
    std::vector<int64_t> intValues;

    IRConstant(IRValueID id, const IRTypeInfo& type, bool v)
        : IRValue(id, type), value(v) {}
    IRConstant(IRValueID id, const IRTypeInfo& type, int32_t v)
        : IRValue(id, type), value(v) {}
    IRConstant(IRValueID id, const IRTypeInfo& type, uint32_t v)
        : IRValue(id, type), value(v) {}
    IRConstant(IRValueID id, const IRTypeInfo& type, float v)
        : IRValue(id, type), value(v) {}
    IRConstant(IRValueID id, const IRTypeInfo& type, const std::vector<float>& v)
        : IRValue(id, type), value(v) {}
    IRConstant(IRValueID id, const IRTypeInfo& type, const std::vector<float>& v, const std::vector<int64_t>& iv)
        : IRValue(id, type), value(v), intValues(iv) {}

    std::string valueToString() const;
};

// ============================================================================
// IR Instructions
// ============================================================================

class IRInstruction
{
public:
    IROp op;
    IRValueID result;               // Result value ID (0 if no result)
    IRTypeInfo resultType;
    std::vector<IRValueID> operands;

    // Additional data for specific instructions
    int swizzleMask = 0;            // For VecShuffle: encoded swizzle pattern
    int componentIndex = 0;          // For VecExtract/VecInsert

    // LoadUniform of an ARRAY uniform: how the element was chosen.  Explicit
    // so a lowering can never mistake element 0 for the bare array, nor a
    // run-time index for a lane selector (t_f9ecd3ac).
    //   None      the bare array (no index) - only meaningful to a caller
    //             that consumes whole arrays, and refused by the lowering
    //   Constant  componentIndex holds the element, already bounds-checked
    //             by the builder against the declared count
    //   Dynamic   operands[0] holds the integral index value; the element
    //             is chosen at run time
    enum class ArrayIndexKind { None, Constant, Dynamic };
    ArrayIndexKind arrayIndexKind = ArrayIndexKind::None;
    IROp predOp = IROp::Nop;         // For PredCarry: the inner op (Add/Mul/Mad/...)
    // For Discard: the guard operand is the condition on the path that
    // REACHES the discard, and this flag says the kill fires where that
    // condition is FALSE.  The reference folds a negated guard into the
    // KIL's condition-code test (NE becomes EQ) and leaves the
    // comparison alone rather than inverting it, so the flag has to
    // travel with the instruction (CF-2, t_91bbd575).
    bool guardIsNegated = false;
    bool shortCircuitRhs = false;
    std::string targetName;          // For branch targets, function calls
    int semanticIndex = 0;           // For shader I/O operations
    std::string semanticName;        // For shader I/O operations (digit-stripped)
    std::string rawSemanticName;     // For shader I/O operations (source spelling, e.g. "POSITION0")
    SourceLocation loc;              // Source expression/statement for diagnostics.
    // Struct-flattened entry params:
    //   LoadAttribute  for `input.pos` carries structParamName="input", fieldName="pos"
    //   StoreOutput    for `o.pos = ...` carries fieldName="pos" (parent struct name is
    //                  implicit — the reference compiler names outputs `<entry-func>.<field>`,
    //                  not `<struct-instance>.<field>`)
    std::string structParamName;     // Source struct-instance name (input side only)
    std::string fieldName;           // Struct member name (input + output)

    IRBasicBlock* parentBlock = nullptr;

    IRInstruction(IROp o, IRValueID res, const IRTypeInfo& resType)
        : op(o), result(res), resultType(resType) {}

    void addOperand(IRValueID op) { operands.push_back(op); }

    std::string toString() const;
};

// ============================================================================
// IR Basic Block
// ============================================================================

class IRBasicBlock
{
public:
    std::string name;
    std::vector<std::unique_ptr<IRInstruction>> instructions;
    IRFunction* parentFunction = nullptr;

    // Predecessors and successors for CFG
    std::vector<IRBasicBlock*> predecessors;
    std::vector<IRBasicBlock*> successors;

    explicit IRBasicBlock(const std::string& n) : name(n) {}

    IRInstruction* addInstruction(std::unique_ptr<IRInstruction> inst);

    // Check if block ends with a terminator
    bool hasTerminator() const;
    IRInstruction* getTerminator() const;

    std::string toString() const;
};

// ============================================================================
// IR Function
// ============================================================================

struct IRParameter
{
    std::string name;
    IRTypeInfo type;
    IRValueID valueId;

    // Shader parameter info
    StorageQualifier storage = StorageQualifier::None;
    std::string semanticName;
    std::string rawSemanticName;  // original source spelling (e.g. "TEXCOORD0"); empty if absent
    int semanticIndex = 0;
    bool inferredSemantic = false;  // semantic was assigned by the unbound-input default pass
};

class IRFunction
{
public:
    std::string name;
    IRTypeInfo returnType;
    // Declared struct return fields, including those never stored. The
    // fragment colour bank depends on all declarations, not just writes.
    std::vector<IRParameter> returnOutputs;
    std::vector<IRParameter> parameters;
    std::vector<std::unique_ptr<IRBasicBlock>> blocks;

    // Entry block is always blocks[0]
    IRBasicBlock* entryBlock = nullptr;

    // Value storage
    std::unordered_map<IRValueID, std::unique_ptr<IRValue>> values;
    IRValueID nextValueId = 1;

    IRModule* parentModule = nullptr;
    bool isEntryPoint = false;

    explicit IRFunction(const std::string& n) : name(n) {}

    // Block management
    IRBasicBlock* createBlock(const std::string& name);
    IRBasicBlock* getBlock(const std::string& name);

    // Value management
    IRValueID allocateValueId() { return nextValueId++; }
    IRValue* getValue(IRValueID id) const;
    IRConstant* createConstant(const IRTypeInfo& type, bool value);
    IRConstant* createConstant(const IRTypeInfo& type, int32_t value);
    IRConstant* createConstant(const IRTypeInfo& type, uint32_t value);
    IRConstant* createConstant(const IRTypeInfo& type, float value);
    IRConstant* createConstant(const IRTypeInfo& type, const std::vector<float>& value, const std::vector<int64_t>& intValues = {});

    std::string toString() const;
};

// ============================================================================
// IR Module
// ============================================================================

// Global variable/uniform
struct IRGlobal
{
    std::string name;
    IRTypeInfo type;
    IRValueID valueId;

    StorageQualifier storage = StorageQualifier::None;
    std::string semanticName;
    int semanticIndex = 0;

    // For uniforms: buffer index and offset
    int bufferIndex = -1;
    int bufferOffset = 0;

    // HLSL/Cg `: register(BANK<N>)` explicit binding.  Bank is 'C' for
    // const, 'S' for sampler, 'V' for varying; 0 means no binding and
    // the const allocator picks the slot.
    char explicitRegisterBank  = 0;
    int  explicitRegisterIndex = 0;

    // A file-scope `const`'s initialiser, evaluated when the global is
    // built.  Every reference to the const folds to an IRConstant made
    // from this, so a global without it is one whose value the backend
    // would have to invent rather than read from the source - which is
    // exactly what used to happen (t_4584aa27).  Empty means the global is
    // not a file-scope const with an evaluable initialiser; ordinary
    // uniforms leave it empty.
    std::vector<float> initialValue;
    std::vector<int64_t> initialIntValues;
};

class IRModule
{
public:
    std::string name;
    std::vector<std::unique_ptr<IRFunction>> functions;
    std::vector<IRGlobal> globals;

    // Shader-specific info
    ShaderStage shaderStage = ShaderStage::Vertex;
    std::string entryPointName = "main";
    IRFunction* entryPoint = nullptr;

    // Global value ID allocation.
    //
    // Globals and per-function values are both IRValueID and are looked up
    // from the SAME maps in the backends - the general path's
    // valueToSource is keyed by the id with no namespace - so the two
    // spaces must not overlap.  Both used to start at 1, so a file-scope
    // uniform's global id landed on an entry value's id and resolved to
    // whatever that value was: a varying.  `uniform float K;` read a
    // texcoord, silently, in a well-formed container (t_f5f750ff).
    //
    // Disjoint by base rather than by a shared counter, because the IR
    // builder identifies a just-allocated value as `nextValueId - 1` in a
    // dozen places and a shared counter would make every one of those
    // wrong the moment a global were allocated mid-function.  The base is
    // far past any reachable per-function value count.
    static constexpr IRValueID kGlobalIdBase = 0x40000000u;
    IRValueID nextGlobalId = kGlobalIdBase;

    IRModule() = default;
    explicit IRModule(const std::string& n) : name(n) {}

    IRFunction* createFunction(const std::string& name);
    IRFunction* getFunction(const std::string& name);

    IRValueID allocateGlobalId() { return nextGlobalId++; }

    void addGlobal(const IRGlobal& global) { globals.push_back(global); }
    IRGlobal* findGlobal(const std::string& name);

    std::string toString() const;
};

// ============================================================================
// IR Utilities
// ============================================================================

namespace IRUtils
{
    // Encode swizzle pattern (e.g., "xyz" -> 0x210 for indices 0,1,2)
    int encodeSwizzle(const std::string& swizzle);

    // Decode swizzle pattern to string
    std::string decodeSwizzle(int mask, int count);

    // Check if instruction is a terminator
    bool isTerminator(IROp op);

    // Check if instruction has side effects
    bool hasSideEffects(IROp op);

    // Get number of operands for an operation
    int getOperandCount(IROp op);

    // Round float to 16-bit half precision (binary16, ties round toward +infinity to match Cg hardware target semantics)
    inline float roundToHalf(float f)
    {
        if (std::isnan(f) || std::isinf(f)) return f;

        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        uint32_t sign = u & 0x80000000u;
        int32_t exp = static_cast<int32_t>((u >> 23) & 0xFFu) - 127 + 15;
        uint32_t mant = u & 0x7FFFFFu;

        uint16_t h;
        if (exp >= 31)
        {
            h = static_cast<uint16_t>((sign >> 16) | 0x7C00u);
        }
        else if (exp <= 0)
        {
            if (exp < -10)
            {
                h = static_cast<uint16_t>(sign >> 16);
            }
            else
            {
                int32_t shift = 1 - exp;
                uint32_t val = mant | 0x800000u;
                int32_t totalShift = shift + 13;
                uint32_t halfUlp = 1u << (totalShift - 1);
                uint32_t rem = val & ((1u << totalShift) - 1u);
                bool roundUp = (rem > halfUlp) || (rem == halfUlp && sign == 0);
                uint32_t kept = val >> totalShift;
                if (roundUp)
                    kept++;
                if (kept > 0x3FFu)
                    h = static_cast<uint16_t>((sign >> 16) | (1u << 10) | (kept & 0x3FFu));
                else
                    h = static_cast<uint16_t>((sign >> 16) | kept);
            }
        }
        else
        {
            uint32_t rem = mant & 0x1FFFu;
            // Round ties toward +infinity:
            // For positive (sign == 0), tie rounds to larger magnitude (roundUp = true).
            // For negative (sign != 0), tie rounds toward zero / +infinity (roundUp = false).
            bool roundUp = (rem > 0x1000u) || (rem == 0x1000u && sign == 0);
            if (roundUp)
            {
                mant += 0x2000u - rem;
                if (mant & 0x800000u)
                {
                    mant = 0;
                    exp++;
                    if (exp >= 31)
                        h = static_cast<uint16_t>((sign >> 16) | 0x7C00u);
                    else
                        h = static_cast<uint16_t>((sign >> 16) | (exp << 10) | (mant >> 13));
                }
                else
                {
                    h = static_cast<uint16_t>((sign >> 16) | (exp << 10) | (mant >> 13));
                }
            }
            else
            {
                h = static_cast<uint16_t>((sign >> 16) | (exp << 10) | (mant >> 13));
            }
        }

        uint32_t h_sign = (h & 0x8000u) << 16;
        uint32_t h_exp = (h >> 10) & 0x1Fu;
        uint32_t h_mant = h & 0x3FFu;
        uint32_t u_out;
        if (h_exp == 31)
        {
            u_out = h_sign | 0x7F800000u | (h_mant << 13);
        }
        else if (h_exp == 0)
        {
            if (h_mant == 0)
            {
                u_out = h_sign;
            }
            else
            {
                while ((h_mant & 0x400u) == 0)
                {
                    h_mant <<= 1;
                    h_exp--;
                }
                h_exp++;
                h_mant &= 0x3FFu;
                u_out = h_sign | ((h_exp + 127 - 15) << 23) | (h_mant << 13);
            }
        }
        else
        {
            u_out = h_sign | ((h_exp + 127 - 15) << 23) | (h_mant << 13);
        }

        float res;
        std::memcpy(&res, &u_out, sizeof(res));
        return res;
    }
}
