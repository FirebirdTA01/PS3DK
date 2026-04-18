#include "ir.h"
#include <sstream>
#include <iomanip>

// ============================================================================
// IRTypeInfo Implementation
// ============================================================================

std::string IRTypeInfo::toString() const
{
    std::string result;

    switch (baseType)
    {
    case IRType::Void:      result = "void"; break;
    case IRType::Bool:      result = "bool"; break;
    case IRType::Int32:     result = "int"; break;
    case IRType::UInt32:    result = "uint"; break;
    case IRType::Float32:   result = "float"; break;
    case IRType::Float16:   result = "half"; break;
    case IRType::Vec2:      result = "vec2"; break;
    case IRType::Vec3:      result = "vec3"; break;
    case IRType::Vec4:      result = "vec4"; break;
    case IRType::Mat2x2:    result = "mat2"; break;
    case IRType::Mat3x3:    result = "mat3"; break;
    case IRType::Mat4x4:    result = "mat4"; break;
    case IRType::Sampler2D: result = "sampler2D"; break;
    case IRType::SamplerCube: result = "samplerCube"; break;
    case IRType::Ptr:       result = "ptr"; break;
    default:                result = "unknown"; break;
    }

    if (isVector() && vectorSize > 1)
    {
        // Add element type prefix
        switch (elementType)
        {
        case IRType::Int32:   result = "ivec" + std::to_string(vectorSize); break;
        case IRType::UInt32:  result = "uvec" + std::to_string(vectorSize); break;
        case IRType::Bool:    result = "bvec" + std::to_string(vectorSize); break;
        case IRType::Float16: result = "hvec" + std::to_string(vectorSize); break;
        default: result = "vec" + std::to_string(vectorSize); break;
        }
    }

    return result;
}

IRTypeInfo IRTypeInfo::fromCgType(const CgType& cgType)
{
    IRTypeInfo info;

    if (cgType.isVoid())
    {
        info.baseType = IRType::Void;
        return info;
    }

    if (cgType.isError())
    {
        info.baseType = IRType::Void;
        return info;
    }

    // Determine element type
    if (cgType.isNumeric())
    {
        switch (cgType.scalarKind())
        {
        case ScalarKind::Bool:
            info.elementType = IRType::Bool;
            break;
        case ScalarKind::Int:
        case ScalarKind::Char:
        case ScalarKind::Short:
            info.elementType = IRType::Int32;
            break;
        case ScalarKind::UInt:
        case ScalarKind::UChar:
        case ScalarKind::UShort:
            info.elementType = IRType::UInt32;
            break;
        case ScalarKind::Float:
        case ScalarKind::Fixed:
            info.elementType = IRType::Float32;
            break;
        case ScalarKind::Half:
            info.elementType = IRType::Float16;
            break;
        default:
            info.elementType = IRType::Float32;
            break;
        }
    }

    if (cgType.isScalar())
    {
        info.vectorSize = 1;
        switch (info.elementType)
        {
        case IRType::Bool:    info.baseType = IRType::Bool; break;
        case IRType::Int32:   info.baseType = IRType::Int32; break;
        case IRType::UInt32:  info.baseType = IRType::UInt32; break;
        case IRType::Float32: info.baseType = IRType::Float32; break;
        case IRType::Float16: info.baseType = IRType::Float16; break;
        default:              info.baseType = IRType::Float32; break;
        }
    }
    else if (cgType.isVector())
    {
        info.vectorSize = cgType.vectorSize();
        switch (info.vectorSize)
        {
        case 2: info.baseType = IRType::Vec2; break;
        case 3: info.baseType = IRType::Vec3; break;
        case 4: info.baseType = IRType::Vec4; break;
        default: info.baseType = IRType::Vec4; break;
        }
    }
    else if (cgType.isMatrix())
    {
        info.matrixRows = cgType.matrixRows();
        info.matrixCols = cgType.matrixCols();
        if (info.matrixRows == 4 && info.matrixCols == 4)
            info.baseType = IRType::Mat4x4;
        else if (info.matrixRows == 3 && info.matrixCols == 3)
            info.baseType = IRType::Mat3x3;
        else if (info.matrixRows == 2 && info.matrixCols == 2)
            info.baseType = IRType::Mat2x2;
        else
            info.baseType = IRType::Mat4x4;  // Default
    }
    else if (cgType.isSampler())
    {
        // Check sampler type from string representation
        std::string typeStr = cgType.toString();
        if (typeStr.find("CUBE") != std::string::npos ||
            typeStr.find("Cube") != std::string::npos)
        {
            info.baseType = IRType::SamplerCube;
        }
        else
        {
            info.baseType = IRType::Sampler2D;
        }
    }

    // Preserve array size and set element type for arrays
    if (cgType.arraySize() > 0)
    {
        info.arraySize = cgType.arraySize();

        // For array types, baseType may not have been set above because
        // CgType::isScalar()/isVector() return false for arrays.
        // Set baseType based on the element type.
        if (info.baseType == IRType::Void && info.elementType != IRType::Float32)
        {
            // Use whatever elementType was determined
        }
        if (info.baseType == IRType::Void)
        {
            // Array of scalars: use the element type as base type
            info.baseType = info.elementType;
            info.vectorSize = 1;
        }
    }

    return info;
}

// ============================================================================
// IROp String Conversion
// ============================================================================

const char* irOpToString(IROp op)
{
    switch (op)
    {
    case IROp::Const:        return "const";
    case IROp::Undef:        return "undef";
    case IROp::Add:          return "add";
    case IROp::Sub:          return "sub";
    case IROp::Mul:          return "mul";
    case IROp::Div:          return "div";
    case IROp::Mod:          return "mod";
    case IROp::Neg:          return "neg";
    case IROp::Mad:          return "mad";
    case IROp::And:          return "and";
    case IROp::Or:           return "or";
    case IROp::Xor:          return "xor";
    case IROp::Not:          return "not";
    case IROp::Shl:          return "shl";
    case IROp::Shr:          return "shr";
    case IROp::UShr:         return "ushr";
    case IROp::CmpEq:        return "cmpeq";
    case IROp::CmpNe:        return "cmpne";
    case IROp::CmpLt:        return "cmplt";
    case IROp::CmpLe:        return "cmple";
    case IROp::CmpGt:        return "cmpgt";
    case IROp::CmpGe:        return "cmpge";
    case IROp::LogicalAnd:   return "land";
    case IROp::LogicalOr:    return "lor";
    case IROp::LogicalNot:   return "lnot";
    case IROp::IntToFloat:   return "itof";
    case IROp::FloatToInt:   return "ftoi";
    case IROp::FloatToHalf:  return "ftoh";
    case IROp::HalfToFloat:  return "htof";
    case IROp::Bitcast:      return "bitcast";
    case IROp::VecConstruct: return "vec";
    case IROp::VecExtract:   return "extract";
    case IROp::VecInsert:    return "insert";
    case IROp::VecShuffle:   return "shuffle";
    case IROp::Abs:          return "abs";
    case IROp::Sign:         return "sign";
    case IROp::Floor:        return "floor";
    case IROp::Ceil:         return "ceil";
    case IROp::Frac:         return "frac";
    case IROp::Round:        return "round";
    case IROp::Trunc:        return "trunc";
    case IROp::Min:          return "min";
    case IROp::Max:          return "max";
    case IROp::Clamp:        return "clamp";
    case IROp::Saturate:     return "sat";
    case IROp::Lerp:         return "lerp";
    case IROp::Step:         return "step";
    case IROp::SmoothStep:   return "smoothstep";
    case IROp::Sin:          return "sin";
    case IROp::Cos:          return "cos";
    case IROp::Tan:          return "tan";
    case IROp::Asin:         return "asin";
    case IROp::Acos:         return "acos";
    case IROp::Atan:         return "atan";
    case IROp::Atan2:        return "atan2";
    case IROp::Pow:          return "pow";
    case IROp::Exp:          return "exp";
    case IROp::Exp2:         return "exp2";
    case IROp::Log:          return "log";
    case IROp::Log2:         return "log2";
    case IROp::Sqrt:         return "sqrt";
    case IROp::RSqrt:        return "rsqrt";
    case IROp::Dot:          return "dot";
    case IROp::Cross:        return "cross";
    case IROp::Length:       return "length";
    case IROp::Distance:     return "distance";
    case IROp::Normalize:    return "normalize";
    case IROp::Reflect:      return "reflect";
    case IROp::Refract:      return "refract";
    case IROp::FaceForward:  return "faceforward";
    case IROp::MatMul:       return "matmul";
    case IROp::MatVecMul:    return "matvecmul";
    case IROp::VecMatMul:    return "vecmatmul";
    case IROp::Transpose:    return "transpose";
    case IROp::MatConstruct: return "matconstruct";
    case IROp::Phi:          return "phi";
    case IROp::Select:       return "select";
    case IROp::Branch:       return "br";
    case IROp::CondBranch:   return "brc";
    case IROp::Return:       return "ret";
    case IROp::Discard:      return "discard";
    case IROp::Load:         return "load";
    case IROp::Store:        return "store";
    case IROp::LoadAttribute: return "ldattr";
    case IROp::LoadUniform:  return "ldunif";
    case IROp::LoadVarying:  return "ldvary";
    case IROp::StoreOutput:  return "stout";
    case IROp::StoreVarying: return "stvary";
    case IROp::TexSample:    return "sample";
    case IROp::TexSampleLod: return "samplelod";
    case IROp::TexSampleGrad: return "samplegrad";
    case IROp::TexSampleProj: return "sampleproj";
    case IROp::TexFetch:     return "texfetch";
    case IROp::Call:         return "call";
    case IROp::Nop:          return "nop";
    case IROp::Comment:      return "// ";
    default:                 return "???";
    }
}

// ============================================================================
// IRValue Implementation
// ============================================================================

std::string IRValue::toString() const
{
    std::ostringstream ss;
    if (!name.empty())
    {
        ss << "%" << name;
    }
    else
    {
        ss << "%" << id;
    }
    return ss.str();
}

std::string IRConstant::valueToString() const
{
    std::ostringstream ss;

    if (std::holds_alternative<bool>(value))
    {
        ss << (std::get<bool>(value) ? "true" : "false");
    }
    else if (std::holds_alternative<int32_t>(value))
    {
        ss << std::get<int32_t>(value);
    }
    else if (std::holds_alternative<uint32_t>(value))
    {
        ss << std::get<uint32_t>(value) << "u";
    }
    else if (std::holds_alternative<float>(value))
    {
        ss << std::fixed << std::setprecision(6) << std::get<float>(value) << "f";
    }
    else if (std::holds_alternative<std::vector<float>>(value))
    {
        const auto& vec = std::get<std::vector<float>>(value);
        ss << "<";
        for (size_t i = 0; i < vec.size(); ++i)
        {
            if (i > 0) ss << ", ";
            ss << std::fixed << std::setprecision(6) << vec[i] << "f";
        }
        ss << ">";
    }

    return ss.str();
}

// ============================================================================
// IRInstruction Implementation
// ============================================================================

std::string IRInstruction::toString() const
{
    std::ostringstream ss;

    // Result assignment
    if (result != InvalidIRValue)
    {
        ss << "%" << result << " = ";
    }

    // Operation
    ss << irOpToString(op);

    // Type (for typed operations)
    if (resultType.baseType != IRType::Void)
    {
        ss << " " << resultType.toString();
    }

    // Operands
    for (size_t i = 0; i < operands.size(); ++i)
    {
        ss << (i == 0 ? " " : ", ") << "%" << operands[i];
    }

    // Additional info
    if (op == IROp::VecShuffle && swizzleMask != 0)
    {
        ss << " [" << IRUtils::decodeSwizzle(swizzleMask, resultType.vectorSize) << "]";
    }
    else if (op == IROp::VecExtract || op == IROp::VecInsert)
    {
        ss << " [" << componentIndex << "]";
    }
    else if (op == IROp::Branch || op == IROp::CondBranch)
    {
        ss << " -> " << targetName;
    }
    else if (op == IROp::Call)
    {
        ss << " @" << targetName;
    }
    else if (op == IROp::LoadAttribute || op == IROp::StoreOutput ||
             op == IROp::LoadVarying || op == IROp::StoreVarying)
    {
        ss << " " << semanticName;
        if (semanticIndex > 0)
        {
            ss << semanticIndex;
        }
    }

    return ss.str();
}

// ============================================================================
// IRBasicBlock Implementation
// ============================================================================

IRInstruction* IRBasicBlock::addInstruction(std::unique_ptr<IRInstruction> inst)
{
    inst->parentBlock = this;
    instructions.push_back(std::move(inst));
    return instructions.back().get();
}

bool IRBasicBlock::hasTerminator() const
{
    if (instructions.empty()) return false;
    return IRUtils::isTerminator(instructions.back()->op);
}

IRInstruction* IRBasicBlock::getTerminator() const
{
    if (!hasTerminator()) return nullptr;
    return instructions.back().get();
}

std::string IRBasicBlock::toString() const
{
    std::ostringstream ss;
    ss << name << ":\n";

    for (const auto& inst : instructions)
    {
        ss << "  " << inst->toString() << "\n";
    }

    return ss.str();
}

// ============================================================================
// IRFunction Implementation
// ============================================================================

IRBasicBlock* IRFunction::createBlock(const std::string& blockName)
{
    auto block = std::make_unique<IRBasicBlock>(blockName);
    block->parentFunction = this;
    blocks.push_back(std::move(block));

    if (!entryBlock)
    {
        entryBlock = blocks.back().get();
    }

    return blocks.back().get();
}

IRBasicBlock* IRFunction::getBlock(const std::string& blockName)
{
    for (auto& block : blocks)
    {
        if (block->name == blockName)
        {
            return block.get();
        }
    }
    return nullptr;
}

IRValue* IRFunction::getValue(IRValueID id) const
{
    auto it = values.find(id);
    if (it != values.end())
    {
        return it->second.get();
    }
    return nullptr;
}

IRConstant* IRFunction::createConstant(const IRTypeInfo& type, bool value)
{
    IRValueID id = allocateValueId();
    auto constant = std::make_unique<IRConstant>(id, type, value);
    IRConstant* ptr = constant.get();
    values[id] = std::move(constant);
    return ptr;
}

IRConstant* IRFunction::createConstant(const IRTypeInfo& type, int32_t value)
{
    IRValueID id = allocateValueId();
    auto constant = std::make_unique<IRConstant>(id, type, value);
    IRConstant* ptr = constant.get();
    values[id] = std::move(constant);
    return ptr;
}

IRConstant* IRFunction::createConstant(const IRTypeInfo& type, uint32_t value)
{
    IRValueID id = allocateValueId();
    auto constant = std::make_unique<IRConstant>(id, type, value);
    IRConstant* ptr = constant.get();
    values[id] = std::move(constant);
    return ptr;
}

IRConstant* IRFunction::createConstant(const IRTypeInfo& type, float value)
{
    IRValueID id = allocateValueId();
    auto constant = std::make_unique<IRConstant>(id, type, value);
    IRConstant* ptr = constant.get();
    values[id] = std::move(constant);
    return ptr;
}

IRConstant* IRFunction::createConstant(const IRTypeInfo& type, const std::vector<float>& value)
{
    IRValueID id = allocateValueId();
    auto constant = std::make_unique<IRConstant>(id, type, value);
    IRConstant* ptr = constant.get();
    values[id] = std::move(constant);
    return ptr;
}

std::string IRFunction::toString() const
{
    std::ostringstream ss;

    ss << "define " << returnType.toString() << " @" << name << "(";

    for (size_t i = 0; i < parameters.size(); ++i)
    {
        if (i > 0) ss << ", ";
        ss << parameters[i].type.toString() << " %" << parameters[i].name;
        if (!parameters[i].semanticName.empty())
        {
            ss << " : " << parameters[i].semanticName;
            if (parameters[i].semanticIndex > 0)
            {
                ss << parameters[i].semanticIndex;
            }
        }
    }

    ss << ")";
    if (isEntryPoint)
    {
        ss << " entry";
    }
    ss << " {\n";

    for (const auto& block : blocks)
    {
        ss << block->toString();
    }

    ss << "}\n";

    return ss.str();
}

// ============================================================================
// IRModule Implementation
// ============================================================================

IRFunction* IRModule::createFunction(const std::string& funcName)
{
    auto func = std::make_unique<IRFunction>(funcName);
    func->parentModule = this;
    functions.push_back(std::move(func));
    return functions.back().get();
}

IRFunction* IRModule::getFunction(const std::string& funcName)
{
    for (auto& func : functions)
    {
        if (func->name == funcName)
        {
            return func.get();
        }
    }
    return nullptr;
}

IRGlobal* IRModule::findGlobal(const std::string& globalName)
{
    for (auto& global : globals)
    {
        if (global.name == globalName)
        {
            return &global;
        }
    }
    return nullptr;
}

std::string IRModule::toString() const
{
    std::ostringstream ss;

    ss << "; Module: " << name << "\n";
    ss << "; Stage: " << (shaderStage == ShaderStage::Vertex ? "vertex" : "fragment") << "\n";
    ss << "; Entry: " << entryPointName << "\n\n";

    // Globals
    if (!globals.empty())
    {
        ss << "; Globals\n";
        for (const auto& global : globals)
        {
            ss << "@" << global.name << " = " << global.type.toString();
            if (global.storage == StorageQualifier::Uniform)
            {
                ss << " uniform";
            }
            if (!global.semanticName.empty())
            {
                ss << " : " << global.semanticName;
                if (global.semanticIndex > 0)
                {
                    ss << global.semanticIndex;
                }
            }
            ss << "\n";
        }
        ss << "\n";
    }

    // Functions
    for (const auto& func : functions)
    {
        ss << func->toString() << "\n";
    }

    return ss.str();
}

// ============================================================================
// IR Utilities
// ============================================================================

namespace IRUtils
{

int encodeSwizzle(const std::string& swizzle)
{
    int mask = 0;
    for (size_t i = 0; i < swizzle.size() && i < 4; ++i)
    {
        int idx = 0;
        char c = swizzle[i];
        switch (c)
        {
        case 'x': case 'r': case 's': idx = 0; break;
        case 'y': case 'g': case 't': idx = 1; break;
        case 'z': case 'b': case 'p': idx = 2; break;
        case 'w': case 'a': case 'q': idx = 3; break;
        default: idx = 0; break;
        }
        mask |= (idx << (i * 2));
    }
    return mask;
}

std::string decodeSwizzle(int mask, int count)
{
    const char components[] = "xyzw";
    std::string result;
    for (int i = 0; i < count && i < 4; ++i)
    {
        int idx = (mask >> (i * 2)) & 3;
        result += components[idx];
    }
    return result;
}

bool isTerminator(IROp op)
{
    switch (op)
    {
    case IROp::Branch:
    case IROp::CondBranch:
    case IROp::Return:
    case IROp::Discard:
        return true;
    default:
        return false;
    }
}

bool hasSideEffects(IROp op)
{
    switch (op)
    {
    case IROp::Store:
    case IROp::StoreOutput:
    case IROp::StoreVarying:
    case IROp::Branch:
    case IROp::CondBranch:
    case IROp::Return:
    case IROp::Discard:
    case IROp::Call:
        return true;
    default:
        return false;
    }
}

int getOperandCount(IROp op)
{
    switch (op)
    {
    // No operands
    case IROp::Const:
    case IROp::Undef:
    case IROp::Nop:
    case IROp::Comment:
        return 0;

    // Unary
    case IROp::Neg:
    case IROp::Not:
    case IROp::LogicalNot:
    case IROp::IntToFloat:
    case IROp::FloatToInt:
    case IROp::FloatToHalf:
    case IROp::HalfToFloat:
    case IROp::Bitcast:
    case IROp::Abs:
    case IROp::Sign:
    case IROp::Floor:
    case IROp::Ceil:
    case IROp::Frac:
    case IROp::Round:
    case IROp::Trunc:
    case IROp::Saturate:
    case IROp::Sin:
    case IROp::Cos:
    case IROp::Tan:
    case IROp::Asin:
    case IROp::Acos:
    case IROp::Atan:
    case IROp::Exp:
    case IROp::Exp2:
    case IROp::Log:
    case IROp::Log2:
    case IROp::Sqrt:
    case IROp::RSqrt:
    case IROp::Length:
    case IROp::Normalize:
    case IROp::Transpose:
    case IROp::Return:
    case IROp::VecExtract:
    case IROp::Load:
    case IROp::LoadAttribute:
    case IROp::LoadUniform:
    case IROp::LoadVarying:
        return 1;

    // Binary
    case IROp::Add:
    case IROp::Sub:
    case IROp::Mul:
    case IROp::Div:
    case IROp::Mod:
    case IROp::And:
    case IROp::Or:
    case IROp::Xor:
    case IROp::Shl:
    case IROp::Shr:
    case IROp::UShr:
    case IROp::CmpEq:
    case IROp::CmpNe:
    case IROp::CmpLt:
    case IROp::CmpLe:
    case IROp::CmpGt:
    case IROp::CmpGe:
    case IROp::LogicalAnd:
    case IROp::LogicalOr:
    case IROp::Min:
    case IROp::Max:
    case IROp::Step:
    case IROp::Atan2:
    case IROp::Pow:
    case IROp::Dot:
    case IROp::Cross:
    case IROp::Distance:
    case IROp::Reflect:
    case IROp::MatMul:
    case IROp::MatVecMul:
    case IROp::VecMatMul:
    case IROp::Store:
    case IROp::StoreOutput:
    case IROp::StoreVarying:
    case IROp::TexSample:
        return 2;

    // Ternary
    case IROp::Select:
    case IROp::Clamp:
    case IROp::Lerp:
    case IROp::SmoothStep:
    case IROp::Refract:
    case IROp::FaceForward:
    case IROp::VecInsert:
    case IROp::CondBranch:
    case IROp::TexSampleLod:
    case IROp::TexSampleProj:
        return 3;

    // Variable
    case IROp::VecConstruct:
    case IROp::VecShuffle:
    case IROp::Phi:
    case IROp::Call:
    case IROp::TexSampleGrad:
    case IROp::TexFetch:
        return -1;  // Variable number of operands

    default:
        return 0;
    }
}

} // namespace IRUtils
