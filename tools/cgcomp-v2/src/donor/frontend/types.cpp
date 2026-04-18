#include "types.h"
#include <algorithm>
#include <unordered_map>

// ============================================================================
// CgType Implementation
// ============================================================================

CgType::CgType() : node(TypeNode::makeVoid()), errorType(false) {}

CgType::CgType(std::shared_ptr<TypeNode> n) : node(n), errorType(false) {}

// Static factory methods
CgType CgType::Void()
{
    return CgType(TypeNode::makeVoid());
}

CgType CgType::Error()
{
    CgType t;
    t.errorType = true;
    return t;
}

std::shared_ptr<TypeNode> CgType::makeScalarNode(BaseType bt)
{
    auto node = std::make_shared<TypeNode>();
    node->baseType = bt;
    node->vectorSize = 1;
    node->matrixRows = 0;
    node->matrixCols = 0;
    node->arraySize = 0;
    return node;
}

std::shared_ptr<TypeNode> CgType::makeVectorNode(BaseType bt, int size)
{
    auto node = std::make_shared<TypeNode>();
    node->baseType = bt;
    node->vectorSize = size;
    node->matrixRows = 0;
    node->matrixCols = 0;
    node->arraySize = 0;
    return node;
}

std::shared_ptr<TypeNode> CgType::makeMatrixNode(BaseType bt, int rows, int cols)
{
    auto node = std::make_shared<TypeNode>();
    node->baseType = bt;
    node->vectorSize = 1;
    node->matrixRows = rows;
    node->matrixCols = cols;
    node->arraySize = 0;
    return node;
}

CgType CgType::Bool() { return CgType(makeScalarNode(BaseType::Bool)); }
CgType CgType::Int() { return CgType(makeScalarNode(BaseType::Int)); }
CgType CgType::UInt() { return CgType(makeScalarNode(BaseType::UInt)); }
CgType CgType::Float() { return CgType(makeScalarNode(BaseType::Float)); }
CgType CgType::Half() { return CgType(makeScalarNode(BaseType::Half)); }
CgType CgType::Fixed() { return CgType(makeScalarNode(BaseType::Fixed)); }
CgType CgType::Char() { return CgType(makeScalarNode(BaseType::Char)); }
CgType CgType::UChar() { return CgType(makeScalarNode(BaseType::UChar)); }
CgType CgType::Short() { return CgType(makeScalarNode(BaseType::Short)); }
CgType CgType::UShort() { return CgType(makeScalarNode(BaseType::UShort)); }

// Scalar factory from ScalarKind
CgType CgType::Scalar(ScalarKind kind)
{
    switch (kind)
    {
    case ScalarKind::Bool:   return Bool();
    case ScalarKind::Int:    return Int();
    case ScalarKind::UInt:   return UInt();
    case ScalarKind::Float:  return Float();
    case ScalarKind::Half:   return Half();
    case ScalarKind::Fixed:  return Fixed();
    case ScalarKind::Char:   return Char();
    case ScalarKind::UChar:  return UChar();
    case ScalarKind::Short:  return Short();
    case ScalarKind::UShort: return UShort();
    default: return Float();
    }
}

// Vector factory methods
CgType CgType::Vec(ScalarKind scalar, int size)
{
    BaseType bt;
    switch (scalar)
    {
    case ScalarKind::Bool:   bt = BaseType::Bool; break;
    case ScalarKind::Int:    bt = BaseType::Int; break;
    case ScalarKind::UInt:   bt = BaseType::UInt; break;
    case ScalarKind::Float:  bt = BaseType::Float; break;
    case ScalarKind::Half:   bt = BaseType::Half; break;
    case ScalarKind::Fixed:  bt = BaseType::Fixed; break;
    case ScalarKind::Char:   bt = BaseType::Char; break;
    case ScalarKind::UChar:  bt = BaseType::UChar; break;
    case ScalarKind::Short:  bt = BaseType::Short; break;
    case ScalarKind::UShort: bt = BaseType::UShort; break;
    default: bt = BaseType::Float; break;
    }
    return CgType(makeVectorNode(bt, size));
}

CgType CgType::Float2() { return Vec(ScalarKind::Float, 2); }
CgType CgType::Float3() { return Vec(ScalarKind::Float, 3); }
CgType CgType::Float4() { return Vec(ScalarKind::Float, 4); }
CgType CgType::Int2() { return Vec(ScalarKind::Int, 2); }
CgType CgType::Int3() { return Vec(ScalarKind::Int, 3); }
CgType CgType::Int4() { return Vec(ScalarKind::Int, 4); }
CgType CgType::Half2() { return Vec(ScalarKind::Half, 2); }
CgType CgType::Half3() { return Vec(ScalarKind::Half, 3); }
CgType CgType::Half4() { return Vec(ScalarKind::Half, 4); }
CgType CgType::Bool2() { return Vec(ScalarKind::Bool, 2); }
CgType CgType::Bool3() { return Vec(ScalarKind::Bool, 3); }
CgType CgType::Bool4() { return Vec(ScalarKind::Bool, 4); }

// Matrix factory methods
CgType CgType::Mat(ScalarKind scalar, int rows, int cols)
{
    BaseType bt;
    switch (scalar)
    {
    case ScalarKind::Float: bt = BaseType::Float; break;
    case ScalarKind::Half:  bt = BaseType::Half; break;
    case ScalarKind::Fixed: bt = BaseType::Fixed; break;
    default: bt = BaseType::Float; break;
    }
    return CgType(makeMatrixNode(bt, rows, cols));
}

CgType CgType::Float2x2() { return Mat(ScalarKind::Float, 2, 2); }
CgType CgType::Float3x3() { return Mat(ScalarKind::Float, 3, 3); }
CgType CgType::Float4x4() { return Mat(ScalarKind::Float, 4, 4); }

// Sampler factory methods
CgType CgType::Sampler1D() { return CgType(makeScalarNode(BaseType::Sampler1D)); }
CgType CgType::Sampler2D() { return CgType(makeScalarNode(BaseType::Sampler2D)); }
CgType CgType::Sampler3D() { return CgType(makeScalarNode(BaseType::Sampler3D)); }
CgType CgType::SamplerCube() { return CgType(makeScalarNode(BaseType::SamplerCube)); }
CgType CgType::ISampler2D() { return CgType(makeScalarNode(BaseType::ISampler2D)); }
CgType CgType::USampler2D() { return CgType(makeScalarNode(BaseType::USampler2D)); }

// Array factory
CgType CgType::Array(const CgType& elementType, int size)
{
    auto node = std::make_shared<TypeNode>();
    node->baseType = BaseType::Array;
    node->arraySize = size;
    node->elementType = elementType.getNode();
    return CgType(node);
}

// Struct factory
CgType CgType::Struct(const std::string& name, const std::vector<StructField>& fields)
{
    auto node = std::make_shared<TypeNode>();
    node->baseType = BaseType::Struct;
    node->structName = name;
    node->structFields = fields;
    return CgType(node);
}

// Type queries
TypeCategory CgType::category() const
{
    if (errorType) return TypeCategory::Error;
    if (!node) return TypeCategory::Error;

    if (node->baseType == BaseType::Void) return TypeCategory::Void;
    if (node->isSampler()) return TypeCategory::Sampler;
    if (node->baseType == BaseType::Struct) return TypeCategory::Struct;
    if (node->arraySize > 0) return TypeCategory::Array;
    if (node->matrixRows > 0) return TypeCategory::Matrix;
    if (node->vectorSize > 1) return TypeCategory::Vector;
    return TypeCategory::Scalar;
}

bool CgType::isVoid() const { return category() == TypeCategory::Void; }
bool CgType::isError() const { return category() == TypeCategory::Error; }
bool CgType::isScalar() const { return category() == TypeCategory::Scalar; }
bool CgType::isVector() const { return category() == TypeCategory::Vector; }
bool CgType::isMatrix() const { return category() == TypeCategory::Matrix; }
bool CgType::isSampler() const { return category() == TypeCategory::Sampler; }
bool CgType::isStruct() const { return category() == TypeCategory::Struct; }
bool CgType::isArray() const { return category() == TypeCategory::Array; }

bool CgType::isNumeric() const
{
    if (!node) return false;
    switch (node->baseType)
    {
    case BaseType::Bool:
    case BaseType::Int:
    case BaseType::UInt:
    case BaseType::Float:
    case BaseType::Half:
    case BaseType::Fixed:
    case BaseType::Char:
    case BaseType::UChar:
    case BaseType::Short:
    case BaseType::UShort:
        return true;
    default:
        return false;
    }
}

bool CgType::isIntegral() const
{
    if (!node) return false;
    switch (node->baseType)
    {
    case BaseType::Int:
    case BaseType::UInt:
    case BaseType::Char:
    case BaseType::UChar:
    case BaseType::Short:
    case BaseType::UShort:
        return true;
    default:
        return false;
    }
}

bool CgType::isFloatingPoint() const
{
    if (!node) return false;
    switch (node->baseType)
    {
    case BaseType::Float:
    case BaseType::Half:
    case BaseType::Fixed:
        return true;
    default:
        return false;
    }
}

bool CgType::isSigned() const
{
    if (!node) return false;
    switch (node->baseType)
    {
    case BaseType::Int:
    case BaseType::Char:
    case BaseType::Short:
    case BaseType::Float:
    case BaseType::Half:
    case BaseType::Fixed:
        return true;
    default:
        return false;
    }
}

bool CgType::isUnsigned() const
{
    if (!node) return false;
    switch (node->baseType)
    {
    case BaseType::Bool:
    case BaseType::UInt:
    case BaseType::UChar:
    case BaseType::UShort:
        return true;
    default:
        return false;
    }
}

int CgType::vectorSize() const
{
    if (!node) return 0;
    return node->vectorSize;
}

int CgType::matrixRows() const
{
    if (!node) return 0;
    return node->matrixRows;
}

int CgType::matrixCols() const
{
    if (!node) return 0;
    return node->matrixCols;
}

int CgType::componentCount() const
{
    if (!node) return 0;
    if (node->matrixRows > 0)
        return node->matrixRows * node->matrixCols;
    return node->vectorSize;
}

int CgType::arraySize() const
{
    if (!node) return 0;
    return node->arraySize;
}

ScalarKind CgType::scalarKind() const
{
    if (!node) return ScalarKind::Float;
    switch (node->baseType)
    {
    case BaseType::Bool:   return ScalarKind::Bool;
    case BaseType::Int:    return ScalarKind::Int;
    case BaseType::UInt:   return ScalarKind::UInt;
    case BaseType::Float:  return ScalarKind::Float;
    case BaseType::Half:   return ScalarKind::Half;
    case BaseType::Fixed:  return ScalarKind::Fixed;
    case BaseType::Char:   return ScalarKind::Char;
    case BaseType::UChar:  return ScalarKind::UChar;
    case BaseType::Short:  return ScalarKind::Short;
    case BaseType::UShort: return ScalarKind::UShort;
    default:               return ScalarKind::Float;
    }
}

const std::string& CgType::structName() const
{
    static const std::string empty;
    if (!node) return empty;
    return node->structName;
}

const std::vector<StructField>& CgType::structFields() const
{
    static const std::vector<StructField> empty;
    if (!node) return empty;
    return node->structFields;
}

std::optional<CgType> CgType::getFieldType(const std::string& fieldName) const
{
    if (!node || node->baseType != BaseType::Struct) return std::nullopt;

    for (const auto& field : node->structFields)
    {
        if (field.name == fieldName)
        {
            return CgType(field.type);
        }
    }
    return std::nullopt;
}

CgType CgType::elementType() const
{
    if (!node || !node->elementType) return Error();
    return CgType(node->elementType);
}

CgType CgType::swizzleType(int resultSize) const
{
    if (!isVector() && !isScalar()) return Error();
    if (resultSize < 1 || resultSize > 4) return Error();

    if (resultSize == 1)
    {
        // Single component swizzle returns scalar
        return CgType(makeScalarNode(node->baseType));
    }
    else
    {
        // Multi-component swizzle returns vector
        return CgType(makeVectorNode(node->baseType, resultSize));
    }
}

bool CgType::equals(const CgType& other) const
{
    if (errorType || other.errorType) return false;
    if (!node || !other.node) return false;

    if (node->baseType != other.node->baseType) return false;
    if (node->vectorSize != other.node->vectorSize) return false;
    if (node->matrixRows != other.node->matrixRows) return false;
    if (node->matrixCols != other.node->matrixCols) return false;
    if (node->arraySize != other.node->arraySize) return false;

    if (node->baseType == BaseType::Struct)
    {
        return node->structName == other.node->structName;
    }

    if (node->arraySize > 0 && node->elementType && other.node->elementType)
    {
        CgType elemA(node->elementType);
        CgType elemB(other.node->elementType);
        return elemA.equals(elemB);
    }

    return true;
}

bool CgType::isAssignableTo(const CgType& target) const
{
    // Same types are always assignable
    if (equals(target)) return true;

    // Can implicitly convert?
    return isImplicitlyConvertibleTo(target);
}

bool CgType::isImplicitlyConvertibleTo(const CgType& target) const
{
    return TypeConversion::canImplicitlyConvert(*this, target);
}

bool CgType::isExplicitlyConvertibleTo(const CgType& target) const
{
    return TypeConversion::canExplicitlyConvert(*this, target);
}

std::string CgType::toString() const
{
    if (errorType) return "<error>";
    if (!node) return "<null>";
    return node->toString();
}

// ============================================================================
// TypeOperations Implementation
// ============================================================================

bool TypeOperations::isArithmeticOp(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Mod:
        return true;
    default:
        return false;
    }
}

bool TypeOperations::isComparisonOp(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::Equal:
    case BinaryOp::NotEqual:
    case BinaryOp::Less:
    case BinaryOp::LessEqual:
    case BinaryOp::Greater:
    case BinaryOp::GreaterEqual:
        return true;
    default:
        return false;
    }
}

bool TypeOperations::isLogicalOp(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::LogicalAnd:
    case BinaryOp::LogicalOr:
        return true;
    default:
        return false;
    }
}

bool TypeOperations::isBitwiseOp(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::BitwiseAnd:
    case BinaryOp::BitwiseOr:
    case BinaryOp::BitwiseXor:
    case BinaryOp::ShiftLeft:
    case BinaryOp::ShiftRight:
        return true;
    default:
        return false;
    }
}

bool TypeOperations::isAssignmentOp(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::Assign:
    case BinaryOp::AddAssign:
    case BinaryOp::SubAssign:
    case BinaryOp::MulAssign:
    case BinaryOp::DivAssign:
    case BinaryOp::ModAssign:
        return true;
    default:
        return false;
    }
}

ScalarKind TypeOperations::promoteScalars(ScalarKind a, ScalarKind b)
{
    // Promotion order: bool < char < short < int < float < half (for Cg)
    // Note: In Cg, float is preferred over half for most operations

    int rankA = TypeConversion::scalarRank(a);
    int rankB = TypeConversion::scalarRank(b);

    // If one is unsigned and one is signed, prefer unsigned if same rank
    if (rankA == rankB)
    {
        // Prefer the first one, or unsigned if applicable
        return a;
    }

    return (rankA > rankB) ? a : b;
}

bool TypeOperations::areComponentWiseCompatible(const CgType& a, const CgType& b)
{
    // Both must be numeric
    if (!a.isNumeric() || !b.isNumeric()) return false;

    // Scalars are compatible with anything
    if (a.isScalar() || b.isScalar()) return true;

    // Vectors must have same size
    if (a.isVector() && b.isVector())
    {
        return a.vectorSize() == b.vectorSize();
    }

    // Matrices must have same dimensions
    if (a.isMatrix() && b.isMatrix())
    {
        return a.matrixRows() == b.matrixRows() &&
               a.matrixCols() == b.matrixCols();
    }

    return false;
}

CgType TypeOperations::promoteTypes(const CgType& a, const CgType& b)
{
    if (a.isError() || b.isError()) return CgType::Error();

    // Same types - no promotion needed
    if (a.equals(b)) return a;

    // Determine the promoted scalar type
    ScalarKind promotedScalar = promoteScalars(a.scalarKind(), b.scalarKind());

    // Determine the promoted vector/matrix size
    int vecSize = std::max(a.vectorSize(), b.vectorSize());
    int matRows = std::max(a.matrixRows(), b.matrixRows());
    int matCols = std::max(a.matrixCols(), b.matrixCols());

    if (matRows > 0)
    {
        return CgType::Mat(promotedScalar, matRows, matCols);
    }
    else if (vecSize > 1)
    {
        return CgType::Vec(promotedScalar, vecSize);
    }
    else
    {
        // Return scalar of promoted type
        switch (promotedScalar)
        {
        case ScalarKind::Bool:   return CgType::Bool();
        case ScalarKind::Int:    return CgType::Int();
        case ScalarKind::UInt:   return CgType::UInt();
        case ScalarKind::Float:  return CgType::Float();
        case ScalarKind::Half:   return CgType::Half();
        case ScalarKind::Fixed:  return CgType::Fixed();
        case ScalarKind::Char:   return CgType::Char();
        case ScalarKind::UChar:  return CgType::UChar();
        case ScalarKind::Short:  return CgType::Short();
        case ScalarKind::UShort: return CgType::UShort();
        default:                 return CgType::Float();
        }
    }
}

CgType TypeOperations::binaryOpResultType(BinaryOp op, const CgType& left, const CgType& right)
{
    if (!isBinaryOpValid(op, left, right))
    {
        return CgType::Error();
    }

    // Comparison operators always return bool (or bool vector)
    if (isComparisonOp(op))
    {
        int size = std::max(left.vectorSize(), right.vectorSize());
        if (size > 1)
        {
            return CgType::Vec(ScalarKind::Bool, size);
        }
        return CgType::Bool();
    }

    // Logical operators return bool
    if (isLogicalOp(op))
    {
        return CgType::Bool();
    }

    // Assignment operators return the left type
    if (isAssignmentOp(op))
    {
        return left;
    }

    // Comma operator returns right type
    if (op == BinaryOp::Comma)
    {
        return right;
    }

    // Arithmetic and bitwise operators: promote types
    return promoteTypes(left, right);
}

CgType TypeOperations::unaryOpResultType(UnaryOp op, const CgType& operand)
{
    if (!isUnaryOpValid(op, operand))
    {
        return CgType::Error();
    }

    switch (op)
    {
    case UnaryOp::LogicalNot:
        // !x returns bool
        return CgType::Bool();

    case UnaryOp::Negate:
    case UnaryOp::BitwiseNot:
    case UnaryOp::PreIncrement:
    case UnaryOp::PreDecrement:
    case UnaryOp::PostIncrement:
    case UnaryOp::PostDecrement:
        // These return the same type as operand
        return operand;

    default:
        return CgType::Error();
    }
}

bool TypeOperations::isBinaryOpValid(BinaryOp op, const CgType& left, const CgType& right)
{
    if (left.isError() || right.isError()) return false;
    if (left.isVoid() || right.isVoid()) return false;

    // Assignment requires compatible types
    if (isAssignmentOp(op))
    {
        return right.isAssignableTo(left);
    }

    // Arithmetic operations require numeric types
    if (isArithmeticOp(op))
    {
        if (!left.isNumeric() || !right.isNumeric()) return false;
        return areComponentWiseCompatible(left, right);
    }

    // Comparison operations
    if (isComparisonOp(op))
    {
        // Can compare numerics
        if (left.isNumeric() && right.isNumeric())
        {
            return areComponentWiseCompatible(left, right);
        }
        // Can compare structs for equality/inequality
        if (left.isStruct() && right.isStruct() &&
            (op == BinaryOp::Equal || op == BinaryOp::NotEqual))
        {
            return left.equals(right);
        }
        return false;
    }

    // Logical operations require bool-convertible operands
    if (isLogicalOp(op))
    {
        // Any scalar can be used in logical ops
        return left.isScalar() && right.isScalar();
    }

    // Bitwise operations require integral types
    if (isBitwiseOp(op))
    {
        return left.isIntegral() && right.isIntegral() &&
               areComponentWiseCompatible(left, right);
    }

    // Comma is always valid
    if (op == BinaryOp::Comma)
    {
        return true;
    }

    return false;
}

bool TypeOperations::isUnaryOpValid(UnaryOp op, const CgType& operand)
{
    if (operand.isError() || operand.isVoid()) return false;

    switch (op)
    {
    case UnaryOp::Negate:
        // Negate requires numeric type
        return operand.isNumeric();

    case UnaryOp::LogicalNot:
        // Logical not works on any scalar
        return operand.isScalar();

    case UnaryOp::BitwiseNot:
        // Bitwise not requires integral type
        return operand.isIntegral();

    case UnaryOp::PreIncrement:
    case UnaryOp::PreDecrement:
    case UnaryOp::PostIncrement:
    case UnaryOp::PostDecrement:
        // Increment/decrement require numeric lvalue
        return operand.isNumeric();

    default:
        return false;
    }
}

std::optional<CgType> TypeOperations::commonType(const CgType& a, const CgType& b)
{
    if (a.isError() || b.isError()) return std::nullopt;
    if (a.equals(b)) return a;

    // Try to find common type through promotion
    if (a.isNumeric() && b.isNumeric() && areComponentWiseCompatible(a, b))
    {
        return promoteTypes(a, b);
    }

    return std::nullopt;
}

// ============================================================================
// TypeConversion Implementation
// ============================================================================

namespace TypeConversion
{

int scalarRank(ScalarKind kind)
{
    switch (kind)
    {
    case ScalarKind::Bool:   return 1;
    case ScalarKind::Char:   return 2;
    case ScalarKind::UChar:  return 2;
    case ScalarKind::Short:  return 3;
    case ScalarKind::UShort: return 3;
    case ScalarKind::Int:    return 4;
    case ScalarKind::UInt:   return 4;
    case ScalarKind::Half:   return 5;
    case ScalarKind::Fixed:  return 6;
    case ScalarKind::Float:  return 7;
    default:                 return 0;
    }
}

bool canImplicitlyConvert(const CgType& from, const CgType& to)
{
    if (from.equals(to)) return true;
    if (from.isError() || to.isError()) return false;

    // Numeric conversions
    if (from.isNumeric() && to.isNumeric())
    {
        // Scalar to scalar: always allowed implicitly in Cg
        if (from.isScalar() && to.isScalar())
        {
            return true;
        }

        // Scalar to vector: scalar is broadcast
        if (from.isScalar() && to.isVector())
        {
            return true;
        }

        // Vector to vector: same size required
        if (from.isVector() && to.isVector())
        {
            return from.vectorSize() == to.vectorSize();
        }

        // Matrix to matrix: same dimensions required
        if (from.isMatrix() && to.isMatrix())
        {
            return from.matrixRows() == to.matrixRows() &&
                   from.matrixCols() == to.matrixCols();
        }
    }

    // Array conversions: only if element types are compatible
    if (from.isArray() && to.isArray())
    {
        return from.arraySize() == to.arraySize() &&
               canImplicitlyConvert(from.elementType(), to.elementType());
    }

    return false;
}

bool canExplicitlyConvert(const CgType& from, const CgType& to)
{
    // Everything that can be implicitly converted can be explicitly converted
    if (canImplicitlyConvert(from, to)) return true;

    if (from.isError() || to.isError()) return false;

    // Numeric to numeric: always allowed with explicit cast
    if (from.isNumeric() && to.isNumeric())
    {
        return true;
    }

    // Vector size change with explicit cast
    if (from.isVector() && to.isVector())
    {
        // Can truncate or extend vectors
        return true;
    }

    return false;
}

int conversionCost(const CgType& from, const CgType& to)
{
    if (from.equals(to)) return 0;
    if (!canImplicitlyConvert(from, to)) return -1;

    int cost = 0;

    // Scalar type conversion cost
    if (from.isNumeric() && to.isNumeric())
    {
        int rankFrom = scalarRank(from.scalarKind());
        int rankTo = scalarRank(to.scalarKind());

        // Promotion (going up) is cheaper than demotion
        if (rankTo > rankFrom)
        {
            cost += (rankTo - rankFrom);
        }
        else if (rankTo < rankFrom)
        {
            cost += (rankFrom - rankTo) * 2;  // Demotion costs more
        }

        // Sign change adds cost
        if (from.isSigned() != to.isSigned())
        {
            cost += 1;
        }
    }

    // Scalar to vector broadcast adds cost
    if (from.isScalar() && to.isVector())
    {
        cost += 1;
    }

    return cost;
}

} // namespace TypeConversion
