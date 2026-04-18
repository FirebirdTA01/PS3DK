#pragma once

#include "ast.h"
#include "types.h"
#include "symbol_table.h"
#include <vector>
#include <string>
#include <memory>
#include <functional>

// ============================================================================
// Semantic Error/Warning
// ============================================================================

struct SemanticDiagnostic
{
    enum class Severity
    {
        Error,
        Warning,
        Note
    };

    Severity severity;
    SourceLocation loc;
    std::string message;

    SemanticDiagnostic(Severity sev, const SourceLocation& l, const std::string& msg)
        : severity(sev), loc(l), message(msg) {}

    bool isError() const { return severity == Severity::Error; }
    bool isWarning() const { return severity == Severity::Warning; }

    std::string toString() const;
};

// ============================================================================
// Shader Profile Information
// ============================================================================

enum class ShaderStage
{
    Vertex,
    Fragment
};

// Represents a shader input/output with semantic binding
struct ShaderIOParam
{
    std::string name;           // Full name (e.g., "input.position" or "position")
    std::string semanticName;   // Semantic name (e.g., "POSITION", "TEXCOORD")
    int semanticIndex = 0;      // Semantic index (e.g., 0 for TEXCOORD0)
    CgType type;                // The type of the parameter
    bool isOutput = false;      // true for outputs, false for inputs

    ShaderIOParam() = default;
    ShaderIOParam(const std::string& n, const std::string& sem, int semIdx, const CgType& t, bool out = false)
        : name(n), semanticName(sem), semanticIndex(semIdx), type(t), isOutput(out) {}
};

struct ShaderInfo
{
    ShaderStage stage = ShaderStage::Vertex;
    std::string entryPointName = "main";
    FunctionDecl* entryPoint = nullptr;

    // Collected shader inputs/outputs (flattened, with semantics)
    std::vector<ShaderIOParam> inputParams;    // Vertex attributes / Fragment varyings
    std::vector<ShaderIOParam> outputParams;   // Vertex varyings / Fragment outputs

    // Global uniforms
    std::vector<VarDecl*> uniforms;            // Uniform variables
    std::vector<ParamDecl*> uniformParams;     // Uniform function parameters
    std::vector<BufferDecl*> bufferUniforms;   // Buffer declarations (BUFFER[N] structs)

    // Legacy pointers (for backwards compatibility)
    std::vector<VarDecl*> attributes;          // Vertex inputs (deprecated, use inputParams)
    std::vector<VarDecl*> varyings;            // (deprecated, use outputParams)

    // Validation flags
    bool hasPositionOutput = false;
    bool hasColorOutput = false;
    bool usesDiscard = false;
};

// ============================================================================
// Semantic Analyzer
// ============================================================================

class SemanticAnalyzer
{
public:
    SemanticAnalyzer();
    ~SemanticAnalyzer();

    // Configure the analyzer
    void setShaderStage(ShaderStage stage);
    void setEntryPoint(const std::string& name);

    // Main analysis entry point
    bool analyze(TranslationUnit& unit);

    // Get diagnostics
    const std::vector<SemanticDiagnostic>& diagnostics() const { return diagnostics_; }
    bool hasErrors() const;
    int errorCount() const;
    int warningCount() const;

    // Get shader info after analysis
    const ShaderInfo& shaderInfo() const { return shaderInfo_; }

    // Get the symbol table (for later phases)
    SymbolTable& symbolTable() { return symbols_; }
    const SymbolTable& symbolTable() const { return symbols_; }

    // Type resolution (needed by IR builder for buffer struct flattening)
    CgType resolveType(TypeNode* typeNode) const;

private:
    SymbolTable symbols_;
    std::vector<SemanticDiagnostic> diagnostics_;
    ShaderInfo shaderInfo_;
    std::vector<std::unique_ptr<BufferDecl>> bufferDeclOwners_;  // Owns BufferDecl created from VarDecl+BUFFER semantic

    // Current context during analysis
    FunctionDecl* currentFunction_ = nullptr;
    bool inLoop_ = false;
    bool inSwitch_ = false;

    // Error/warning reporting
    void error(const SourceLocation& loc, const std::string& message);
    void warning(const SourceLocation& loc, const std::string& message);
    void note(const SourceLocation& loc, const std::string& message);

    // Analysis passes
    void collectDeclarations(TranslationUnit& unit);
    void analyzeDeclarations(TranslationUnit& unit);
    void validateShader();

    // Declaration analysis
    void collectStructDecl(StructDecl* decl);
    void collectFunctionDecl(FunctionDecl* decl);
    void collectVarDecl(VarDecl* decl);
    void collectBufferDecl(BufferDecl* decl);

    void analyzeStructDecl(StructDecl* decl);
    void analyzeFunctionDecl(FunctionDecl* decl);
    void analyzeVarDecl(VarDecl* decl);

    // Statement analysis
    void analyzeStmt(StmtNode* stmt);
    void analyzeBlockStmt(BlockStmt* stmt);
    void analyzeIfStmt(IfStmt* stmt);
    void analyzeForStmt(ForStmt* stmt);
    void analyzeWhileStmt(WhileStmt* stmt);
    void analyzeDoWhileStmt(DoWhileStmt* stmt);
    void analyzeSwitchStmt(SwitchStmt* stmt);
    void analyzeReturnStmt(ReturnStmt* stmt);
    void analyzeExprStmt(ExprStmt* stmt);
    void analyzeDeclStmt(DeclStmt* stmt);

    // Expression analysis - returns the resolved type
    CgType analyzeExpr(ExprNode* expr);
    CgType analyzeLiteralExpr(LiteralExpr* expr);
    CgType analyzeIdentifierExpr(IdentifierExpr* expr);
    CgType analyzeBinaryExpr(BinaryExpr* expr);
    CgType analyzeUnaryExpr(UnaryExpr* expr);
    CgType analyzeCallExpr(CallExpr* expr);
    CgType analyzeMemberAccessExpr(MemberAccessExpr* expr);
    CgType analyzeIndexExpr(IndexExpr* expr);
    CgType analyzeTernaryExpr(TernaryExpr* expr);
    CgType analyzeCastExpr(CastExpr* expr);
    CgType analyzeConstructorExpr(ConstructorExpr* expr);
    CgType analyzeSizeofExpr(SizeofExpr* expr);

    // Helper methods
    bool checkAssignment(const CgType& target, const CgType& value, const SourceLocation& loc);
    bool checkCondition(ExprNode* expr);
    bool isLvalue(ExprNode* expr);
    void validateSwizzle(MemberAccessExpr* expr, const CgType& objectType);

    // Shader-specific validation
    void validateEntryPoint();
    void validateVertexShader();
    void validateFragmentShader();
    void collectShaderIO(FunctionDecl* entryPoint);
};

// ============================================================================
// Utility Functions
// ============================================================================

namespace SemanticUtils
{
    // Check if a semantic is valid for the given shader stage
    bool isValidSemantic(const Semantic& sem, ShaderStage stage, bool isInput);

    // Get semantic category
    enum class SemanticCategory
    {
        Position,
        Color,
        TexCoord,
        Normal,
        Tangent,
        Binormal,
        BlendWeight,
        BlendIndices,
        Depth,
        Fog,
        PointSize,
        // Additional PSP2/Vita semantics
        VertexIndex,        // INDEX - vertex ID
        InstanceIndex,      // INSTANCE - instance ID
        SpriteCoord,        // SPRITECOORD/POINTCOORD - point sprite coordinates
        FragColor,          // FRAGCOLOR - programmable blending input
        Face,               // FACE - front/back facing
        ClipPlane,          // CLP0-CLP7 - user clip planes
        WorldPosition,      // WPOS - window position
        Varyings,           // Generic varying
        Other
    };

    SemanticCategory getSemanticCategory(const Semantic& sem);

    // Check if identifier is a valid swizzle pattern
    bool isValidSwizzle(const std::string& swizzle, int maxComponents);

    // Parse swizzle pattern into component indices
    bool parseSwizzle(const std::string& swizzle, int swizzleIndices[4], int& length);
}
