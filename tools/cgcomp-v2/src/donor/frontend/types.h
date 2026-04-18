#pragma once

#include "ast.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>

// ============================================================================
// CgType - Extended type representation for semantic analysis
// ============================================================================

// Type category for quick classification
enum class TypeCategory
{
    Void,
    Scalar,
    Vector,
    Matrix,
    Sampler,
    Struct,
    Array,
    Error       // Represents a type error (for error recovery)
};

// Scalar type kind for numeric types
enum class ScalarKind
{
    Bool,
    Int,
    UInt,
    Float,
    Half,
    Fixed,
    Char,
    UChar,
    Short,
    UShort
};

class CgType
{
public:
    // Constructors
    CgType();  // Creates void type
    explicit CgType(std::shared_ptr<TypeNode> node);

    // Factory methods for common types
    static CgType Void();
    static CgType Error();
    static CgType Bool();
    static CgType Int();
    static CgType UInt();
    static CgType Float();
    static CgType Half();
    static CgType Fixed();
    static CgType Char();
    static CgType UChar();
    static CgType Short();
    static CgType UShort();

    // Scalar factory from ScalarKind
    static CgType Scalar(ScalarKind kind);  // e.g., Scalar(Float) = float

    // Vector factory methods
    static CgType Vec(ScalarKind scalar, int size);  // e.g., Vec(Float, 4) = float4
    static CgType Float2();
    static CgType Float3();
    static CgType Float4();
    static CgType Int2();
    static CgType Int3();
    static CgType Int4();
    static CgType Half2();
    static CgType Half3();
    static CgType Half4();
    static CgType Bool2();
    static CgType Bool3();
    static CgType Bool4();

    // Matrix factory methods
    static CgType Mat(ScalarKind scalar, int rows, int cols);
    static CgType Float2x2();
    static CgType Float3x3();
    static CgType Float4x4();

    // Sampler factory methods
    static CgType Sampler1D();
    static CgType Sampler2D();
    static CgType Sampler3D();
    static CgType SamplerCube();
    static CgType ISampler2D();
    static CgType USampler2D();

    // Array factory
    static CgType Array(const CgType& elementType, int size);

    // Struct factory
    static CgType Struct(const std::string& name, const std::vector<StructField>& fields);

    // Type queries
    TypeCategory category() const;
    bool isVoid() const;
    bool isError() const;
    bool isScalar() const;
    bool isVector() const;
    bool isMatrix() const;
    bool isSampler() const;
    bool isStruct() const;
    bool isArray() const;

    // Numeric type queries
    bool isNumeric() const;          // Int, UInt, Float, Half, Fixed, Char, etc.
    bool isIntegral() const;         // Int, UInt, Char, UChar, Short, UShort
    bool isFloatingPoint() const;    // Float, Half, Fixed
    bool isSigned() const;           // Int, Char, Short, Float, Half, Fixed
    bool isUnsigned() const;         // UInt, UChar, UShort, Bool

    // Component info
    int vectorSize() const;          // 1 for scalar, 2-4 for vectors
    int matrixRows() const;          // 0 for non-matrix
    int matrixCols() const;          // 0 for non-matrix
    int componentCount() const;      // Total components (rows * cols or vectorSize)
    int arraySize() const;           // 0 for non-array

    // For scalars/vectors, get the underlying scalar type
    ScalarKind scalarKind() const;

    // For structs
    const std::string& structName() const;
    const std::vector<StructField>& structFields() const;
    std::optional<CgType> getFieldType(const std::string& fieldName) const;

    // For arrays
    CgType elementType() const;

    // Get swizzle result type (e.g., float4.xyz -> float3)
    CgType swizzleType(int resultSize) const;

    // Type compatibility
    bool equals(const CgType& other) const;
    bool isAssignableTo(const CgType& target) const;      // Can this be assigned to target?
    bool isImplicitlyConvertibleTo(const CgType& target) const;
    bool isExplicitlyConvertibleTo(const CgType& target) const;

    // Get underlying TypeNode (for AST integration)
    std::shared_ptr<TypeNode> getNode() const { return node; }

    // String representation
    std::string toString() const;

    // Operators
    bool operator==(const CgType& other) const { return equals(other); }
    bool operator!=(const CgType& other) const { return !equals(other); }

private:
    std::shared_ptr<TypeNode> node;
    bool errorType = false;

    static std::shared_ptr<TypeNode> makeScalarNode(BaseType bt);
    static std::shared_ptr<TypeNode> makeVectorNode(BaseType bt, int size);
    static std::shared_ptr<TypeNode> makeMatrixNode(BaseType bt, int rows, int cols);
};

// ============================================================================
// Type Operations - Binary/Unary operator type inference
// ============================================================================

class TypeOperations
{
public:
    // Get result type of binary operation
    // Returns Error type if operation is invalid
    static CgType binaryOpResultType(BinaryOp op, const CgType& left, const CgType& right);

    // Get result type of unary operation
    static CgType unaryOpResultType(UnaryOp op, const CgType& operand);

    // Check if binary operation is valid
    static bool isBinaryOpValid(BinaryOp op, const CgType& left, const CgType& right);

    // Check if unary operation is valid
    static bool isUnaryOpValid(UnaryOp op, const CgType& operand);

    // Get common type for two types (for ternary operator)
    static std::optional<CgType> commonType(const CgType& a, const CgType& b);

    // Check if types are compatible for component-wise operations
    static bool areComponentWiseCompatible(const CgType& a, const CgType& b);

    // Get the promoted type when mixing scalars and vectors
    // e.g., float + float4 -> float4
    static CgType promoteTypes(const CgType& a, const CgType& b);

    // Binary operator category checks
    static bool isArithmeticOp(BinaryOp op);
    static bool isComparisonOp(BinaryOp op);
    static bool isLogicalOp(BinaryOp op);
    static bool isBitwiseOp(BinaryOp op);
    static bool isAssignmentOp(BinaryOp op);

private:
    // Arithmetic type promotion rules
    static ScalarKind promoteScalars(ScalarKind a, ScalarKind b);
};

// ============================================================================
// Type Conversion Utilities
// ============================================================================

namespace TypeConversion
{
    // Check if implicit conversion is allowed
    bool canImplicitlyConvert(const CgType& from, const CgType& to);

    // Check if explicit cast is allowed
    bool canExplicitlyConvert(const CgType& from, const CgType& to);

    // Get the cost of implicit conversion (for overload resolution)
    // Lower cost = better match. -1 = no conversion possible
    int conversionCost(const CgType& from, const CgType& to);

    // Scalar promotion rank (for type promotion rules)
    int scalarRank(ScalarKind kind);
}
