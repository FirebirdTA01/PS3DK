#pragma once

#include "types.h"
#include "ast.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

// ============================================================================
// Symbol - Represents a named entity in the program
// ============================================================================

enum class SymbolKind
{
    Variable,
    Parameter,
    Function,
    Type,       // struct, typedef
    Field,      // struct field
    Builtin     // built-in function
};

struct Symbol
{
    SymbolKind kind;
    std::string name;
    CgType type;
    DeclNode* declaration = nullptr;  // Points to AST node if applicable
    SourceLocation loc;

    // For variables/parameters
    StorageQualifier storage = StorageQualifier::None;
    Semantic semantic;
    bool isConst = false;

    // For functions
    std::vector<CgType> parameterTypes;
    std::vector<std::string> parameterNames;
    bool isIntrinsic = false;
    std::string intrinsicOpcode;

    // For overloaded functions - list of all overloads
    std::vector<Symbol*> overloads;

    Symbol() = default;
    Symbol(SymbolKind k, const std::string& n, const CgType& t)
        : kind(k), name(n), type(t) {}
};

// ============================================================================
// Scope - A single scope level (block, function, global)
// ============================================================================

class Scope
{
public:
    enum class Kind
    {
        Global,
        Function,
        Block
    };

    Scope(Kind k, Scope* parent = nullptr);

    Kind kind() const { return scopeKind; }
    Scope* parent() const { return parentScope; }

    // Symbol management
    bool addSymbol(std::unique_ptr<Symbol> symbol);
    Symbol* lookupLocal(const std::string& name) const;
    Symbol* lookup(const std::string& name) const;  // Searches parent scopes

    // Type management (for struct types)
    bool addType(const std::string& name, const CgType& type);
    std::optional<CgType> lookupType(const std::string& name) const;

    // Get all symbols in this scope
    const std::unordered_map<std::string, std::unique_ptr<Symbol>>& symbols() const
    {
        return symbolTable;
    }

    // Get all types in this scope
    const std::unordered_map<std::string, CgType>& types() const
    {
        return typeTable;
    }

private:
    Kind scopeKind;
    Scope* parentScope;
    std::unordered_map<std::string, std::unique_ptr<Symbol>> symbolTable;
    std::unordered_map<std::string, CgType> typeTable;
};

// ============================================================================
// SymbolTable - Manages scopes and symbol lookup
// ============================================================================

class SymbolTable
{
public:
    SymbolTable();
    ~SymbolTable();

    // Scope management
    void pushScope(Scope::Kind kind);
    void popScope();
    Scope* currentScope() const { return scopeStack.empty() ? nullptr : scopeStack.back().get(); }
    Scope* globalScope() const { return globalScopePtr; }

    // Symbol operations
    bool addSymbol(std::unique_ptr<Symbol> symbol);
    bool addSymbol(SymbolKind kind, const std::string& name, const CgType& type, DeclNode* decl = nullptr);
    Symbol* lookup(const std::string& name) const;
    Symbol* lookupLocal(const std::string& name) const;

    // Type operations
    bool addType(const std::string& name, const CgType& type);
    std::optional<CgType> lookupType(const std::string& name) const;

    // Function overload resolution
    struct OverloadCandidate
    {
        Symbol* symbol;
        int conversionCost;
        bool exactMatch;
    };

    // Find the best matching overload for a function call
    std::optional<OverloadCandidate> resolveOverload(
        const std::string& name,
        const std::vector<CgType>& argumentTypes) const;

    // Add a function and handle overloading
    bool addFunction(const std::string& name,
                     const CgType& returnType,
                     const std::vector<CgType>& paramTypes,
                     const std::vector<std::string>& paramNames,
                     FunctionDecl* decl = nullptr,
                     bool isIntrinsic = false,
                     const std::string& opcode = "");

    // Register built-in functions and types
    void registerBuiltins();

    // Get scope depth
    size_t depth() const { return scopeStack.size(); }

private:
    std::vector<std::unique_ptr<Scope>> scopeStack;
    Scope* globalScopePtr = nullptr;

    // Storage for all function symbols (owns the memory)
    std::vector<std::unique_ptr<Symbol>> allFunctionSymbols;

    // For function overloads - maps name to list of overloaded symbols (non-owning pointers)
    std::unordered_map<std::string, std::vector<Symbol*>> functionOverloads;

    void registerBuiltinTypes();
    void registerBuiltinFunctions();
    void registerMathFunctions();
    void registerVectorFunctions();
    void registerTextureSymbols();
    void registerVitaIntrinsics();

    // Helper to add common math function overloads (sin, cos, etc.)
    void addMathFunctionOverloads(const std::string& name);

    // Helper to add function with various vector sizes
    void addScalarVectorOverloads(const std::string& name,
                                   const CgType& scalarResult,
                                   const CgType& scalarParam);
};

// ============================================================================
// Symbol Table Utilities
// ============================================================================

namespace SymbolUtils
{
    // Create a symbol from an AST declaration
    std::unique_ptr<Symbol> symbolFromVarDecl(VarDecl* decl);
    std::unique_ptr<Symbol> symbolFromParamDecl(ParamDecl* decl);
    std::unique_ptr<Symbol> symbolFromFunctionDecl(FunctionDecl* decl);
    std::unique_ptr<Symbol> symbolFromStructDecl(StructDecl* decl);

    // Get parameter types from a function declaration
    std::vector<CgType> getParameterTypes(FunctionDecl* decl);

    // Format a function signature for error messages
    std::string formatFunctionSignature(const std::string& name,
                                         const std::vector<CgType>& paramTypes);
}
