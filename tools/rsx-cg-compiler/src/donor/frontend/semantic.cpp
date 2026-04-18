#include "semantic.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iostream>  // For debug output

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
    for (auto& decl : unit.declarations)
    {
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
}

void SemanticAnalyzer::collectVarDecl(VarDecl* decl)
{
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
    for (auto& decl : unit.declarations)
    {
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
    }
}

void SemanticAnalyzer::analyzeFunctionDecl(FunctionDecl* decl)
{
    // Skip prototypes and intrinsics
    if (decl->isPrototype() || decl->isIntrinsic) return;

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

        if (!symbols_.addSymbol(std::move(sym)))
        {
            error(param->loc, "duplicate parameter name '" + param->name + "'");
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
    if (!stmt->declaration) return;

    if (stmt->declaration->kind == DeclKind::Variable)
    {
        VarDecl* varDecl = static_cast<VarDecl*>(stmt->declaration.get());

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

        if (!symbols_.addSymbol(std::move(sym)))
        {
            error(varDecl->loc, "redefinition of variable '" + varDecl->name + "'");
        }

        // Analyze initializer
        analyzeVarDecl(varDecl);
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
        error(expr->loc, "use of undeclared identifier '" + expr->name + "'");
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

    // Check for any error types in arguments
    for (const auto& t : argTypes)
    {
        if (t.isError()) return CgType::Error();
    }

    // Resolve overload
    auto candidate = symbols_.resolveOverload(expr->functionName, argTypes);
    if (!candidate)
    {
        std::string sig = SymbolUtils::formatFunctionSignature(expr->functionName, argTypes);
        error(expr->loc, "no matching function for call to '" + sig + "'");
        return CgType::Error();
    }

    // Link to resolved function
    expr->resolvedFunction = candidate->symbol->declaration;

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
        return *fieldType;
    }

    // Check for swizzle on vector
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

    // Index must be integral
    if (!indexType.isIntegral())
    {
        error(expr->index->loc, "array index must have integral type");
        return CgType::Error();
    }

    // Check array/vector indexing
    if (arrayType.isArray())
    {
        return arrayType.elementType();
    }
    else if (arrayType.isVector())
    {
        // Indexing a vector returns a scalar
        return CgType::Scalar(arrayType.scalarKind());
    }
    else if (arrayType.isMatrix())
    {
        // Indexing a matrix returns a column vector
        return CgType::Vec(arrayType.scalarKind(), arrayType.matrixRows());
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

    for (auto& arg : expr->arguments)
    {
        CgType argType = analyzeExpr(arg.get());
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

        totalComponents += argType.componentCount();
    }

    if (hasError) return CgType::Error();

    // Check component count
    int requiredComponents = constructedType.componentCount();

    // Allow single scalar to broadcast, or exact match, or any count for struct
    if (totalComponents != requiredComponents &&
        !(expr->arguments.size() == 1 && totalComponents == 1) &&
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

    if (shaderInfo_.stage == ShaderStage::Vertex)
    {
        validateVertexShader();
    }
    else
    {
        validateFragmentShader();
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

    // DEBUG: Enable to trace parameter collection (keep commented when not debugging)
    #define DEBUG_SHADER_IO_COLLECTION 0
    #if DEBUG_SHADER_IO_COLLECTION
    std::cout << "[DEBUG] collectShaderIO: Entry point '" << entryPoint->name
              << "' has " << entryPoint->parameters.size() << " parameters\n";
    #endif

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

        // Check if it's a struct type - need to flatten members
        if (paramType.isStruct())
        {
            // Flatten struct members into individual parameters
            // Use the resolved type's structFields(), not the TypeNode's (which may be empty)
            std::string prefix = param->name;
            for (const auto& field : paramType.structFields())
            {
                if (!field.semantic.isEmpty())
                {
                    CgType fieldType = resolveType(field.type.get());
                    std::string fullName = prefix + "." + field.name;

                    ShaderIOParam ioParam(fullName, field.semantic.name,
                                          field.semantic.index, fieldType, isOutput);

                    if (isOutput)
                    {
                        shaderInfo_.outputParams.push_back(ioParam);

                        // Check for special semantics
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
            }
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
            // Non-struct input parameter without explicit semantic
            // Assign a default semantic based on parameter position
            // This handles cases like: float4 main(float2 position) : POSITION
            // where the input has no semantic but is still a shader input
            std::string defaultSemantic = "ATTR" + std::to_string(defaultAttrIndex++);

            ShaderIOParam ioParam(param->name, defaultSemantic, 0, paramType, false);
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
        for (const auto& field : returnStructType.structFields())
        {
            if (!field.semantic.isEmpty())
            {
                CgType fieldType = resolveType(field.type.get());
                ShaderIOParam ioParam("return." + field.name, field.semantic.name,
                                      field.semantic.index, fieldType, true);
                shaderInfo_.outputParams.push_back(ioParam);

                if (field.semantic.name == "POSITION")
                    shaderInfo_.hasPositionOutput = true;
                if (field.semantic.name == "COLOR")
                    shaderInfo_.hasColorOutput = true;
            }
        }
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
