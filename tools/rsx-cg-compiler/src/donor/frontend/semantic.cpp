#include "semantic.h"
#include <functional>
#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>
#include <iostream>  // For debug output

namespace
{
std::optional<int64_t> constantIntegerIndex(const ExprNode* expr)
{
    if (!expr)
        return std::nullopt;

    if (expr->kind == ExprKind::Literal)
    {
        const auto* lit = static_cast<const LiteralExpr*>(expr);
        if (lit->literalKind == LiteralExpr::LiteralKind::Int)
            return std::get<int64_t>(lit->value);
        return std::nullopt;
    }

    if (expr->kind == ExprKind::Unary)
    {
        const auto* unary = static_cast<const UnaryExpr*>(expr);
        if (unary->op != UnaryOp::Negate)
            return std::nullopt;

        std::optional<int64_t> value = constantIntegerIndex(unary->operand.get());
        if (!value || *value == std::numeric_limits<int64_t>::min())
            return std::nullopt;
        return -*value;
    }

    return std::nullopt;
}

bool constantIndexInRange(int elementCount, const std::optional<int64_t>& index)
{
    if (!index)
        return true;
    if (*index >= 0 && *index < elementCount)
        return true;
    return false;
}
}

// ============================================================================
// SemanticDiagnostic Implementation
// ============================================================================

std::string SemanticDiagnostic::toString() const
{
    std::string prefix;
    switch (severity)
    {
    case Severity::Error:   prefix = "error"; break;
    case Severity::Warning: prefix = "warning"; break;
    case Severity::Note:    prefix = "note"; break;
    }
    return loc.toString() + ": " + prefix + ": " + message;
}

// ============================================================================
// SemanticAnalyzer Implementation
// ============================================================================

SemanticAnalyzer::SemanticAnalyzer()
{
    // Register builtin types and functions
    symbols_.registerBuiltins();
}

SemanticAnalyzer::~SemanticAnalyzer() = default;

void SemanticAnalyzer::setShaderStage(ShaderStage stage)
{
    shaderInfo_.stage = stage;
}

void SemanticAnalyzer::setEntryPoint(const std::string& name)
{
    shaderInfo_.entryPointName = name;
}

bool SemanticAnalyzer::analyze(TranslationUnit& unit)
{
    diagnostics_.clear();

    // Pass 1: Collect all declarations into symbol table
    collectDeclarations(unit);

    if (hasErrors()) return false;

    // Pass 2: Analyze declarations (type-check function bodies, etc.)
    analyzeDeclarations(unit);

    if (hasErrors()) return false;

    // Pass 3: Validate shader-specific requirements
    validateShader();

    return !hasErrors();
}

bool SemanticAnalyzer::hasErrors() const
{
    for (const auto& diag : diagnostics_)
    {
        if (diag.isError()) return true;
    }
    return false;
}

int SemanticAnalyzer::errorCount() const
{
    int count = 0;
    for (const auto& diag : diagnostics_)
    {
        if (diag.isError()) count++;
    }
    return count;
}

int SemanticAnalyzer::warningCount() const
{
    int count = 0;
    for (const auto& diag : diagnostics_)
    {
        if (diag.isWarning()) count++;
    }
    return count;
}

void SemanticAnalyzer::error(const SourceLocation& loc, const std::string& message)
{
    diagnostics_.emplace_back(SemanticDiagnostic::Severity::Error, loc, message);
}

void SemanticAnalyzer::warning(const SourceLocation& loc, const std::string& message)
{
    diagnostics_.emplace_back(SemanticDiagnostic::Severity::Warning, loc, message);
}

void SemanticAnalyzer::note(const SourceLocation& loc, const std::string& message)
{
    diagnostics_.emplace_back(SemanticDiagnostic::Severity::Note, loc, message);
}

// ============================================================================
// Pass 1: Collect Declarations
// ============================================================================

void SemanticAnalyzer::collectDeclarations(TranslationUnit& unit)
{
    // ONE INDEX PER TOP-LEVEL DECLARATION, in parser order.  Pass 2 replays
    // the same order, so a call inside declaration #N sees exactly #0..#N -
    // its own index included, which is what makes direct recursion resolve.
    // Indices start at 1 so that 0 stays the builtins' "always visible"
    // (t_36492ad8).
    size_t declIndex = 0;
    for (auto& decl : unit.declarations)
    {
        symbols_.setDeclIndex(++declIndex);
        switch (decl->kind)
        {
        case DeclKind::Struct:
            collectStructDecl(static_cast<StructDecl*>(decl.get()));
            break;
        case DeclKind::Function:
            collectFunctionDecl(static_cast<FunctionDecl*>(decl.get()));
            break;
        case DeclKind::Variable:
            collectVarDecl(static_cast<VarDecl*>(decl.get()));
            break;
        case DeclKind::Buffer:
            collectBufferDecl(static_cast<BufferDecl*>(decl.get()));
            break;
        default:
            break;
        }
    }
    // Pass 2 must walk exactly this many; see analyzeDeclarations.
    declCountPass1_ = declIndex;

}

void SemanticAnalyzer::collectStructDecl(StructDecl* decl)
{
    // Create a CgType for this struct
    CgType structType = CgType::Struct(decl->name, decl->fields);

    // Add to type table
    if (!symbols_.addType(decl->name, structType))
    {
        error(decl->loc, "redefinition of struct '" + decl->name + "'");
        return;
    }

    // Also add as a symbol
    auto sym = SymbolUtils::symbolFromStructDecl(decl);
    symbols_.addSymbol(std::move(sym));
}

void SemanticAnalyzer::collectFunctionDecl(FunctionDecl* decl)
{
    // Where this NAME was first declared, for the same declaration-scope
    // binding rule the globals get.
    functionDeclIndex_.emplace(decl->name, symbols_.currentDeclIndex());

    // Get parameter types
    std::vector<CgType> paramTypes;
    std::vector<std::string> paramNames;

    for (const auto& param : decl->parameters)
    {
        paramTypes.push_back(CgType(param->type));
        paramNames.push_back(param->name);
    }

    CgType returnType(decl->returnType);

    // Add function to symbol table (handles overloading)
    if (!symbols_.addFunction(decl->name, returnType, paramTypes, paramNames,
                               decl, decl->isIntrinsic, decl->intrinsicOpcode))
    {
        // Not an error for overloads - addFunction handles that
    }

    // Track entry point
    if (decl->name == shaderInfo_.entryPointName)
    {
        if (shaderInfo_.entryPoint != nullptr)
        {
            error(decl->loc, "multiple definitions of entry point '" + decl->name + "'");
        }
        else
        {
            shaderInfo_.entryPoint = decl;
        }
    }

    // Every declaration, prototypes INCLUDED, in source order, for the C5122
    // walk in pass 3 (t_61109061).  A prototype carries no body, but it can
    // carry the SEMANTIC that is judged and it is what a call resolves to.
    allFunctions_.push_back(decl);

    // WHERE A DEFAULT VALUE IS LEGAL (t_4b54f26b A1).  Measured on the
    // reference, which has a named diagnostic for exactly this question:
    //
    //   float2 texcoord : TEXCOORD0 = {0.5, 0.25}   -> refused, C1114
    //   out float4 c : COLOR = {1,0,0,1}            -> refused, C1114
    //   float3 shade(float3 base, const bool b = true)  -> ACCEPTED
    //
    //     error C1114: only uniform parameters to the entry function can
    //     have default values: "texcoord"
    //
    // So a default is legal on a UNIFORM parameter of the ENTRY, and on any
    // parameter of a helper, and nowhere else.  This check lives here and
    // not in the parser because the parser does not know which function was
    // SELECTED - with -e the entry can be any function, and a check keyed on
    // the name "main" would refuse a legal default in the selected entry and
    // accept an illegal one in a function that merely happens to be called
    // main.  Measured control, one source compiled twice: a non-uniform
    // default on `alpha` is refused under -e alpha and accepted under
    // -e beta, where alpha is a helper.
    const bool isEntry = (decl->name == shaderInfo_.entryPointName);
    for (const auto& p : decl->parameters)
    {
        if (!p || !p->defaultValue) continue;
        if (isEntry)
        {
            if (p->storage != StorageQualifier::Uniform)
            {
                error(p->loc,
                      "only uniform parameters to the entry function can have "
                      "default values: \"" + p->name + "\"");
            }
        }
        // A default on a HELPER parameter is legal and its omitted argument
        // is materialised at the call site (t_36492ad8).  There is no else
        // branch: the interim refusal that used to live here is what this
        // slice removes.
    }
}

// The one default-value rule the general type checker cannot see
// (t_4b54f26b A1): a BRACED initialiser must supply exactly the declared
// component count and never broadcasts, while the parenthesised constructor
// it parses into DOES broadcast from a single argument.  Measured:
//     float4 u = float4(2)   ACCEPT [2,2,2,2]
//     float4 u = {2}         REFUSE  too little data
//     float3 u = {1,2}       REFUSE  too little data
//     float4 u = {1,2,3,4,5} REFUSE  too much data
// Braces and parentheses build the SAME ConstructorExpr, so the parser marks
// which was written and only that flag distinguishes them here.
//
// Everything else - category, dimensions, nesting, narrowing - is
// analyzeExpr + checkAssignment's job, called beside this one.  An earlier
// version of this function tried to do it all with component counts and was
// wrong in both directions across three review rounds.
void SemanticAnalyzer::checkParameterDefaultShape(ParamDecl* p)
{
    if (!p || !p->defaultValue || !p->type) return;
    // ARRAYS have their own initialiser rules - one ELEMENT per array slot -
    // and componentCount() describes the ELEMENT.  The reference ACCEPTS
    // `uniform float4 a[2] = { float4(1,2,3,4), float4(5,6,7,8) }`; an
    // earlier version of this check refused it, which is worse than the gap.
    // Array defaults are not measured yet (t_2b592fc7).
    if (p->type->arraySize > 0) return;
    if (p->defaultValue->kind != ExprKind::Constructor) return;
    const auto* ctor = static_cast<const ConstructorExpr*>(p->defaultValue.get());
    if (!ctor->bracedInitializer) return;

    const int declared = p->type->componentCount();
    const int args = static_cast<int>(ctor->arguments.size());
    if (declared <= 0 || args == declared) return;
    error(p->loc, std::string(args < declared ? "too little data in the "
                                                "default value for '"
                                              : "too much data in the "
                                                "default value for '") +
                  p->name + "': " + std::to_string(args) + " for a " +
                  std::to_string(declared) + "-component type");
}

void SemanticAnalyzer::collectVarDecl(VarDecl* decl)
{
    allGlobalVars_.push_back(decl);
    // The FIRST declaration of the name wins the index; a redefinition is
    // already reported below and must not move the boundary.
    globalDeclIndex_.emplace(decl->name, symbols_.currentDeclIndex());

    auto sym = SymbolUtils::symbolFromVarDecl(decl);
    if (!sym) return;

    // Resolve the type to include struct fields if it's a struct type
    sym->type = resolveType(decl->type.get());

    if (!symbols_.addSymbol(std::move(sym)))
    {
        error(decl->loc, "redefinition of variable '" + decl->name + "'");
    }

    // Check if this is a BUFFER semantic (struct uniform in named buffer)
    // e.g., "PerFrameVertexUniforms u_perVFrame : BUFFER[0]"
    if (decl->semantic.name == "BUFFER")
    {
        // Create a BufferDecl wrapper for unified handling
        auto bufferDecl = std::make_unique<BufferDecl>(decl->loc, decl->name, decl->semantic.index);
        bufferDecl->type = decl->type;
        shaderInfo_.bufferUniforms.push_back(bufferDecl.get());
        // Store ownership in a separate list to keep the pointer valid
        bufferDeclOwners_.push_back(std::move(bufferDecl));
    }
    // Track regular uniforms and attributes
    else if (decl->storage == StorageQualifier::Uniform)
    {
        shaderInfo_.uniforms.push_back(decl);
    }
}

void SemanticAnalyzer::collectBufferDecl(BufferDecl* decl)
{
    // Buffer declarations create a variable in the global scope
    auto sym = std::make_unique<Symbol>();
    sym->kind = SymbolKind::Variable;
    sym->name = decl->name;
    // Resolve the type to include struct fields if it's a struct type
    sym->type = resolveType(decl->type.get());
    sym->declaration = decl;
    sym->loc = decl->loc;

    if (!symbols_.addSymbol(std::move(sym)))
    {
        error(decl->loc, "redefinition of buffer '" + decl->name + "'");
    }

    // Track buffer declarations for parameter generation
    // Struct members will be flattened into qualified parameter names (e.g., "u_perVFrame.u_viewMatrix")
    shaderInfo_.bufferUniforms.push_back(decl);
}

// ============================================================================
// Pass 2: Analyze Declarations
// ============================================================================

void SemanticAnalyzer::analyzeDeclarations(TranslationUnit& unit)
{
    // The same walk as pass 1, so the counter reproduces the same indices.
    size_t declIndex = 0;
    for (auto& decl : unit.declarations)
    {
        visibleThrough_ = ++declIndex;
        switch (decl->kind)
        {
        case DeclKind::Struct:
            analyzeStructDecl(static_cast<StructDecl*>(decl.get()));
            break;
        case DeclKind::Function:
            analyzeFunctionDecl(static_cast<FunctionDecl*>(decl.get()));
            break;
        case DeclKind::Variable:
            analyzeVarDecl(static_cast<VarDecl*>(decl.get()));
            break;
        default:
            break;
        }
    }
    // WALK PARITY.  Pass 1 stamps a declIndex per top-level declaration and
    // pass 2 reproduces it by walking the same list the same way.  If the two
    // ever diverge - one loop filtering a declaration kind the other does not -
    // every index after the divergence is wrong and calls silently resolve
    // against the wrong set, with nothing to see.  Cheap to assert, impossible
    // to notice otherwise (review: Fable).
    if (declIndex != declCountPass1_)
    {
        SourceLocation unitLoc;
        error(unitLoc, "internal error: declaration walk parity - pass 1 "
                       "numbered " + std::to_string(declCountPass1_) +
                       " top-level declarations, pass 2 walked " +
                       std::to_string(declIndex) +
                       "; call visibility would be wrong (t_36492ad8)");
    }

}

void SemanticAnalyzer::analyzeStructDecl(StructDecl* decl)
{
    // Validate field types
    for (const auto& field : decl->fields)
    {
        CgType fieldType = resolveType(field.type.get());
        if (fieldType.isError())
        {
            error(decl->loc, "unknown type in struct field '" + field.name + "'");
        }
        else if (!field.semantic.isEmpty() && fieldType.isStruct())
        {
            error(decl->loc, "semantics on struct-typed members are not supported: '" + field.name + "'");
        }
    }
}

// A PROTOTYPE'S DEFAULTS ARE REAL AND HAVE TO BE RESOLVED.  Measured:
//
//     float inner(float x) { return x; }
//     float outer(float x, float k = inner(0.5));   // prototype
//     ... outer(t.x) ...                            // call
//     float outer(float x, float k) { ... }         // definition, no default
//
// the reference ACCEPTS this and emits the same container as passing
// inner(0.5) explicitly.  Rule 1 picks the default from the latest
// declaration VISIBLE AT THE CALL, which here is the prototype - so the
// prototype's expression is the one that gets substituted.  Skipping
// prototypes outright left that AST unanalysed and unresolved, and the IR
// builder then failed on an unresolved operand rather than diagnosing
// anything (found by codex).  Only the BODY is skipped now; the parameter
// scope and the default analysis are the same code the definition runs.
void SemanticAnalyzer::analyzePrototypeDefaults(FunctionDecl* decl)
{
    bool any = false;
    for (const auto& param : decl->parameters)
        if (param && param->defaultValue) { any = true; break; }
    if (!any) return;

    FunctionDecl* saved = currentFunction_;
    currentFunction_ = decl;
    symbols_.pushScope(Scope::Kind::Function);
    for (const auto& param : decl->parameters)
    {
        if (!param) continue;
        CgType resolvedType = resolveType(param->type.get());
        if (resolvedType.isError()) continue;
        auto sym = std::make_unique<Symbol>();
        sym->kind = SymbolKind::Parameter;
        sym->name = param->name;
        sym->type = resolvedType;
        sym->declaration = param.get();
        sym->loc = param->loc;
        sym->storage = param->storage;
        sym->semantic = param->semantic;
        symbols_.addSymbol(std::move(sym));
    }
    for (const auto& param : decl->parameters)
    {
        if (!param || !param->defaultValue) continue;
        const CgType resolvedType = resolveType(param->type.get());
        if (resolvedType.isError()) continue;
        const CgType defType = analyzeExpr(param->defaultValue.get());
        if (!defType.isError())
            checkAssignment(resolvedType, defType, param->defaultValue->loc);
        checkParameterDefaultShape(param.get());
    }
    symbols_.popScope();
    currentFunction_ = saved;
}

void SemanticAnalyzer::analyzeFunctionDecl(FunctionDecl* decl)
{
    if (decl->isIntrinsic) return;

    // A DECLARATION RULE, so it applies to a prototype as much as a body.
    checkDefaultNamesBindAtDeclaration(decl);

    if (decl->isPrototype())
    {
        analyzePrototypeDefaults(decl);
        return;
    }

    // Neither of these depends on a call, and both fire on programs that
    // never use the default at all.
    checkDuplicateDefinition(decl);

    currentFunction_ = decl;

    // Create function scope
    symbols_.pushScope(Scope::Kind::Function);

    // Add parameters to scope
    for (const auto& param : decl->parameters)
    {
        // Resolve the type (handles struct type lookup)
        CgType resolvedType = resolveType(param->type.get());
        if (resolvedType.isError())
        {
            error(param->loc, "unknown type for parameter '" + param->name + "'");
            continue;
        }

        // Create symbol with the resolved type
        auto sym = std::make_unique<Symbol>();
        sym->kind = SymbolKind::Parameter;
        sym->name = param->name;
        sym->type = resolvedType;
        sym->declaration = param.get();
        sym->loc = param->loc;
        sym->storage = param->storage;
        sym->semantic = param->semantic;

        if (!param->semantic.isEmpty() && resolvedType.isStruct())
        {
            error(param->loc, "semantics on struct-typed parameters are not supported: '" + param->name + "'");
        }

        if (!symbols_.addSymbol(std::move(sym)))
        {
            error(param->loc, "duplicate parameter name '" + param->name + "'");
        }

        // A uniform entry parameter's DEFAULT is type-checked exactly like a
        // variable's initialiser (t_4b54f26b A1).  This is the same
        // analyzeExpr + checkAssignment pair analyzeVarDecl uses, and using
        // it rather than hand-rolled arithmetic is the whole point: three
        // rounds of review found width-only rules wrong in both directions -
        // `float4 u = float(2)` and `float4 u = float4(float2(1,2),
        // float2(3,4))` are legal broadcasts/nestings that a component count
        // refuses, while `float4 u = float2x2(1,2,3,4)` and `float2x2 u =
        // float4(1,2,3,4)` are four components each and the reference refuses
        // BOTH because the CATEGORY differs.  The existing checker already
        // models category and dimensions; it just was never reached, because
        // parameters with defaults used to be refused in the parser.
        // EVERY parameter's default is analysed, not only an entry uniform's.
        // The reference checks a default expression when the FUNCTION IS
        // REACHED, not when the default is used - measured, three cells:
        //
        //   helper unreached, default names an undeclared id   ACCEPT
        //   helper REACHED, call supplies the argument         REFUSE C1008
        //   helper REACHED, default actually materialised      REFUSE C1102
        //
        // The middle one is the whole point: the call never uses the default
        // and the reference still refuses.  Analysing only on materialisation
        // (which is what the ir_builder does) accepts it, and analysing only
        // uniforms - as this did - never looked at a helper's default at all.
        //
        // The reachability gate comes free: an undeclared name in here routes
        // through deferOrEmitNameError, which holds C1008-class findings until
        // the reached set is known (t_36492ad8 commit 1).  currentFunction_ is
        // already this declaration, so the finding is attributed correctly.
        if (param->defaultValue && !resolvedType.isError())
        {
            const CgType defType = analyzeExpr(param->defaultValue.get());
            if (!defType.isError())
                checkAssignment(resolvedType, defType, param->defaultValue->loc);
            // The ONE rule the general checker cannot know: a BRACED
            // initialiser must supply exactly the declared component count
            // and never broadcasts, while the parenthesised constructor it
            // parses into does broadcast from one argument.  `float4 u = {2}`
            // is refused (too little data) and `float4 u = float4(2)` is
            // accepted, and the two are the same AST node apart from the flag.
            checkParameterDefaultShape(param.get());
        }
    }

    // Analyze function body
    if (decl->body)
    {
        analyzeBlockStmt(decl->body.get());
    }

    symbols_.popScope();
    currentFunction_ = nullptr;
}

void SemanticAnalyzer::analyzeVarDecl(VarDecl* decl)
{
    CgType varType = resolveType(decl->type.get());
    if (varType.isError())
    {
        error(decl->loc, "unknown type for variable '" + decl->name + "'");
        return;
    }

    // Check initializer
    if (decl->initializer)
    {
        CgType initType = analyzeExpr(decl->initializer.get());
        if (!initType.isError())
        {
            checkAssignment(varType, initType, decl->initializer->loc);
        }
    }
}

// ============================================================================
// Statement Analysis
// ============================================================================

void SemanticAnalyzer::analyzeStmt(StmtNode* stmt)
{
    if (!stmt) return;

    switch (stmt->kind)
    {
    case StmtKind::Block:
        analyzeBlockStmt(static_cast<BlockStmt*>(stmt));
        break;
    case StmtKind::If:
        analyzeIfStmt(static_cast<IfStmt*>(stmt));
        break;
    case StmtKind::For:
        analyzeForStmt(static_cast<ForStmt*>(stmt));
        break;
    case StmtKind::While:
        analyzeWhileStmt(static_cast<WhileStmt*>(stmt));
        break;
    case StmtKind::DoWhile:
        analyzeDoWhileStmt(static_cast<DoWhileStmt*>(stmt));
        break;
    case StmtKind::Switch:
        analyzeSwitchStmt(static_cast<SwitchStmt*>(stmt));
        break;
    case StmtKind::Return:
        analyzeReturnStmt(static_cast<ReturnStmt*>(stmt));
        break;
    case StmtKind::Expr:
        analyzeExprStmt(static_cast<ExprStmt*>(stmt));
        break;
    case StmtKind::Decl:
        analyzeDeclStmt(static_cast<DeclStmt*>(stmt));
        break;
    case StmtKind::Break:
        if (!inLoop_ && !inSwitch_)
        {
            error(stmt->loc, "'break' statement not in loop or switch");
        }
        break;
    case StmtKind::Continue:
        if (!inLoop_)
        {
            error(stmt->loc, "'continue' statement not in loop");
        }
        break;
    case StmtKind::Discard:
        if (shaderInfo_.stage != ShaderStage::Fragment)
        {
            error(stmt->loc, "'discard' can only be used in fragment shaders");
        }
        shaderInfo_.usesDiscard = true;
        break;
    case StmtKind::Empty:
    case StmtKind::Case:
    case StmtKind::Default:
        // These are handled in switch
        break;
    }
}

void SemanticAnalyzer::analyzeBlockStmt(BlockStmt* stmt)
{
    symbols_.pushScope(Scope::Kind::Block);

    for (auto& s : stmt->statements)
    {
        analyzeStmt(s.get());
    }

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeIfStmt(IfStmt* stmt)
{
    checkCondition(stmt->condition.get());
    analyzeStmt(stmt->thenBranch.get());
    if (stmt->elseBranch)
    {
        analyzeStmt(stmt->elseBranch.get());
    }
}

void SemanticAnalyzer::analyzeForStmt(ForStmt* stmt)
{
    symbols_.pushScope(Scope::Kind::Block);

    if (stmt->init)
    {
        analyzeStmt(stmt->init.get());
    }
    if (stmt->condition)
    {
        checkCondition(stmt->condition.get());
    }
    if (stmt->increment)
    {
        analyzeExpr(stmt->increment.get());
    }

    bool wasInLoop = inLoop_;
    inLoop_ = true;
    analyzeStmt(stmt->body.get());
    inLoop_ = wasInLoop;

    symbols_.popScope();
}

void SemanticAnalyzer::analyzeWhileStmt(WhileStmt* stmt)
{
    checkCondition(stmt->condition.get());

    bool wasInLoop = inLoop_;
    inLoop_ = true;
    analyzeStmt(stmt->body.get());
    inLoop_ = wasInLoop;
}

void SemanticAnalyzer::analyzeDoWhileStmt(DoWhileStmt* stmt)
{
    bool wasInLoop = inLoop_;
    inLoop_ = true;
    analyzeStmt(stmt->body.get());
    inLoop_ = wasInLoop;

    checkCondition(stmt->condition.get());
}

void SemanticAnalyzer::analyzeSwitchStmt(SwitchStmt* stmt)
{
    CgType switchType = analyzeExpr(stmt->expr.get());
    if (!switchType.isIntegral() && !switchType.isError())
    {
        error(stmt->expr->loc, "switch expression must have integral type");
    }

    bool wasInSwitch = inSwitch_;
    inSwitch_ = true;
    if (stmt->body)
    {
        analyzeBlockStmt(stmt->body.get());
    }
    inSwitch_ = wasInSwitch;
}

void SemanticAnalyzer::analyzeReturnStmt(ReturnStmt* stmt)
{
    if (!currentFunction_)
    {
        error(stmt->loc, "'return' statement outside of function");
        return;
    }

    CgType returnType(currentFunction_->returnType);

    if (stmt->value)
    {
        CgType valueType = analyzeExpr(stmt->value.get());
        if (!valueType.isError() && !returnType.isVoid())
        {
            checkAssignment(returnType, valueType, stmt->value->loc);
        }
        else if (returnType.isVoid())
        {
            error(stmt->loc, "void function should not return a value");
        }
    }
    else if (!returnType.isVoid())
    {
        error(stmt->loc, "non-void function should return a value");
    }
}

void SemanticAnalyzer::analyzeExprStmt(ExprStmt* stmt)
{
    if (stmt->expr)
    {
        analyzeExpr(stmt->expr.get());
    }
}

void SemanticAnalyzer::analyzeDeclStmt(DeclStmt* stmt)
{
    // EVERY declarator, not just the first: `float a, b;` declares both, and
    // analysing only the first is what made a declared name read as
    // undeclared later (t_a90b1ef1).  The existing redefinition check below
    // then also does its job on `float a, a;`, which used to compile.
    for (const auto& declaration : stmt->declarations)
    {
    if (!declaration) continue;

    if (declaration->kind == DeclKind::Variable)
    {
        VarDecl* varDecl = static_cast<VarDecl*>(declaration.get());

        // Resolve the type (handles struct type lookup)
        CgType resolvedType = resolveType(varDecl->type.get());
        if (resolvedType.isError())
        {
            error(varDecl->loc, "unknown type for variable '" + varDecl->name + "'");
            return;
        }

        // Create symbol with the resolved type
        auto sym = std::make_unique<Symbol>();
        sym->kind = SymbolKind::Variable;
        sym->name = varDecl->name;
        sym->type = resolvedType;
        sym->declaration = varDecl;
        sym->loc = varDecl->loc;
        sym->storage = varDecl->storage;
        sym->semantic = varDecl->semantic;
        sym->isConst = (varDecl->storage == StorageQualifier::Const);

        // USED EARLIER IN THIS SCOPE, DECLARED HERE.  The reference refuses
        // that outright - "error C1002: the name X is already defined" - and
        // it does so whether or not the function is reachable, so it is not
        // the deferred C1008 class.  Keyed by the SCOPE INSTANCE: use outer /
        // declare inner, use inner / declare outer, two sibling blocks and
        // file-scope-later are all LEGAL and must stay accepted (t_17071b54).
        if (symbols_.currentScopeHadUnresolvedUse(varDecl->name))
        {
            error(varDecl->loc, "the name '" + varDecl->name +
                                "' is already defined");
        }

        if (!symbols_.addSymbol(std::move(sym)))
        {
            error(varDecl->loc, "redefinition of variable '" + varDecl->name + "'");
        }

        // Analyze initializer
        analyzeVarDecl(varDecl);
    }
    }
}

// ============================================================================
// Expression Analysis
// ============================================================================

CgType SemanticAnalyzer::analyzeExpr(ExprNode* expr)
{
    if (!expr) return CgType::Error();

    CgType result;

    switch (expr->kind)
    {
    case ExprKind::Literal:
        result = analyzeLiteralExpr(static_cast<LiteralExpr*>(expr));
        break;
    case ExprKind::Identifier:
        result = analyzeIdentifierExpr(static_cast<IdentifierExpr*>(expr));
        break;
    case ExprKind::Binary:
        result = analyzeBinaryExpr(static_cast<BinaryExpr*>(expr));
        break;
    case ExprKind::Unary:
        result = analyzeUnaryExpr(static_cast<UnaryExpr*>(expr));
        break;
    case ExprKind::Call:
        result = analyzeCallExpr(static_cast<CallExpr*>(expr));
        break;
    case ExprKind::MemberAccess:
        result = analyzeMemberAccessExpr(static_cast<MemberAccessExpr*>(expr));
        break;
    case ExprKind::Index:
        result = analyzeIndexExpr(static_cast<IndexExpr*>(expr));
        break;
    case ExprKind::Ternary:
        result = analyzeTernaryExpr(static_cast<TernaryExpr*>(expr));
        break;
    case ExprKind::Cast:
        result = analyzeCastExpr(static_cast<CastExpr*>(expr));
        break;
    case ExprKind::Constructor:
        result = analyzeConstructorExpr(static_cast<ConstructorExpr*>(expr));
        break;
    case ExprKind::Sizeof:
        result = analyzeSizeofExpr(static_cast<SizeofExpr*>(expr));
        break;
    default:
        result = CgType::Error();
        break;
    }

    // Store resolved type in AST node
    expr->resolvedType = result.getNode();

    return result;
}

CgType SemanticAnalyzer::analyzeLiteralExpr(LiteralExpr* expr)
{
    switch (expr->literalKind)
    {
    case LiteralExpr::LiteralKind::Int:
        return CgType::Int();
    case LiteralExpr::LiteralKind::Float:
        if (expr->suffix == "h" || expr->suffix == "H")
            return CgType::Half();
        return CgType::Float();
    case LiteralExpr::LiteralKind::Bool:
        return CgType::Bool();
    default:
        return CgType::Error();
    }
}

CgType SemanticAnalyzer::analyzeIdentifierExpr(IdentifierExpr* expr)
{
    Symbol* sym = symbols_.lookup(expr->name);
    if (!sym)
    {
        deferOrEmitNameError(expr->loc,
                             "use of undeclared identifier '" + expr->name + "'",
                             expr->name);
        return CgType::Error();
    }

    // Link to declaration
    expr->resolvedDecl = sym->declaration;

    return sym->type;
}

CgType SemanticAnalyzer::analyzeBinaryExpr(BinaryExpr* expr)
{
    CgType leftType = analyzeExpr(expr->left.get());
    CgType rightType = analyzeExpr(expr->right.get());

    if (leftType.isError() || rightType.isError())
    {
        return CgType::Error();
    }

    // Check for assignment operators
    if (TypeOperations::isAssignmentOp(expr->op))
    {
        if (!isLvalue(expr->left.get()))
        {
            error(expr->left->loc, "expression is not assignable");
            return CgType::Error();
        }
        if (!checkAssignment(leftType, rightType, expr->right->loc))
        {
            return CgType::Error();
        }
        return leftType;
    }

    // Check operator validity
    if (!TypeOperations::isBinaryOpValid(expr->op, leftType, rightType))
    {
        error(expr->loc, "invalid operands to binary '" + binaryOpToString(expr->op) +
              "' (have '" + leftType.toString() + "' and '" + rightType.toString() + "')");
        return CgType::Error();
    }

    return TypeOperations::binaryOpResultType(expr->op, leftType, rightType);
}

CgType SemanticAnalyzer::analyzeUnaryExpr(UnaryExpr* expr)
{
    CgType operandType = analyzeExpr(expr->operand.get());

    if (operandType.isError())
    {
        return CgType::Error();
    }

    // Check for increment/decrement on lvalue
    if (expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PreDecrement ||
        expr->op == UnaryOp::PostIncrement || expr->op == UnaryOp::PostDecrement)
    {
        if (!isLvalue(expr->operand.get()))
        {
            error(expr->operand->loc, "expression is not assignable");
            return CgType::Error();
        }
    }

    if (!TypeOperations::isUnaryOpValid(expr->op, operandType))
    {
        error(expr->loc, "invalid argument type '" + operandType.toString() +
              "' to unary '" + unaryOpToString(expr->op) + "'");
        return CgType::Error();
    }

    return TypeOperations::unaryOpResultType(expr->op, operandType);
}

CgType SemanticAnalyzer::analyzeCallExpr(CallExpr* expr)
{
    // Analyze arguments first
    std::vector<CgType> argTypes;
    for (auto& arg : expr->arguments)
    {
        CgType argType = analyzeExpr(arg.get());
        argTypes.push_back(argType);
    }

    // RECORD THE UNRESOLVED CALLEE NAME BEFORE THE ERROR-ARGUMENT RETURN.
    // Recording is not diagnosing.  `f(missing(t)); float f;` is C1002 on the
    // reference - the later `float f` collides with the earlier use of the
    // name f - but the inner call makes the argument error-typed, and the
    // early return below skipped the outer call entirely, so the use of f was
    // never recorded and the collision could not be seen (review: codex).
    //
    // THE FIX IS NOT TO MOVE THE C1105 CHECK ABOVE THAT RETURN.  An
    // error-typed argument SUPPRESSES the diagnostics on its call, measured
    // both ways: `f(missing(t))` with no later declaration is ACCEPTED, and so
    // is a local f shadowing a visible function when the call's argument is
    // error-typed.  So the name is recorded here and nothing is emitted.
    // VISIBLE BINDING IDENTITY, NOT WHOLE-UNIT LOOKUP SUCCESS.  symbols_.lookup
    // has no visibility cutoff: pass 1 put every function in the table, so a
    // function declared AFTER this call is still "found" and the use went
    // unrecorded - `f(missing); float f;` with `float4 f(...)` later lost its
    // C1002 the same way the error-argument return did (review: codex, twice,
    // on two different lookups).
    //
    // A NON-FUNCTION binding still counts as visible here.  A file-scope
    // variable declared later is the separately carded t_17071b54 case, and
    // treating it as invisible would change that behaviour inside this commit
    // rather than on its own card.
    {
        Symbol* bound = symbols_.lookup(expr->functionName);
        const bool visibleHere =
            symbols_.hasVisibleFunction(expr->functionName, visibleThrough_) ||
            (bound && bound->kind != SymbolKind::Function &&
                      bound->kind != SymbolKind::Builtin);
        if (!visibleHere) symbols_.noteUnresolvedUse(expr->functionName);
    }

    // Check for any error types in arguments
    for (const auto& t : argTypes)
    {
        if (t.isError()) return CgType::Error();
    }

    // A NEARER BINDING WINS OVER THE FUNCTION OF THE SAME NAME.
    // `float4 f(float4 t){...}  float4 g(float4 t){ float f = 1; return f(t); }`
    // is C1105 "cannot call a non-function" on the reference: the LOCAL
    // shadows the function, so the call is a call on a float.
    //
    // This has to be asked BEFORE the overload set is consulted, not in the
    // failure path below - functionOverloads is keyed by NAME and knows
    // nothing about scopes, so resolveOverload SUCCEEDS here and a check
    // inside `if (!candidate)` is never reached (review: codex read the
    // ordering out of the source; Fable measured the cell).  Scope::lookup
    // already walks innermost-first, so the nearest binding is exactly what
    // symbols_.lookup returns and no scope-chain rewrite is needed.
    if (Symbol* nearest = symbols_.lookup(expr->functionName))
    {
        if (nearest->kind != SymbolKind::Function &&
            nearest->kind != SymbolKind::Builtin)
        {
            error(expr->loc, "cannot call '" + expr->functionName +
                             "': it is not a function");
            return CgType::Error();
        }
    }

    // Resolve overload
    bool ambiguousOverload = false;
    auto candidate = symbols_.resolveOverload(expr->functionName, argTypes,
                                              &ambiguousOverload, visibleThrough_);
    if (!candidate)
    {
        // THE THREE FAILURES ARE DIFFERENT THINGS AND THE ORDER MATTERS.
        //
        // AMBIGUOUS first: the call DID match, twice, so the name is plainly
        // visible and neither test below applies.  The reference calls this
        // C1101.
        //
        // Then NAME-NOT-FOUND versus NO-VIABLE-OVERLOAD.  Only the first is
        // the reference's C1008 class and only it is reachability-gated.  A
        // name with at least one VISIBLE declaration but no overload that fits
        // is C1103 and is reported everywhere - measured, and it holds even
        // when a later exact overload exists, which is why this asks about
        // visibility of the NAME and never about whole-unit resolvability
        // (review: codex).
        if (ambiguousOverload)
        {
            error(expr->loc, "ambiguous overloaded function reference '" +
                             expr->functionName + "'");
            return CgType::Error();
        }
        std::string sig = SymbolUtils::formatFunctionSignature(expr->functionName, argTypes);
        if (!symbols_.hasVisibleFunction(expr->functionName, visibleThrough_))
        {
            // THE NAME MAY EXIST AND SIMPLY NOT BE A FUNCTION.  `float missing
            // = 1; return missing(t);` is C1105 "cannot call a non-function"
            // on the reference and is NOT the C1008 class, so it is reported
            // unconditionally - deferring it let an unreachable body call an
            // int (review: codex).  A function declared LATER still resolves
            // to a Function symbol here, so it falls through to the deferral
            // and this does not swallow the forward-call case.
            Symbol* named = symbols_.lookup(expr->functionName);
            if (named && named->kind != SymbolKind::Function &&
                named->kind != SymbolKind::Builtin)
            {
                error(expr->loc, "cannot call '" + expr->functionName +
                                 "': it is not a function");
                return CgType::Error();
            }
            deferOrEmitNameError(expr->loc,
                                 "use of undeclared identifier '" +
                                 expr->functionName + "'",
                                 expr->functionName);
            return CgType::Error();
        }
        error(expr->loc, "no matching function for call to '" + sig + "'");
        return CgType::Error();
    }

    // Link to resolved function
    expr->resolvedFunction = candidate->symbol->declaration;

    // AN OMITTED out/inout ARGUMENT IS REFUSED AFTER RANKING, NOT BEFORE IT.
    //
    // Measured on sce-cgc 475 (codex's six cells; Fable retracted the
    // viability exclusion on them; reproduced here): a defaulted out/inout
    // parameter does NOT make its candidate non-viable.  It ranks normally and
    // ties with a plain overload of the supplied arity - `void f(float4)`
    // beside `void f(float4, out float k=.5)` called `f(t)` is C1101 in BOTH
    // declaration orders, exactly as a plain `float k=.5` second parameter is.
    // So the restriction cannot live in viability: excluding the candidate
    // there leaves `f(float4)` the unique winner and ACCEPTS four programs the
    // reference refuses (measured on the interim build that did exclude it).
    //
    // What the reference objects to is the ARGUMENT, not the arity: the
    // default expression it would substitute is not an lvalue, so it cannot
    // bind to an out parameter.  Its wording is reproduced verbatim, as the
    // two C1033 sites already do:
    //
    //     error C1111: non-lvalue actual parameter #2 cannot be out parameter ("o")
    //
    // That is a DIFFERENT refusal from C1103 "too few parameters" - a call
    // omitting an out parameter with no default fails arity above and never
    // reaches here - which is what lets the two rows tell each other apart.
    if (expr->resolvedFunction &&
        expr->resolvedFunction->kind == DeclKind::Function)
    {
        auto* resolved = static_cast<FunctionDecl*>(expr->resolvedFunction);
        for (size_t i = argTypes.size(); i < resolved->parameters.size(); ++i)
        {
            const auto& param = resolved->parameters[i];
            if (!param) continue;
            if (param->storage != StorageQualifier::Out &&
                param->storage != StorageQualifier::InOut)
            {
                continue;
            }
            error(expr->loc,
                  "error C1111: non-lvalue actual parameter #" +
                  std::to_string(i + 1) + " cannot be out parameter (\"" +
                  param->name + "\")");
            return CgType::Error();
        }
    }

    return candidate->symbol->type;
}

CgType SemanticAnalyzer::analyzeMemberAccessExpr(MemberAccessExpr* expr)
{
    CgType objectType = analyzeExpr(expr->object.get());

    if (objectType.isError())
    {
        return CgType::Error();
    }

    // Check for struct field access
    if (objectType.isStruct())
    {
        auto fieldType = objectType.getFieldType(expr->member);
        if (!fieldType)
        {
            error(expr->loc, "no member named '" + expr->member + "' in '" +
                  objectType.structName() + "'");
            return CgType::Error();
        }
        if (fieldType->getNode())
        {
            CgType resolved = resolveType(fieldType->getNode().get());
            if (!resolved.isError())
                return resolved;
        }
        return *fieldType;
    }

    // Check for swizzle on vector or scalar
    if (objectType.isVector() || objectType.isScalar())
    {
        validateSwizzle(expr, objectType);
        if (expr->swizzleLength > 0)
        {
            return objectType.swizzleType(expr->swizzleLength);
        }
        return CgType::Error();
    }

    error(expr->loc, "member reference base type '" + objectType.toString() +
          "' is not a structure or vector");
    return CgType::Error();
}

CgType SemanticAnalyzer::analyzeIndexExpr(IndexExpr* expr)
{
    CgType arrayType = analyzeExpr(expr->array.get());
    CgType indexType = analyzeExpr(expr->index.get());

    if (arrayType.isError() || indexType.isError())
    {
        return CgType::Error();
    }

    // An integral index, or a FLOATING one: the reference accepts a
    // float or half index with int() semantics - a constant truncates
    // toward zero (u[1.7] is u[1], u[-0.5] is u[0], u[4.0] on four
    // elements is C1068), and a run-time float index is the same address
    // register load as u[int(idx)] (t_050bebce).  The builder inserts the
    // truncation; the constant evaluator applies it.
    if (!indexType.isIntegral() && !indexType.isFloatingPoint())
    {
        error(expr->index->loc, "array index must have integral type");
        return CgType::Error();
    }

    std::optional<int64_t> constantIndex = constantIntegerIndex(expr->index.get());

    // Check array/vector indexing
    if (arrayType.isArray())
    {
        return arrayType.elementType();
    }
    else if (arrayType.isVector())
    {
        if (!constantIndexInRange(arrayType.vectorSize(), constantIndex))
        {
            error(expr->index->loc, "array index out of bounds for type '" +
                  arrayType.toString() + "'");
            return CgType::Error();
        }
        // Indexing a vector returns a scalar
        return CgType::Scalar(arrayType.scalarKind());
    }
    else if (arrayType.isMatrix())
    {
        // Indexing a matrix returns a column vector
        // M[i] is a ROW: a vector as wide as the matrix has COLUMNS.  Rows
        // and columns only coincide for square matrices (t_bc130064).
        return CgType::Vec(arrayType.scalarKind(), arrayType.matrixCols());
    }

    error(expr->array->loc, "subscripted value is not an array, vector, or matrix");
    return CgType::Error();
}

CgType SemanticAnalyzer::analyzeTernaryExpr(TernaryExpr* expr)
{
    checkCondition(expr->condition.get());

    CgType thenType = analyzeExpr(expr->thenExpr.get());
    CgType elseType = analyzeExpr(expr->elseExpr.get());

    if (thenType.isError() || elseType.isError())
    {
        return CgType::Error();
    }

    auto commonType = TypeOperations::commonType(thenType, elseType);
    if (!commonType)
    {
        error(expr->loc, "incompatible operand types in conditional expression ('" +
              thenType.toString() + "' and '" + elseType.toString() + "')");
        return CgType::Error();
    }

    return *commonType;
}

CgType SemanticAnalyzer::analyzeCastExpr(CastExpr* expr)
{
    CgType targetType = resolveType(expr->targetType.get());
    CgType operandType = analyzeExpr(expr->operand.get());

    if (targetType.isError() || operandType.isError())
    {
        return CgType::Error();
    }

    // A STRUCT FILL'S SOURCE KIND IS BOUNDED LIKE ITS LEAVES, and this check
    // has to sit OUTSIDE the conversion fallback below: a struct whose leaves
    // are all admitted answers TRUE from canExplicitlyConvert, so anything
    // placed in that fallback is skipped for exactly the shapes it is meant
    // to judge (codex).  A fixed, narrow-integer or UNSIGNED source carries
    // its own conversion - the reference reads (S)((fixed)3.0) as
    // 1.9990234375, (S)((short)65537) as 1, and an unsigned 2147483648 into a
    // signed leaf as -2147483648 - and this fill would write it through.
    // Those scalar casts are already wrong outside any struct, so the repair
    // is on their own card; what this refuses is EXPOSING them through a new
    // acceptance.
    if (targetType.isStruct() && operandType.isNumeric())
    {
        const ScalarKind kind = operandType.scalarKind();
        if (kind != ScalarKind::Float && kind != ScalarKind::Half &&
            kind != ScalarKind::Int)
        {
            error(expr->loc, "cannot cast from '" + operandType.toString() +
                  "' to '" + targetType.toString() + "'");
            return CgType::Error();
        }
    }

    // A SCALAR CAST TO A STRUCT fills every leaf - `OUT o = (OUT)0;`.  The
    // type rules answer it for a flat struct, but a NESTED field's TypeNode
    // may carry only the struct's name, and resolving that needs the symbol
    // table, which lives here rather than in types.cpp.
    if (!operandType.isExplicitlyConvertibleTo(targetType) &&
        operandType.isNumeric() && targetType.isStruct())
    {
        std::function<bool(const CgType&, int)> leavesAreNumeric =
            [&](const CgType& type, int depth) -> bool {
            if (depth > 8) return false;
            const auto& fields = type.structFields();
            if (fields.empty()) return false;
            for (const auto& field : fields)
            {
                if (!field.type) return false;
                const CgType resolved = resolveType(field.type.get());
                if (resolved.isError()) return false;
                if (resolved.isStruct())
                {
                    if (!leavesAreNumeric(resolved, depth + 1)) return false;
                    continue;
                }
                if (resolved.isArray()) return false;
                if (resolved.isMatrix()) return false;
                // Matrix leaves are refused with the rest of the boundary -
                // the fill converted through the matrix's base type rather
                // than its element type (codex).
                if (!resolved.isNumeric() && !resolved.isVector())
                    return false;
                // Only the leaf types the fill can convert - see the same
                // switch in types.cpp: bool, fixed and the narrow integers
                // each need their own conversion and are refused by name
                // rather than written through unchanged.
                switch (resolved.scalarKind())
                {
                case ScalarKind::Float:
                case ScalarKind::Half:
                case ScalarKind::Int:
                    break;
                default:
                    // UInt too: `(S)(-1)` into an unsigned leaf emitted -1
                    // where the reference wraps (codex).  The signedness
                    // conversion is as missing as bool's truth test.
                    return false;
                }
            }
            return true;
        };
        if (leavesAreNumeric(targetType, 0))
            return targetType;
    }

    if (!operandType.isExplicitlyConvertibleTo(targetType))
    {
        error(expr->loc, "cannot cast from '" + operandType.toString() +
              "' to '" + targetType.toString() + "'");
        return CgType::Error();
    }

    return targetType;
}

CgType SemanticAnalyzer::analyzeConstructorExpr(ConstructorExpr* expr)
{
    CgType constructedType = resolveType(expr->constructedType.get());

    if (constructedType.isError())
    {
        return CgType::Error();
    }

    // Analyze all arguments
    int totalComponents = 0;
    bool hasError = false;
    CgType singleArgType;
    std::vector<CgType> argTypes;

    for (auto& arg : expr->arguments)
    {
        CgType argType = analyzeExpr(arg.get());
        argTypes.push_back(argType);
        if (argType.isError())
        {
            hasError = true;
            continue;
        }

        if (!argType.isNumeric())
        {
            error(arg->loc, "constructor argument must be numeric type");
            hasError = true;
            continue;
        }

        if (expr->arguments.size() == 1)
        {
            singleArgType = argType;
        }

        totalComponents += argType.componentCount();
    }

    if (hasError) return CgType::Error();

    // Check component count.  A brace list initialising an ARRAY constructs
    // arraySize elements of the element type: `static const float2 taps[2] =
    // { float2(1,2), float2(3,4) }` needs 4 components, not 2, and the
    // reference accepts the constructor, nested and flat spellings
    // byte-identically (t_6a929b40, measured; nested braces are still the
    // parser's gap).
    int requiredComponents = constructedType.componentCount();
    if (constructedType.isArray())
        requiredComponents = constructedType.elementType().componentCount() * constructedType.arraySize();

    // Single-argument constructors in Cg are casts: narrowing is accepted,
    // widening is refused with "error C1033: cast not allowed" (t_d03921c3).
    bool allowSingleArg = false;
    if (expr->arguments.size() == 1 && !singleArgType.isError())
    {
        if (totalComponents == 1)
        {
            allowSingleArg = true;
        }
        else if (singleArgType.isVector() && (constructedType.isVector() || constructedType.isScalar()) &&
                 singleArgType.vectorSize() >= constructedType.vectorSize())
        {
            allowSingleArg = true;
        }
        else if (singleArgType.isMatrix() && constructedType.isMatrix() &&
                 singleArgType.matrixRows() >= constructedType.matrixRows() &&
                 singleArgType.matrixCols() >= constructedType.matrixCols())
        {
            allowSingleArg = true;
        }
        else if ((singleArgType.isVector() && constructedType.isVector() &&
                  singleArgType.vectorSize() < constructedType.vectorSize()) ||
                 (singleArgType.isMatrix() && constructedType.isMatrix() &&
                  (singleArgType.matrixRows() < constructedType.matrixRows() ||
                   singleArgType.matrixCols() < constructedType.matrixCols())))
        {
            error(expr->loc, "error C1033: cast not allowed");
            return CgType::Error();
        }
    }

    // A matrix constructor packs its arguments ROW-MAJOR, and a VECTOR
    // argument may not straddle a row boundary.  Measured on the reference
    // (2026-09-15, t_bc130064, .local/probe-rect m1-m11): float2x2(float2, s, s),
    // float2x2(s, s, float2), float3x3(float3, s, s, s, float3),
    // float3x4(float2, float2, float4, float4) and float4x4(eight float2) all
    // ACCEPT; float2x2(s, float2, s), float4x3(float2, float2, ...) and
    // float3x4(float3, float4, ...) are C5204, as are float3x4(f3,f3,f3,f3) and
    // float4x3(f4,f4,f4).  Mixed spellings are common in the SDK's static
    // const globals, so the rule is the cursor, not "one vector per row".
    if (constructedType.isMatrix() && expr->arguments.size() > 1)
    {
        const int cols = constructedType.matrixCols();
        int cursor = 0;
        for (const CgType& argType : argTypes)
        {
            const int width = argType.isVector() ? argType.vectorSize() : 1;
            if (width > 1 && (cursor % cols) + width > cols)
            {
                error(expr->loc, "error C5204: expression cannot be used to construct a " +
                      std::to_string(constructedType.matrixRows()) + "x" +
                      std::to_string(cols) + " matrix: a " + std::to_string(width) +
                      "-wide argument at component " + std::to_string(cursor) +
                      " would straddle a row of " + std::to_string(cols));
                return CgType::Error();
            }
            cursor += width;
        }
    }

    // Allow single scalar to broadcast, single narrowing, exact match, or any count for struct
    if (totalComponents != requiredComponents &&
        !allowSingleArg &&
        !constructedType.isStruct())
    {
        error(expr->loc, "constructor requires " + std::to_string(requiredComponents) +
              " components, but " + std::to_string(totalComponents) + " were provided");
        return CgType::Error();
    }

    return constructedType;
}

CgType SemanticAnalyzer::analyzeSizeofExpr(SizeofExpr* expr)
{
    // sizeof always returns int
    if (std::holds_alternative<std::shared_ptr<TypeNode>>(expr->operand))
    {
        CgType type = resolveType(std::get<std::shared_ptr<TypeNode>>(expr->operand).get());
        if (type.isError())
        {
            error(expr->loc, "sizeof applied to unknown type");
        }
    }
    else
    {
        analyzeExpr(std::get<std::unique_ptr<ExprNode>>(expr->operand).get());
    }

    return CgType::Int();
}

// ============================================================================
// Helper Methods
// ============================================================================

CgType SemanticAnalyzer::resolveType(TypeNode* typeNode) const
{
    if (!typeNode) return CgType::Error();

    // Check for struct types
    if (typeNode->baseType == BaseType::Struct && !typeNode->structName.empty())
    {
        auto type = symbols_.lookupType(typeNode->structName);
        if (!type)
        {
            return CgType::Error();
        }
        return *type;
    }

    return CgType(std::make_shared<TypeNode>(*typeNode));
}

bool SemanticAnalyzer::checkAssignment(const CgType& target, const CgType& value,
                                        const SourceLocation& loc)
{
    if (target.isError() || value.isError())
    {
        return false;
    }

    // Cg permits implicit vector narrowing on assignment, truncating
    // from the left (float4 -> float2 keeps xy).  Keep this local to
    // assignment checking so overload resolution and arithmetic do not
    // gain silent narrowing.
    if (target.isVector() && value.isVector() &&
        target.isNumeric() && value.isNumeric() &&
        value.vectorSize() > target.vectorSize())
    {
        return true;
    }

    if (!value.isAssignableTo(target))
    {
        error(loc, "cannot assign '" + value.toString() + "' to '" + target.toString() + "'");
        return false;
    }

    return true;
}

bool SemanticAnalyzer::checkCondition(ExprNode* expr)
{
    if (!expr) return false;

    CgType condType = analyzeExpr(expr);
    if (condType.isError()) return false;

    // Condition should be scalar (any numeric type is ok in Cg)
    if (!condType.isScalar())
    {
        error(expr->loc, "condition must be a scalar expression");
        return false;
    }

    return true;
}

bool SemanticAnalyzer::isLvalue(ExprNode* expr)
{
    if (!expr) return false;

    switch (expr->kind)
    {
    case ExprKind::Identifier:
        {
            IdentifierExpr* id = static_cast<IdentifierExpr*>(expr);
            Symbol* sym = symbols_.lookup(id->name);
            if (sym && sym->isConst)
            {
                return false;  // const variables are not assignable
            }
            return true;
        }
    case ExprKind::MemberAccess:
        {
            MemberAccessExpr* ma = static_cast<MemberAccessExpr*>(expr);
            return isLvalue(ma->object.get());
        }
    case ExprKind::Index:
        {
            IndexExpr* idx = static_cast<IndexExpr*>(expr);
            return isLvalue(idx->array.get());
        }
    case ExprKind::Unary:
        {
            UnaryExpr* unary = static_cast<UnaryExpr*>(expr);
            // Dereference would be an lvalue, but Cg doesn't have pointers
            return false;
        }
    default:
        return false;
    }
}

void SemanticAnalyzer::validateSwizzle(MemberAccessExpr* expr, const CgType& objectType)
{
    int maxComponents = objectType.vectorSize();
    if (maxComponents == 1) maxComponents = 1;  // Scalar has 1 component

    if (!SemanticUtils::isValidSwizzle(expr->member, maxComponents))
    {
        error(expr->loc, "invalid swizzle '" + expr->member +
              "' for type '" + objectType.toString() + "'");
        expr->swizzleLength = 0;
        return;
    }

    if (!SemanticUtils::parseSwizzle(expr->member, expr->swizzleIndices, expr->swizzleLength))
    {
        error(expr->loc, "invalid swizzle pattern '" + expr->member + "'");
        expr->swizzleLength = 0;
        return;
    }

    expr->isSwizzle = true;
}

// ============================================================================
// Shader Validation
// ============================================================================

void SemanticAnalyzer::validateShader()
{
    validateEntryPoint();

    // Held C1008-class findings, now that the entry is known and the call
    // graph is fully resolved.  Before checkNonEntrySemantics only so that a
    // name error inside a function reads before that function's semantic
    // error, which is the reference's order.
    emitDeferredNameFindings();

    checkNonEntrySemantics();

    if (shaderInfo_.stage == ShaderStage::Vertex)
    {
        validateVertexShader();
    }
    else
    {
        validateFragmentShader();
    }
}

// A function REACHED FROM THE SELECTED ENTRY may not carry a RETURN semantic
// (t_61109061).  MEASURED, because the diagnostic's own text is wrong about its
// rule - it says "semantics not allowed on functions other than the entry
// function", and the reference accepts two shapes that sentence forbids:
//
//   called helper, RETURN semantic                    REFUSE  C5122
//   called helper, RETURN + PARAMETER semantics       REFUSE  C5122
//   called helper, PARAMETER semantic only            ACCEPT
//   declared but never called, full semantics         ACCEPT
//   called only by a function the entry never calls   ACCEPT
//   transitively reachable through an intermediate    REFUSE  C5122
//   called only inside `if (false) ...`               REFUSE  C5122
//   called from a default-argument expression         ACCEPT
//
// So the rule is transitive reachability from the SELECTED entry, over the
// SYNTACTIC call graph, WITHOUT branch pruning - and default-argument
// expressions do not create edges.  Two consequences for the implementation:
//
//  - it cannot reuse a reachability pass that runs after constant folding.
//    The `if (false)` row is REFUSED by the reference, so the edge has to be
//    seen in the AST before anything prunes it.  ir_passes' m_reachableBlocks
//    is a different notion entirely: basic blocks within one function.
//  - it must walk EVERY container.  A missed node kind under-approximates
//    reachability, and under-approximating means ACCEPTING a program the
//    reference refuses - this check's own defect, reintroduced by its fix and
//    invisible to any fixture that happens to use a handled kind.  The
//    switches below therefore carry NO `default:` label, so an enum addition
//    is a compiler warning rather than a silent hole, and twelve container
//    fixtures cover it behaviourally.
// THE ONE REACHABILITY WALK.  Transitive and syntactic from the selected
// entry, with no branch pruning - a call inside `if (false)` still reaches,
// measured on the reference (t_61109061, and again for the C1008 class here).
// It runs AFTER pass 2 so CallExpr::resolvedFunction is fully populated;
// deciding reachability from a partly-resolved graph would under-approximate
// the reached set, and under-approximating means SUPPRESSING a diagnostic the
// reference emits (review: codex).
std::unordered_set<const FunctionDecl*> SemanticAnalyzer::reachedFunctions() const
{
    std::unordered_set<const FunctionDecl*> reached;
    FunctionDecl* entry = shaderInfo_.entryPoint;
    if (!entry) return reached;

    // The reached set holds the FIRST declaration of each function, because
    // that is the one the reference judges and the one a call resolves to.
    std::vector<FunctionDecl*> work;
    auto seed = [&](FunctionDecl* fn)
    {
        if (!fn) return;
        FunctionDecl* first = firstDeclarationOf(fn);
        if (!first) first = fn;
        if (reached.insert(first).second) work.push_back(first);
    };

    seed(entry);

    // A FILE-SCOPE INITIALISER IS A ROOT TOO.  `static float g = f(0.25);`
    // reaches f's body even though the entry never mentions f, and the
    // reference name-checks it there: that program is C1008 when f's body has
    // an undeclared name, where rooting only at the entry dropped the finding
    // AND skipped lowering f (review: codex).
    //
    // C5122 SHARES THIS ROOT SET - measured, not assumed, because widening a
    // separately-measured graph on a hunch is how the prototype hole got into
    // t_61109061.  A helper carrying a return semantic and called ONLY from a
    // static initialiser is refused C5122 by the reference, while the same
    // helper never called at all is accepted.  So one walk serves both.
    for (VarDecl* global : allGlobalVars_)
    {
        if (!global || !global->initializer) continue;
        std::vector<FunctionDecl*> callees;
        collectCallEdges(global->initializer.get(), callees);
        for (FunctionDecl* callee : callees) seed(callee);
    }

    while (!work.empty())
    {
        FunctionDecl* fn = work.back();
        work.pop_back();

        // WALK THE DEFINITION'S BODY, not this declaration's.  A call resolves
        // to the first declaration, which is usually a bodyless prototype;
        // stopping there under-approximates reachability.
        FunctionDecl* def = definitionOf(fn);
        if (!def || !def->body) continue;

        std::vector<FunctionDecl*> callees;
        collectCallEdges(def->body.get(), callees);
        for (FunctionDecl* callee : callees) seed(callee);
    }
    return reached;
}

// A NAME THE UNIT DOES NOT DECLARE AT THIS POINT.  The reference reports this
// class - C1008, covering an undeclared identifier, an undeclared function and
// a forward call - ONLY inside functions reachable from the selected entry.
// Measured, helper never called from main:
//
//     undeclared identifier / function / forward call   reference ACCEPTS
//     a type error (C1056) / a wrong arity (C1103)      reference REFUSES
//
// so the body IS analysed and only this class is gated.  Outside a function
// body there is nothing to be unreachable: a file-scope initialiser and an
// entry-parameter default are checked unconditionally, which the reference
// agrees with (it refuses a later name in both, C1002).
void SemanticAnalyzer::deferOrEmitNameError(const SourceLocation& loc,
                                            const std::string& message,
                                            const std::string& name)
{
    // Remember it against THIS scope instance: if the same scope later
    // declares the name, the reference calls that C1002 and refuses
    // regardless of reachability, so the deferral must not swallow it.
    if (!name.empty()) symbols_.noteUnresolvedUse(name);

    if (!currentFunction_)
    {
        error(loc, message);
        return;
    }
    deferredNameFindings_.push_back({currentFunction_, loc, message});
}

void SemanticAnalyzer::emitDeferredNameFindings()
{
    if (deferredNameFindings_.empty()) return;

    // No entry means validateEntryPoint has already refused; emitting every
    // held finding there would bury that diagnostic under a cascade.
    if (!shaderInfo_.entryPoint)
    {
        deferredNameFindings_.clear();
        return;
    }

    std::unordered_set<const FunctionDecl*> reached = reachedFunctions();
    for (const DeferredNameFinding& finding : deferredNameFindings_)
    {
        FunctionDecl* first = firstDeclarationOf(finding.function);
        if (!first) first = finding.function;
        if (reached.find(first) == reached.end()) continue;
        error(finding.loc, finding.message);
    }
    deferredNameFindings_.clear();
}

std::unordered_set<const FunctionDecl*>
SemanticAnalyzer::entryReachableDefinitions() const
{
    std::unordered_set<const FunctionDecl*> defs;
    for (const FunctionDecl* fn : reachedFunctions())
    {
        FunctionDecl* def = definitionOf(const_cast<FunctionDecl*>(fn));
        if (def) defs.insert(def);
    }
    return defs;
}

void SemanticAnalyzer::checkNonEntrySemantics()
{
    FunctionDecl* entry = shaderInfo_.entryPoint;
    if (!entry) return;   // already diagnosed by validateEntryPoint

    std::unordered_set<const FunctionDecl*> reached = reachedFunctions();

    // Source order, not hash order: deterministic diagnostics.
    for (FunctionDecl* fn : allFunctions_)
    {
        if (reached.find(fn) == reached.end()) continue;
        if (!fn || fn == entry) continue;
        if (fn->isIntrinsic) continue;          // builtin header, not user source
        if (fn->name == shaderInfo_.entryPointName) continue;
        if (fn->returnSemantic.name.empty()) continue;
        error(fn->loc,
              "semantics not allowed on functions other than the entry "
              "function: '" + fn->name + "'");
    }
}

bool SemanticAnalyzer::sameSignature(const FunctionDecl* a,
                                     const FunctionDecl* b) const
{
    if (!a || !b) return false;
    if (a->name != b->name) return false;
    if (a->parameters.size() != b->parameters.size()) return false;
    for (size_t i = 0; i < a->parameters.size(); ++i)
    {
        const auto& pa = a->parameters[i];
        const auto& pb = b->parameters[i];
        if (!pa || !pb) return false;
        if (!pa->type || !pb->type) return false;
        // RESOLVE AND COMPARE WHOLE TYPES.  A field-by-field comparison of the
        // TypeNode is a trap: an ARRAY carries its real type in elementType,
        // so `float[2]` and `float4[2]` agree on baseType, vectorSize,
        // matrixRows/Cols and arraySize and differ only underneath.  Comparing
        // the flat fields merged those two overloads, which made the semantic
        // of one apply to a call that resolved to the other - measured as BOTH
        // a new over-refusal and a missed refusal (t_61109061, found in review).
        // CgType::equals is the tree-walking identity the rest of the analyser
        // already uses; a second, shallower notion of "same type" living only
        // in this check is exactly how the two drift apart.
        const CgType ta = resolveType(pa->type.get());
        const CgType tb = resolveType(pb->type.get());
        if (!ta.equals(tb)) return false;
    }
    return true;
}

// TWO DEFINITIONS OF ONE SIGNATURE IS C1106, and it has nothing to do with
// defaults or with where the call sits.  Measured on sce-cgc 475:
//
//     def(k=.25) ; call ; def(k=.5)      C1106
//     def(k=.25) ; def(k=.5) ; call      C1106
//     def(k=.25) ; def(no default)       C1106, even with no call at all
//     PROTOTYPE(k=.25) ; def(no default) ; call(one argument)   C1103
//
// so it is the second BODY that is refused, not the second default, and a
// prototype/definition pair stays legal - the last cell is rule 1 doing its
// job (the latest visible declaration has no default, so the call is short an
// argument).  The reference reports it at the FIRST definition, which is what
// the location below reproduces.  Its wording is kept verbatim, as the C1033
// and C1111 sites already do.
void SemanticAnalyzer::checkDuplicateDefinition(FunctionDecl* decl)
{
    if (!decl || !decl->body) return;
    FunctionDecl* first = definitionOf(decl);
    if (!first || first == decl) return;
    error(first->loc, "error C1106: overloaded function declaration \"" +
                      decl->name + "\" differs only in parameter qualifiers");
}

// A DEFAULT'S NAMES BIND AT THE HELPER'S DECLARATION, not at the call and not
// at the end of the unit.  Measured:
//
//     f(x, k = K) {...}  then  static const float K = .75;  ->  C1002
//                              "the name K is already defined", reported at f
//     static const float K = .75;  then  f(x, k = K) {...}  ->  ACCEPT
//
// and the refusal does NOT depend on the default ever being used: supplying
// the argument at every call site still refuses, and a plain (non-const)
// global collides the same way.  So this is a binding rule, not a
// materialisation rule - which is why it is checked here, at the declaration,
// rather than anywhere near a call.
void SemanticAnalyzer::checkDefaultNamesBindAtDeclaration(FunctionDecl* decl)
{
    if (!decl) return;
    auto checkName = [&](const std::string& name, const SourceLocation&)
    {
        auto it = globalDeclIndex_.find(name);
        if (it != globalDeclIndex_.end() && it->second > visibleThrough_)
        {
            error(decl->loc, "error C1002: the name \"" + name +
                             "\" is already defined");
            return;
        }
        // A FUNCTION NAME IS NOT VISIBLE INSIDE ITS OWN DECLARATION, hence
        // >= rather than >.  `float f(float x = f()) {...}` is C1002 on the
        // reference - the declaration is not complete while its own default
        // is being read - and without this the call-edge walk expands that
        // default forever.  A MUTUAL pair still resolves: each names the
        // other, whose first declaration has a strictly smaller index.
        auto fn = functionDeclIndex_.find(name);
        if (fn != functionDeclIndex_.end() && fn->second >= visibleThrough_)
        {
            error(decl->loc, "error C1002: the name \"" + name +
                             "\" is already defined");
        }
    };
    std::function<void(const ExprNode*)> walk = [&](const ExprNode* expr)
    {
        if (!expr) return;
        if (expr->kind == ExprKind::Identifier)
        {
            checkName(static_cast<const IdentifierExpr*>(expr)->name, expr->loc);
            return;
        }
        switch (expr->kind)
        {
        case ExprKind::Binary:
        {
            const auto* b = static_cast<const BinaryExpr*>(expr);
            walk(b->left.get()); walk(b->right.get());
            break;
        }
        case ExprKind::Unary:
            walk(static_cast<const UnaryExpr*>(expr)->operand.get());
            break;
        case ExprKind::Call:
        {
            // THE CALLEE NAME IS A NAME TOO.  Walking only the arguments let
            // `f(x, k = inner(.5))` with inner declared LATER through, which
            // the reference refuses C1002 - and it only showed when the helper
            // was never reached, because a reached one is caught later by the
            // ordinary resolution path (found by codex).
            const auto* call = static_cast<const CallExpr*>(expr);
            checkName(call->functionName, expr->loc);
            for (const auto& a : call->arguments) walk(a.get());
            break;
        }
        case ExprKind::MemberAccess:
            walk(static_cast<const MemberAccessExpr*>(expr)->object.get());
            break;
        case ExprKind::Index:
        {
            const auto* i = static_cast<const IndexExpr*>(expr);
            walk(i->array.get()); walk(i->index.get());
            break;
        }
        case ExprKind::Ternary:
        {
            const auto* t = static_cast<const TernaryExpr*>(expr);
            walk(t->condition.get()); walk(t->thenExpr.get()); walk(t->elseExpr.get());
            break;
        }
        case ExprKind::Cast:
            walk(static_cast<const CastExpr*>(expr)->operand.get());
            break;
        case ExprKind::Constructor:
            for (const auto& a : static_cast<const ConstructorExpr*>(expr)->arguments)
                walk(a.get());
            break;
        case ExprKind::Literal:
        case ExprKind::Identifier:
        case ExprKind::Sizeof:
            break;
        // NO SILENT ARM.  A kind this switch does not list would simply not be
        // walked, and the rule would go unchecked inside it with nothing to
        // see (review: Fable).  ExprKind has exactly the eleven members above;
        // if one is added, this stops compiling rather than quietly skipping.
        }
    };
    for (const auto& param : decl->parameters)
    {
        if (param && param->defaultValue) walk(param->defaultValue.get());
    }
}

FunctionDecl* SemanticAnalyzer::firstDeclarationOf(const FunctionDecl* fn) const
{
    for (FunctionDecl* cand : allFunctions_)
        if (sameSignature(cand, fn)) return cand;
    return nullptr;
}

FunctionDecl* SemanticAnalyzer::definitionOf(const FunctionDecl* fn) const
{
    for (FunctionDecl* cand : allFunctions_)
        if (sameSignature(cand, fn) && cand->body) return cand;
    return nullptr;
}

// The two halves of the SYNTACTIC walk.  Deliberately exhaustive and
// deliberately without a `default:` - see checkNonEntrySemantics.
void SemanticAnalyzer::collectCallEdges(const StmtNode* stmt,
                                        std::vector<FunctionDecl*>& out) const
{
    if (!stmt) return;
    switch (stmt->kind)
    {
    case StmtKind::Expr:
        collectCallEdges(static_cast<const ExprStmt*>(stmt)->expr.get(), out);
        break;
    case StmtKind::Decl:
        for (const auto& d : static_cast<const DeclStmt*>(stmt)->declarations)
        {
            const auto* v = dynamic_cast<const VarDecl*>(d.get());
            if (v) collectCallEdges(v->initializer.get(), out);
        }
        break;
    case StmtKind::Block:
        for (const auto& st : static_cast<const BlockStmt*>(stmt)->statements)
            collectCallEdges(st.get(), out);
        break;
    case StmtKind::If:
    {
        const auto* node = static_cast<const IfStmt*>(stmt);
        collectCallEdges(node->condition.get(), out);
        collectCallEdges(node->thenBranch.get(), out);
        collectCallEdges(node->elseBranch.get(), out);
        break;
    }
    case StmtKind::For:
    {
        const auto* node = static_cast<const ForStmt*>(stmt);
        collectCallEdges(node->init.get(), out);
        collectCallEdges(node->condition.get(), out);
        collectCallEdges(node->increment.get(), out);
        collectCallEdges(node->body.get(), out);
        break;
    }
    case StmtKind::While:
    {
        const auto* node = static_cast<const WhileStmt*>(stmt);
        collectCallEdges(node->condition.get(), out);
        collectCallEdges(node->body.get(), out);
        break;
    }
    case StmtKind::DoWhile:
    {
        const auto* node = static_cast<const DoWhileStmt*>(stmt);
        collectCallEdges(node->body.get(), out);
        collectCallEdges(node->condition.get(), out);
        break;
    }
    case StmtKind::Switch:
    {
        const auto* node = static_cast<const SwitchStmt*>(stmt);
        collectCallEdges(node->expr.get(), out);
        collectCallEdges(node->body.get(), out);
        break;
    }
    case StmtKind::Case:
        collectCallEdges(static_cast<const CaseStmt*>(stmt)->value.get(), out);
        break;
    case StmtKind::Return:
        collectCallEdges(static_cast<const ReturnStmt*>(stmt)->value.get(), out);
        break;
    case StmtKind::Default:
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Discard:
    case StmtKind::Empty:
        break;   // carry no sub-statement and no sub-expression
    }
}

void SemanticAnalyzer::collectCallEdges(const ExprNode* expr,
                                        std::vector<FunctionDecl*>& out) const
{
    if (!expr) return;
    switch (expr->kind)
    {
    case ExprKind::Call:
    {
        const auto* node = static_cast<const CallExpr*>(expr);
        // Resolved by pass 2.  Keyed on the DECLARATION, never the spelling:
        // overloads share a name.
        if (node->resolvedFunction)
        {
            auto* fn = dynamic_cast<FunctionDecl*>(node->resolvedFunction);
            if (fn)
            {
                out.push_back(fn);
                // A MATERIALISED DEFAULT IS CODE AT THIS CALL SITE, so the
                // calls inside it are call edges OF THIS CALL - and only when
                // the argument is actually OMITTED.  Measured (codex's cells,
                // reproduced here):
                //
                //   float inner(float x) : COLOR { return x; }
                //   float outer(float x, float k = inner(1.0)) { return x*k; }
                //   outer(t.x)        reference REFUSES C5122 - inner is
                //                     reached, and a reached helper may not
                //                     carry a return semantic
                //   outer(t.x, 2.0)   reference ACCEPTS - the default is never
                //                     substituted, so inner is never reached
                //
                // That second cell is what stops the obvious over-fix: rooting
                // every call named in every default of every reached helper
                // would refuse a program the reference compiles.  The edge
                // belongs to the CALL SITE, not to the declaration.
                //
                // Transitive without extra machinery: the recursion below
                // walks the default expression, so a call inside it seeds its
                // own callee and omits its own defaults in turn, and the
                // seeded function's body is walked by the caller's work list -
                // which is how the relay cell (inner has no semantic, but
                // calls a leaf that does) is refused.
                // AND IT CAN BE CYCLIC.  `float f(float x = f()) {...}`
                // expands its own default forever; the reference refuses that
                // program outright (C1002/C1014) but we must not blow the
                // stack deciding so - and MUTUAL defaults that do terminate
                // (`f(x = g(1))` beside `g(x = f(1))`, called with an
                // argument) are ACCEPTED by the reference, so the guard has
                // to be an active set and not a depth limit or a
                // "seen once" set.
                for (size_t i = node->arguments.size();
                     i < fn->parameters.size(); ++i)
                {
                    const auto& param = fn->parameters[i];
                    if (!param || !param->defaultValue) continue;
                    const ExprNode* d = param->defaultValue.get();
                    if (!defaultsBeingWalked_.insert(d).second) continue;
                    collectCallEdges(d, out);
                    defaultsBeingWalked_.erase(d);
                }
            }
        }
        for (const auto& a : node->arguments) collectCallEdges(a.get(), out);
        break;
    }
    case ExprKind::Binary:
    {
        const auto* node = static_cast<const BinaryExpr*>(expr);
        collectCallEdges(node->left.get(), out);
        collectCallEdges(node->right.get(), out);
        break;
    }
    case ExprKind::Unary:
        collectCallEdges(static_cast<const UnaryExpr*>(expr)->operand.get(), out);
        break;
    case ExprKind::MemberAccess:
        collectCallEdges(static_cast<const MemberAccessExpr*>(expr)->object.get(), out);
        break;
    case ExprKind::Index:
    {
        const auto* node = static_cast<const IndexExpr*>(expr);
        collectCallEdges(node->array.get(), out);
        collectCallEdges(node->index.get(), out);
        break;
    }
    case ExprKind::Ternary:
    {
        const auto* node = static_cast<const TernaryExpr*>(expr);
        collectCallEdges(node->condition.get(), out);
        collectCallEdges(node->thenExpr.get(), out);
        collectCallEdges(node->elseExpr.get(), out);
        break;
    }
    case ExprKind::Cast:
        collectCallEdges(static_cast<const CastExpr*>(expr)->operand.get(), out);
        break;
    case ExprKind::Constructor:
        for (const auto& a : static_cast<const ConstructorExpr*>(expr)->arguments)
            collectCallEdges(a.get(), out);
        break;
    case ExprKind::Literal:
    case ExprKind::Identifier:
    case ExprKind::Sizeof:
        break;   // carry no sub-expression
    }
}

void SemanticAnalyzer::validateEntryPoint()
{
    if (!shaderInfo_.entryPoint)
    {
        error(SourceLocation{}, "entry point '" + shaderInfo_.entryPointName + "' not found");
        return;
    }

    collectShaderIO(shaderInfo_.entryPoint);
}

void SemanticAnalyzer::collectShaderIO(FunctionDecl* entryPoint)
{
    // Counter for assigning default semantics to undecorated parameters
    int defaultAttrIndex = 0;

    // Fragment-stage default-binding rule (matches reference compiler):
    // each non-uniform input parameter without an explicit semantic gets
    // the next-available TEXCOORD<N> slot — skipping any TEXCOORD<N>
    // already explicitly used by a sibling. Verified via byte probes of
    // unbound / mixed-binding shapes; resource code 0x0c94+N is written
    // but the semantic *string* is suppressed in the .fpo container.
    std::set<int> usedTexCoordIndices;
    if (shaderInfo_.stage == ShaderStage::Fragment)
    {
        for (const auto& param : entryPoint->parameters)
        {
            if (param->semantic.isEmpty()) continue;
            const std::string& sn = param->semantic.name;
            if (sn == "TEXCOORD" || sn == "texcoord" || sn == "TexCoord")
                usedTexCoordIndices.insert(param->semantic.index);
        }
    }
    int nextTexCoordIndex = 0;

    // DEBUG: Enable to trace parameter collection (keep commented when not debugging)
    #define DEBUG_SHADER_IO_COLLECTION 0
    #if DEBUG_SHADER_IO_COLLECTION
    std::cout << "[DEBUG] collectShaderIO: Entry point '" << entryPoint->name
              << "' has " << entryPoint->parameters.size() << " parameters\n";
    #endif

    auto flattenStructParams = [&](auto& self, const std::string& prefix, const CgType& sType, bool isOut) -> void {
        for (const auto& field : sType.structFields())
        {
            std::string fullName = prefix + "." + field.name;
            CgType fieldType = resolveType(field.type.get());
            if (!field.semantic.isEmpty())
            {
                if (fieldType.isStruct())
                {
                    error(entryPoint->loc, "semantics on struct-typed members are not supported: '" + fullName + "'");
                    return;
                }
                ShaderIOParam ioParam(fullName, field.semantic.name,
                                      field.semantic.index, fieldType, isOut);
                if (isOut)
                {
                    shaderInfo_.outputParams.push_back(ioParam);
                    if (field.semantic.name == "POSITION")
                        shaderInfo_.hasPositionOutput = true;
                    if (field.semantic.name == "COLOR")
                        shaderInfo_.hasColorOutput = true;
                }
                else
                {
                    shaderInfo_.inputParams.push_back(ioParam);
                }
            }
            else
            {
                if (fieldType.isStruct())
                {
                    self(self, fullName, fieldType, isOut);
                }
            }
        }
    };

    // Collect input parameters (attributes for vertex, varyings for fragment)
    // and output parameters
    for (const auto& param : entryPoint->parameters)
    {
        bool isOutput = (param->storage == StorageQualifier::Out ||
                         param->storage == StorageQualifier::InOut);

        #if DEBUG_SHADER_IO_COLLECTION
        std::cout << "[DEBUG]   Param '" << param->name << "': storage=";
        switch(param->storage) {
            case StorageQualifier::None: std::cout << "None"; break;
            case StorageQualifier::In: std::cout << "In"; break;
            case StorageQualifier::Out: std::cout << "Out"; break;
            case StorageQualifier::InOut: std::cout << "InOut"; break;
            case StorageQualifier::Uniform: std::cout << "Uniform"; break;
            default: std::cout << "?"; break;
        }
        std::cout << ", semantic=" << (param->semantic.isEmpty() ? "(empty)" : param->semantic.name)
                  << param->semantic.index << ", isOutput=" << isOutput << "\n";
        #endif

        // Resolve the parameter type
        CgType paramType = resolveType(param->type.get());

        if (paramType.isStruct())
        {
            flattenStructParams(flattenStructParams, param->name, paramType, isOutput);
        }
        else if (!param->semantic.isEmpty())
        {
            // Non-struct parameter with semantic
            ShaderIOParam ioParam(param->name, param->semantic.name,
                                  param->semantic.index, paramType, isOutput);

            if (isOutput)
            {
                shaderInfo_.outputParams.push_back(ioParam);

                if (param->semantic.name == "POSITION")
                    shaderInfo_.hasPositionOutput = true;
                if (param->semantic.name == "COLOR")
                    shaderInfo_.hasColorOutput = true;
            }
            else
            {
                shaderInfo_.inputParams.push_back(ioParam);
                #if DEBUG_SHADER_IO_COLLECTION
                std::cout << "[DEBUG]   -> Added to inputParams, count=" << shaderInfo_.inputParams.size() << "\n";
                #endif
            }
        }
        else if (param->storage == StorageQualifier::Uniform)
        {
            // Uniform function parameter - track it for GXP parameter table
            shaderInfo_.uniformParams.push_back(param.get());
        }
        else if (!isOutput)
        {
            // Non-struct input parameter without explicit semantic.
            // Assign a default binding the IR builder + emitter can
            // recognise:
            //   - fragment stage: TEXCOORD<N>, skipping any TEXCOORD<N>
            //     already used by a sibling parameter. Matches the
            //     reference compiler.
            //   - vertex stage: ATTR<N> in declaration order (existing
            //     behaviour, no test shader exercises it but kept).
            // The inferred semantic is written back into the AST so the
            // IR builder picks it up via param->semantic, and marked
            // `inferred = true` so the .fpo container emitter suppresses
            // the semantic *string* (the resource code carries enough
            // for the runtime to bind the slot).
            std::string defaultSemantic;
            int defaultIndex = 0;
            if (shaderInfo_.stage == ShaderStage::Fragment)
            {
                while (usedTexCoordIndices.count(nextTexCoordIndex))
                    ++nextTexCoordIndex;
                defaultSemantic = "TEXCOORD";
                defaultIndex    = nextTexCoordIndex++;
            }
            else
            {
                defaultSemantic = "ATTR";
                defaultIndex    = defaultAttrIndex++;
            }

            param->semantic.name     = defaultSemantic;
            param->semantic.rawName  = defaultSemantic + std::to_string(defaultIndex);
            param->semantic.index    = defaultIndex;
            param->semantic.inferred = true;

            ShaderIOParam ioParam(param->name, defaultSemantic, defaultIndex, paramType, false);
            shaderInfo_.inputParams.push_back(ioParam);
        }
    }

    // Check return type for semantics (common for simple shaders)
    if (!entryPoint->returnSemantic.isEmpty())
    {
        CgType returnType = resolveType(entryPoint->returnType.get());
        ShaderIOParam ioParam("return", entryPoint->returnSemantic.name,
                              entryPoint->returnSemantic.index, returnType, true);
        shaderInfo_.outputParams.push_back(ioParam);

        if (entryPoint->returnSemantic.name == "POSITION")
            shaderInfo_.hasPositionOutput = true;
        else if (entryPoint->returnSemantic.name == "COLOR")
            shaderInfo_.hasColorOutput = true;
    }

    // If return type is a struct, check for semantic annotations on its fields
    if (entryPoint->returnType && entryPoint->returnType->baseType == BaseType::Struct)
    {
        CgType returnStructType = resolveType(entryPoint->returnType.get());
        flattenStructParams(flattenStructParams, "return", returnStructType, true);
    }
}

void SemanticAnalyzer::validateVertexShader()
{
    // Vertex shaders must output POSITION
    if (!shaderInfo_.hasPositionOutput)
    {
        warning(shaderInfo_.entryPoint->loc,
                "vertex shader does not output POSITION - this may cause linking errors");
    }
}

void SemanticAnalyzer::validateFragmentShader()
{
    // Fragment shaders typically output COLOR, but it's not strictly required
    // if they use discard for all fragments
}

// ============================================================================
// SemanticUtils Implementation
// ============================================================================

namespace SemanticUtils
{

bool isValidSemantic(const Semantic& sem, ShaderStage stage, bool isInput)
{
    // Most semantics are valid in most contexts
    // This could be expanded with more specific validation
    return true;
}

SemanticCategory getSemanticCategory(const Semantic& sem)
{
    if (sem.name == "POSITION" || sem.name == "SV_POSITION") return SemanticCategory::Position;
    if (sem.name == "COLOR" || sem.name == "SV_TARGET") return SemanticCategory::Color;
    if (sem.name == "TEXCOORD") return SemanticCategory::TexCoord;
    if (sem.name == "NORMAL") return SemanticCategory::Normal;
    if (sem.name == "TANGENT") return SemanticCategory::Tangent;
    if (sem.name == "BINORMAL") return SemanticCategory::Binormal;
    if (sem.name == "BLENDWEIGHT") return SemanticCategory::BlendWeight;
    if (sem.name == "BLENDINDICES") return SemanticCategory::BlendIndices;
    if (sem.name == "DEPTH" || sem.name == "SV_DEPTH") return SemanticCategory::Depth;
    if (sem.name == "FOG") return SemanticCategory::Fog;
    if (sem.name == "PSIZE" || sem.name == "SV_POINTSIZE") return SemanticCategory::PointSize;
    // PSP2/Vita additional semantics
    if (sem.name == "INDEX" || sem.name == "SV_VERTEXID") return SemanticCategory::VertexIndex;
    if (sem.name == "INSTANCE" || sem.name == "SV_INSTANCEID") return SemanticCategory::InstanceIndex;
    if (sem.name == "SPRITECOORD" || sem.name == "POINTCOORD") return SemanticCategory::SpriteCoord;
    if (sem.name == "FRAGCOLOR") return SemanticCategory::FragColor;
    if (sem.name == "FACE" || sem.name == "VFACE" || sem.name == "SV_ISFRONTFACE") return SemanticCategory::Face;
    if (sem.name == "WPOS" || sem.name == "VPOS") return SemanticCategory::WorldPosition;
    // Clip planes CLP0-CLP7
    if (sem.name.length() >= 3 && sem.name.substr(0, 3) == "CLP") return SemanticCategory::ClipPlane;
    // Handle _HALF and _CENTROID suffixes
    if (sem.name.find("_HALF") != std::string::npos) return SemanticCategory::TexCoord;
    if (sem.name.find("_CENTROID") != std::string::npos)
    {
        // Extract base semantic before _CENTROID
        std::string base = sem.name.substr(0, sem.name.find("_CENTROID"));
        if (base == "COLOR") return SemanticCategory::Color;
        if (base == "TEXCOORD") return SemanticCategory::TexCoord;
        if (base == "FOG") return SemanticCategory::Fog;
        if (base == "WPOS") return SemanticCategory::WorldPosition;
    }
    return SemanticCategory::Other;
}

bool isValidSwizzle(const std::string& swizzle, int maxComponents)
{
    if (swizzle.empty() || swizzle.length() > 4)
    {
        return false;
    }

    // Check for valid swizzle characters
    bool usesXYZW = false;
    bool usesRGBA = false;
    bool usesSTPQ = false;

    for (char c : swizzle)
    {
        int index = -1;
        switch (c)
        {
        case 'x': case 'X': index = 0; usesXYZW = true; break;
        case 'y': case 'Y': index = 1; usesXYZW = true; break;
        case 'z': case 'Z': index = 2; usesXYZW = true; break;
        case 'w': case 'W': index = 3; usesXYZW = true; break;
        case 'r': case 'R': index = 0; usesRGBA = true; break;
        case 'g': case 'G': index = 1; usesRGBA = true; break;
        case 'b': case 'B': index = 2; usesRGBA = true; break;
        case 'a': case 'A': index = 3; usesRGBA = true; break;
        case 's': case 'S': index = 0; usesSTPQ = true; break;
        case 't': case 'T': index = 1; usesSTPQ = true; break;
        case 'p': case 'P': index = 2; usesSTPQ = true; break;
        case 'q': case 'Q': index = 3; usesSTPQ = true; break;
        default:
            return false;
        }

        if (index >= maxComponents)
        {
            return false;
        }
    }

    // Can't mix swizzle sets
    int setsUsed = (usesXYZW ? 1 : 0) + (usesRGBA ? 1 : 0) + (usesSTPQ ? 1 : 0);
    if (setsUsed > 1)
    {
        return false;
    }

    return true;
}

bool parseSwizzle(const std::string& swizzle, int swizzleIndices[4], int& length)
{
    length = static_cast<int>(swizzle.length());
    if (length == 0 || length > 4)
    {
        return false;
    }

    for (int i = 0; i < length; ++i)
    {
        char c = std::tolower(swizzle[i]);
        switch (c)
        {
        case 'x': case 'r': case 's': swizzleIndices[i] = 0; break;
        case 'y': case 'g': case 't': swizzleIndices[i] = 1; break;
        case 'z': case 'b': case 'p': swizzleIndices[i] = 2; break;
        case 'w': case 'a': case 'q': swizzleIndices[i] = 3; break;
        default:
            return false;
        }
    }

    return true;
}

} // namespace SemanticUtils
