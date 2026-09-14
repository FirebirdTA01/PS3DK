#pragma once

#include "types.h"
#include "ast.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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
    // WHERE THIS DECLARATION SITS IN THE UNIT, in parser order over top-level
    // declarations.  The reference resolves every call against the
    // declarations VISIBLE AT THAT CALL; our frontend is two-pass, so without
    // this the whole unit is visible from everywhere and a call resolves to
    // things written after it.  A source location cannot serve as the order -
    // an #included file's line numbers are not comparable to the includer's -
    // so this is a monotonic counter, not a position (t_36492ad8).
    size_t declIndex = 0;
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

    // A NAME USED HERE AND NOT FOUND.  The reference refuses a name that is
    // used and then DECLARED LATER IN THE SAME LEXICAL SCOPE ("error C1002:
    // the name X is already defined"), and accepts every other arrangement:
    // use outer then declare inner, use inner then declare outer, two sibling
    // blocks, and file-scope-later are all legal (measured by codex over nine
    // probes; t_17071b54).  So the record belongs to the SCOPE INSTANCE - not
    // to the function, which would over-refuse three legal shapes, and not to
    // the depth, which cannot tell siblings apart.
    void noteUnresolvedUse(const std::string& name) { unresolvedUses.insert(name); }
    bool hadUnresolvedUse(const std::string& name) const
    { return unresolvedUses.count(name) != 0; }

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
    std::unordered_set<std::string> unresolvedUses;
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
    // VISIBILITY IS PART OF RESOLUTION.  Only declarations with
    // declIndex <= visibleThrough take part; SIZE_MAX means "the whole unit",
    // for callers with no position to give.
    std::optional<OverloadCandidate> resolveOverload(
        const std::string& name,
        const std::vector<CgType>& argumentTypes,
        size_t visibleThrough = SIZE_MAX) const;

    // Add a function and handle overloading
    bool addFunction(const std::string& name,
                     const CgType& returnType,
                     const std::vector<CgType>& paramTypes,
                     const std::vector<std::string>& paramNames,
                     FunctionDecl* decl = nullptr,
                     bool isIntrinsic = false,
                     const std::string& opcode = "");

    // Does this NAME have any declaration visible at that point - a source
    // declaration with declIndex <= visibleThrough, or a builtin?  Asked to
    // separate "no such name" (the reference's C1008 class, which it reports
    // only inside entry-reachable functions) from "that name exists but no
    // overload of it fits this call" (C1103, unconditional).
    bool hasVisibleFunction(const std::string& name, size_t visibleThrough) const;

    // Record / query an unresolved use in the CURRENT scope instance; see
    // Scope::noteUnresolvedUse for why the scope and not the function.
    void noteUnresolvedUse(const std::string& name)
    { if (currentScope()) currentScope()->noteUnresolvedUse(name); }
    bool currentScopeHadUnresolvedUse(const std::string& name) const
    { return currentScope() && currentScope()->hadUnresolvedUse(name); }

    // THE DECLARATION CURSOR.  Every function registered from here on carries
    // this index.  Builtins are registered before any source declaration and
    // keep index 0, so they stay visible to every call; the semantic analyser
    // bumps this once per top-level source declaration, in parser order.  A
    // cursor rather than an addFunction parameter, because every builtin
    // registration would otherwise have to thread it through.
    void setDeclIndex(size_t index) { declIndexCursor_ = index; }

    // Register built-in functions and types
    void registerBuiltins();

    // Get scope depth
    size_t depth() const { return scopeStack.size(); }

private:
    size_t declIndexCursor_ = 0;
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
