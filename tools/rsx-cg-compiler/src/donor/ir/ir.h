#pragma once

#include "types.h"
#include "ast.h"
#include "semantic.h"  // For ShaderStage
#include <cstdint>
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
    Sampler2D,
    SamplerCube,

    // Pointer type (for memory operations)
    Ptr
};

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
    Lerp,
    Step,
    SmoothStep,

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
    IROp predOp = IROp::Nop;         // For PredCarry: the inner op (Add/Mul/Mad/...)
    std::string targetName;          // For branch targets, function calls
    int semanticIndex = 0;           // For shader I/O operations
    std::string semanticName;        // For shader I/O operations (digit-stripped)
    std::string rawSemanticName;     // For shader I/O operations (source spelling, e.g. "POSITION0")
    // Struct-flattened entry params:
    //   LoadAttribute  for `input.pos` carries structParamName="input", fieldName="pos"
    //   StoreOutput    for `o.pos = ...` carries fieldName="pos" (parent struct name is
    //                  implicit — sce-cgc names outputs `<entry-func>.<field>`,
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
    IRConstant* createConstant(const IRTypeInfo& type, const std::vector<float>& value);

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

    // Global value ID allocation
    IRValueID nextGlobalId = 1;

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
}
