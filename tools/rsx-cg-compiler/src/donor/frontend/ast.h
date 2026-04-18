#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>

// Forward declarations
struct AstNode;
struct ExprNode;
struct StmtNode;
struct DeclNode;
struct TypeNode;

// Source location for error reporting
struct SourceLocation
{
    std::string filename;
    int line = 0;
    int column = 0;

    std::string toString() const
    {
        return filename + ":" + std::to_string(line) + ":" + std::to_string(column);
    }
};

// Storage qualifiers for variables/parameters
enum class StorageQualifier
{
    None,
    Uniform,
    In,
    Out,
    InOut,
    Const,
    Static,
    Extern
};

// Semantic annotations (POSITION, TEXCOORD0, etc.)
struct Semantic
{
    std::string name;      // e.g., "POSITION", "TEXCOORD", "COLOR"
    int index = 0;         // e.g., 0 for TEXCOORD0, 1 for TEXCOORD1

    bool isEmpty() const { return name.empty(); }
};

// Vita-specific attributes
struct VitaAttributes
{
    bool noStrip = false;
    bool nativeColor = false;
    std::string regFormat;      // __regformat value
    int msaaLevel = 0;          // 0=none, 2=2x, 4=4x
};

// ============================================================================
// Type Nodes
// ============================================================================

enum class BaseType
{
    Void,
    Bool,
    Int,
    UInt,
    Float,
    Half,
    Fixed,
    Char,
    UChar,
    Short,
    UShort,
    // Sampler types
    Sampler1D,
    Sampler2D,
    Sampler3D,
    SamplerCube,
    SamplerRect,
    ISampler1D,
    ISampler2D,
    ISampler3D,
    ISamplerCube,
    ISamplerRect,
    USampler1D,
    USampler2D,
    USampler3D,
    USamplerCube,
    USamplerRect,
    // Composite
    Struct,
    Array
};

struct StructField
{
    std::string name;
    std::shared_ptr<TypeNode> type;
    Semantic semantic;
};

struct TypeNode
{
    BaseType baseType = BaseType::Void;
    int vectorSize = 1;        // 1 for scalar, 2-4 for vectors
    int matrixRows = 0;        // 0 for non-matrix, 2-4 for matrices
    int matrixCols = 0;
    int arraySize = 0;         // 0 for non-array, >0 for fixed-size arrays
    bool isRowMajor = false;   // Matrix layout
    bool isPacked = false;     // Cg packed arrays

    // For struct types
    std::string structName;
    std::vector<StructField> structFields;

    // For array types
    std::shared_ptr<TypeNode> elementType;

    // Utility methods
    bool isScalar() const { return vectorSize == 1 && matrixRows == 0 && arraySize == 0; }
    bool isVector() const { return vectorSize > 1 && matrixRows == 0; }
    bool isMatrix() const { return matrixRows > 0; }
    bool isArray() const { return arraySize > 0; }
    bool isSampler() const;
    bool isNumeric() const;
    bool isIntegral() const;
    bool isFloatingPoint() const;
    int componentCount() const;

    std::string toString() const;

    // Factory methods for common types
    static std::shared_ptr<TypeNode> makeVoid();
    static std::shared_ptr<TypeNode> makeBool();
    static std::shared_ptr<TypeNode> makeInt();
    static std::shared_ptr<TypeNode> makeUInt();
    static std::shared_ptr<TypeNode> makeFloat();
    static std::shared_ptr<TypeNode> makeFloat2();
    static std::shared_ptr<TypeNode> makeFloat3();
    static std::shared_ptr<TypeNode> makeFloat4();
    static std::shared_ptr<TypeNode> makeFloat4x4();
    static std::shared_ptr<TypeNode> makeHalf();
    static std::shared_ptr<TypeNode> makeHalf4();
    static std::shared_ptr<TypeNode> makeSampler2D();
};

// ============================================================================
// Expression Nodes
// ============================================================================

enum class ExprKind
{
    Literal,
    Identifier,
    Binary,
    Unary,
    Call,
    MemberAccess,
    Index,
    Ternary,
    Cast,
    Constructor,
    Sizeof
};

struct ExprNode
{
    ExprKind kind;
    SourceLocation loc;
    std::shared_ptr<TypeNode> resolvedType;  // Filled during semantic analysis

    virtual ~ExprNode() = default;

protected:
    ExprNode(ExprKind k, SourceLocation l) : kind(k), loc(l) {}
};

// Literal values: int, float, bool
struct LiteralExpr : ExprNode
{
    enum class LiteralKind { Int, Float, Bool };
    LiteralKind literalKind;
    std::variant<int64_t, double, bool> value;
    std::string suffix;  // "f", "h" for half, etc.

    LiteralExpr(SourceLocation loc, int64_t v)
        : ExprNode(ExprKind::Literal, loc), literalKind(LiteralKind::Int), value(v) {}
    LiteralExpr(SourceLocation loc, double v, const std::string& suf = "")
        : ExprNode(ExprKind::Literal, loc), literalKind(LiteralKind::Float), value(v), suffix(suf) {}
    LiteralExpr(SourceLocation loc, bool v)
        : ExprNode(ExprKind::Literal, loc), literalKind(LiteralKind::Bool), value(v) {}
};

// Variable/function references
struct IdentifierExpr : ExprNode
{
    std::string name;
    DeclNode* resolvedDecl = nullptr;  // Filled during semantic analysis

    IdentifierExpr(SourceLocation loc, const std::string& n)
        : ExprNode(ExprKind::Identifier, loc), name(n) {}
};

// Binary operations
enum class BinaryOp
{
    Add, Sub, Mul, Div, Mod,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    LogicalAnd, LogicalOr,
    BitwiseAnd, BitwiseOr, BitwiseXor,
    ShiftLeft, ShiftRight,
    Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
    Comma
};

struct BinaryExpr : ExprNode
{
    BinaryOp op;
    std::unique_ptr<ExprNode> left;
    std::unique_ptr<ExprNode> right;

    BinaryExpr(SourceLocation loc, BinaryOp o, std::unique_ptr<ExprNode> l, std::unique_ptr<ExprNode> r)
        : ExprNode(ExprKind::Binary, loc), op(o), left(std::move(l)), right(std::move(r)) {}
};

// Unary operations
enum class UnaryOp
{
    Negate,         // -x
    LogicalNot,     // !x
    BitwiseNot,     // ~x
    PreIncrement,   // ++x
    PreDecrement,   // --x
    PostIncrement,  // x++
    PostDecrement   // x--
};

struct UnaryExpr : ExprNode
{
    UnaryOp op;
    std::unique_ptr<ExprNode> operand;

    UnaryExpr(SourceLocation loc, UnaryOp o, std::unique_ptr<ExprNode> operand)
        : ExprNode(ExprKind::Unary, loc), op(o), operand(std::move(operand)) {}
};

// Function calls
struct CallExpr : ExprNode
{
    std::string functionName;
    std::vector<std::unique_ptr<ExprNode>> arguments;
    DeclNode* resolvedFunction = nullptr;  // Filled during semantic analysis

    CallExpr(SourceLocation loc, const std::string& name)
        : ExprNode(ExprKind::Call, loc), functionName(name) {}
};

// Member access: a.x, a.xyz (swizzle)
struct MemberAccessExpr : ExprNode
{
    std::unique_ptr<ExprNode> object;
    std::string member;
    bool isSwizzle = false;
    int swizzleIndices[4] = {0, 0, 0, 0};  // Filled during semantic analysis
    int swizzleLength = 0;

    MemberAccessExpr(SourceLocation loc, std::unique_ptr<ExprNode> obj, const std::string& mem)
        : ExprNode(ExprKind::MemberAccess, loc), object(std::move(obj)), member(mem) {}
};

// Array indexing: a[i]
struct IndexExpr : ExprNode
{
    std::unique_ptr<ExprNode> array;
    std::unique_ptr<ExprNode> index;

    IndexExpr(SourceLocation loc, std::unique_ptr<ExprNode> arr, std::unique_ptr<ExprNode> idx)
        : ExprNode(ExprKind::Index, loc), array(std::move(arr)), index(std::move(idx)) {}
};

// Ternary conditional: cond ? a : b
struct TernaryExpr : ExprNode
{
    std::unique_ptr<ExprNode> condition;
    std::unique_ptr<ExprNode> thenExpr;
    std::unique_ptr<ExprNode> elseExpr;

    TernaryExpr(SourceLocation loc, std::unique_ptr<ExprNode> cond,
                std::unique_ptr<ExprNode> then_, std::unique_ptr<ExprNode> else_)
        : ExprNode(ExprKind::Ternary, loc), condition(std::move(cond)),
          thenExpr(std::move(then_)), elseExpr(std::move(else_)) {}
};

// Type cast: (float4)x
struct CastExpr : ExprNode
{
    std::shared_ptr<TypeNode> targetType;
    std::unique_ptr<ExprNode> operand;

    CastExpr(SourceLocation loc, std::shared_ptr<TypeNode> type, std::unique_ptr<ExprNode> op)
        : ExprNode(ExprKind::Cast, loc), targetType(type), operand(std::move(op)) {}
};

// Type constructor: float4(1, 2, 3, 4)
struct ConstructorExpr : ExprNode
{
    std::shared_ptr<TypeNode> constructedType;
    std::vector<std::unique_ptr<ExprNode>> arguments;

    ConstructorExpr(SourceLocation loc, std::shared_ptr<TypeNode> type)
        : ExprNode(ExprKind::Constructor, loc), constructedType(type) {}
};

// sizeof expression
struct SizeofExpr : ExprNode
{
    std::variant<std::shared_ptr<TypeNode>, std::unique_ptr<ExprNode>> operand;

    SizeofExpr(SourceLocation loc, std::shared_ptr<TypeNode> type)
        : ExprNode(ExprKind::Sizeof, loc), operand(type) {}
    SizeofExpr(SourceLocation loc, std::unique_ptr<ExprNode> expr)
        : ExprNode(ExprKind::Sizeof, loc), operand(std::move(expr)) {}
};

// ============================================================================
// Statement Nodes
// ============================================================================

enum class StmtKind
{
    Expr,
    Decl,
    Block,
    If,
    For,
    While,
    DoWhile,
    Switch,
    Case,
    Default,
    Return,
    Break,
    Continue,
    Discard,
    Empty
};

struct StmtNode
{
    StmtKind kind;
    SourceLocation loc;

    virtual ~StmtNode() = default;

protected:
    StmtNode(StmtKind k, SourceLocation l) : kind(k), loc(l) {}
};

// Expression statement
struct ExprStmt : StmtNode
{
    std::unique_ptr<ExprNode> expr;

    ExprStmt(SourceLocation loc, std::unique_ptr<ExprNode> e)
        : StmtNode(StmtKind::Expr, loc), expr(std::move(e)) {}
};

// Declaration statement (variable declaration inside function)
struct DeclStmt : StmtNode
{
    std::unique_ptr<DeclNode> declaration;

    DeclStmt(SourceLocation loc, std::unique_ptr<DeclNode> decl)
        : StmtNode(StmtKind::Decl, loc), declaration(std::move(decl)) {}
};

// Block statement { ... }
struct BlockStmt : StmtNode
{
    std::vector<std::unique_ptr<StmtNode>> statements;

    BlockStmt(SourceLocation loc) : StmtNode(StmtKind::Block, loc) {}
};

// If statement
struct IfStmt : StmtNode
{
    std::unique_ptr<ExprNode> condition;
    std::unique_ptr<StmtNode> thenBranch;
    std::unique_ptr<StmtNode> elseBranch;  // nullable

    IfStmt(SourceLocation loc, std::unique_ptr<ExprNode> cond,
           std::unique_ptr<StmtNode> then_, std::unique_ptr<StmtNode> else_ = nullptr)
        : StmtNode(StmtKind::If, loc), condition(std::move(cond)),
          thenBranch(std::move(then_)), elseBranch(std::move(else_)) {}
};

// For loop
struct ForStmt : StmtNode
{
    std::unique_ptr<StmtNode> init;        // DeclStmt or ExprStmt, nullable
    std::unique_ptr<ExprNode> condition;    // nullable
    std::unique_ptr<ExprNode> increment;    // nullable
    std::unique_ptr<StmtNode> body;

    ForStmt(SourceLocation loc) : StmtNode(StmtKind::For, loc) {}
};

// While loop
struct WhileStmt : StmtNode
{
    std::unique_ptr<ExprNode> condition;
    std::unique_ptr<StmtNode> body;

    WhileStmt(SourceLocation loc, std::unique_ptr<ExprNode> cond, std::unique_ptr<StmtNode> body)
        : StmtNode(StmtKind::While, loc), condition(std::move(cond)), body(std::move(body)) {}
};

// Do-while loop
struct DoWhileStmt : StmtNode
{
    std::unique_ptr<StmtNode> body;
    std::unique_ptr<ExprNode> condition;

    DoWhileStmt(SourceLocation loc, std::unique_ptr<StmtNode> body, std::unique_ptr<ExprNode> cond)
        : StmtNode(StmtKind::DoWhile, loc), body(std::move(body)), condition(std::move(cond)) {}
};

// Switch statement
struct SwitchStmt : StmtNode
{
    std::unique_ptr<ExprNode> expr;
    std::unique_ptr<BlockStmt> body;

    SwitchStmt(SourceLocation loc, std::unique_ptr<ExprNode> e)
        : StmtNode(StmtKind::Switch, loc), expr(std::move(e)) {}
};

// Case label
struct CaseStmt : StmtNode
{
    std::unique_ptr<ExprNode> value;  // Must be constant expression

    CaseStmt(SourceLocation loc, std::unique_ptr<ExprNode> v)
        : StmtNode(StmtKind::Case, loc), value(std::move(v)) {}
};

// Default label
struct DefaultStmt : StmtNode
{
    DefaultStmt(SourceLocation loc) : StmtNode(StmtKind::Default, loc) {}
};

// Return statement
struct ReturnStmt : StmtNode
{
    std::unique_ptr<ExprNode> value;  // nullable for void returns

    ReturnStmt(SourceLocation loc, std::unique_ptr<ExprNode> v = nullptr)
        : StmtNode(StmtKind::Return, loc), value(std::move(v)) {}
};

// Break statement
struct BreakStmt : StmtNode
{
    BreakStmt(SourceLocation loc) : StmtNode(StmtKind::Break, loc) {}
};

// Continue statement
struct ContinueStmt : StmtNode
{
    ContinueStmt(SourceLocation loc) : StmtNode(StmtKind::Continue, loc) {}
};

// Discard statement (fragment shader only)
struct DiscardStmt : StmtNode
{
    DiscardStmt(SourceLocation loc) : StmtNode(StmtKind::Discard, loc) {}
};

// Empty statement (just semicolon)
struct EmptyStmt : StmtNode
{
    EmptyStmt(SourceLocation loc) : StmtNode(StmtKind::Empty, loc) {}
};

// ============================================================================
// Declaration Nodes
// ============================================================================

enum class DeclKind
{
    Variable,
    Parameter,
    Function,
    Struct,
    Typedef,
    Buffer
};

struct DeclNode
{
    DeclKind kind;
    SourceLocation loc;
    std::string name;

    virtual ~DeclNode() = default;

protected:
    DeclNode(DeclKind k, SourceLocation l, const std::string& n)
        : kind(k), loc(l), name(n) {}
};

// Variable declaration
struct VarDecl : DeclNode
{
    std::shared_ptr<TypeNode> type;
    std::unique_ptr<ExprNode> initializer;  // nullable
    StorageQualifier storage = StorageQualifier::None;
    Semantic semantic;
    VitaAttributes vitaAttrs;

    VarDecl(SourceLocation loc, const std::string& name, std::shared_ptr<TypeNode> type)
        : DeclNode(DeclKind::Variable, loc, name), type(type) {}
};

// Function parameter (extends VarDecl concept)
struct ParamDecl : DeclNode
{
    std::shared_ptr<TypeNode> type;
    StorageQualifier storage = StorageQualifier::In;  // default is 'in'
    Semantic semantic;
    std::unique_ptr<ExprNode> defaultValue;  // nullable

    ParamDecl(SourceLocation loc, const std::string& name, std::shared_ptr<TypeNode> type)
        : DeclNode(DeclKind::Parameter, loc, name), type(type) {}
};

// Function declaration/definition
struct FunctionDecl : DeclNode
{
    std::shared_ptr<TypeNode> returnType;
    std::vector<std::unique_ptr<ParamDecl>> parameters;
    std::unique_ptr<BlockStmt> body;  // nullptr for prototypes
    Semantic returnSemantic;

    // Intrinsic info (from builtin header)
    bool isIntrinsic = false;
    std::string intrinsicOpcode;  // From __attribute__((OpcodeName(...)))

    bool isPrototype() const { return body == nullptr; }

    FunctionDecl(SourceLocation loc, const std::string& name, std::shared_ptr<TypeNode> retType)
        : DeclNode(DeclKind::Function, loc, name), returnType(retType) {}
};

// Struct declaration
struct StructDecl : DeclNode
{
    std::vector<StructField> fields;

    StructDecl(SourceLocation loc, const std::string& name)
        : DeclNode(DeclKind::Struct, loc, name) {}
};

// Typedef declaration
struct TypedefDecl : DeclNode
{
    std::shared_ptr<TypeNode> aliasedType;

    TypedefDecl(SourceLocation loc, const std::string& name, std::shared_ptr<TypeNode> type)
        : DeclNode(DeclKind::Typedef, loc, name), aliasedType(type) {}
};

// Buffer declaration: BUFFER[N] declarations
struct BufferDecl : DeclNode
{
    int bufferIndex;
    std::shared_ptr<TypeNode> type;

    BufferDecl(SourceLocation loc, const std::string& name, int index)
        : DeclNode(DeclKind::Buffer, loc, name), bufferIndex(index) {}
};

// ============================================================================
// Translation Unit (top-level AST root)
// ============================================================================

struct TranslationUnit
{
    std::vector<std::unique_ptr<DeclNode>> declarations;
    FunctionDecl* entryPoint = nullptr;  // Points to the entry function (main or specified)

    std::string filename;
};

// ============================================================================
// AST Printer (for debugging)
// ============================================================================

class AstPrinter
{
public:
    void print(const TranslationUnit& unit);
    void print(const DeclNode* decl, int indent = 0);
    void print(const StmtNode* stmt, int indent = 0);
    void print(const ExprNode* expr, int indent = 0);
    void print(const TypeNode* type);

private:
    void indent(int level);
};

// ============================================================================
// Helper functions
// ============================================================================

std::string binaryOpToString(BinaryOp op);
std::string unaryOpToString(UnaryOp op);
std::string storageQualifierToString(StorageQualifier sq);
std::string baseTypeToString(BaseType bt);
