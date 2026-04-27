#include "ir_builder.h"
#include <sstream>

// ============================================================================
// Constructor/Destructor
// ============================================================================

IRBuilder::IRBuilder() = default;
IRBuilder::~IRBuilder() = default;

// ============================================================================
// Error Handling
// ============================================================================

void IRBuilder::error(const std::string& msg)
{
    errors_.push_back("IR generation error: " + msg);
}

void IRBuilder::error(const SourceLocation& loc, const std::string& msg)
{
    errors_.push_back(loc.toString() + ": IR generation error: " + msg);
}

std::string IRBuilder::makeLabel(const std::string& prefix)
{
    return prefix + std::to_string(labelCounter_++);
}

// ============================================================================
// Main Build Entry Point
// ============================================================================

std::unique_ptr<IRModule> IRBuilder::build(TranslationUnit& unit, const SemanticAnalyzer& semantic)
{
    semantic_ = &semantic;
    module_ = std::make_unique<IRModule>(unit.filename);

    // Set shader stage
    module_->shaderStage = semantic.shaderInfo().stage;
    module_->entryPointName = semantic.shaderInfo().entryPointName;

    // Build globals (uniforms, attributes, etc.)
    buildGlobals(unit);

    // Build functions
    for (auto& decl : unit.declarations)
    {
        if (decl->kind == DeclKind::Function)
        {
            auto* funcDecl = static_cast<FunctionDecl*>(decl.get());
            if (!funcDecl->isPrototype() && !funcDecl->isIntrinsic)
            {
                buildFunction(funcDecl);
            }
        }
    }

    // Set entry point
    module_->entryPoint = module_->getFunction(module_->entryPointName);

    return std::move(module_);
}

// ============================================================================
// Global Building
// ============================================================================

void IRBuilder::buildGlobals(TranslationUnit& unit)
{
    for (auto& decl : unit.declarations)
    {
        if (decl->kind == DeclKind::Variable)
        {
            auto* varDecl = static_cast<VarDecl*>(decl.get());

            IRGlobal global;
            global.name = varDecl->name;
            global.type = getIRType(varDecl->type.get());
            global.valueId = module_->allocateGlobalId();
            global.storage = varDecl->storage;

            if (!varDecl->semantic.isEmpty())
            {
                global.semanticName = varDecl->semantic.name;
                global.semanticIndex = varDecl->semantic.index;
            }

            // Carry explicit `: register(CN)` binding into the global so
            // the const allocator can pin the matrix/vector to the
            // requested constant register instead of the default.
            if (varDecl->semantic.hasExplicitRegister())
            {
                global.explicitRegisterBank  = varDecl->semantic.explicitRegisterBank;
                global.explicitRegisterIndex = varDecl->semantic.explicitRegisterIndex;
            }

            module_->addGlobal(global);

            // Map declaration to value.  Uniform globals are intentionally
            // *not* inserted into nameToValue_ — buildIdentifierExpr falls
            // through to module_->findGlobal() and emits a LoadUniform so
            // the lowering can resolve the source const-bank slot.  Other
            // globals stay in nameToValue_ (existing behaviour).
            declToValue_[varDecl] = global.valueId;
            if (varDecl->storage != StorageQualifier::Uniform)
            {
                nameToValue_[varDecl->name] = global.valueId;
            }

            // For uniform struct types, also flatten members into separate globals
            // so that "u_data.color" and "u_data.scale" get their own uniform slots
            if (varDecl->storage == StorageQualifier::Uniform &&
                varDecl->type && varDecl->type->baseType == BaseType::Struct)
            {
                const std::vector<StructField>* fields = getStructFields(varDecl->type.get());
                if (fields)
                {
                    std::string prefix = varDecl->name;
                    for (const auto& field : *fields)
                    {
                        std::string qualifiedName = prefix + "." + field.name;
                        IRTypeInfo fieldIRType = getIRType(field.type.get());

                        IRGlobal memberGlobal;
                        memberGlobal.name = qualifiedName;
                        memberGlobal.type = fieldIRType;
                        memberGlobal.valueId = module_->allocateGlobalId();
                        memberGlobal.storage = StorageQualifier::Uniform;

                        module_->addGlobal(memberGlobal);
                        nameToValue_[qualifiedName] = memberGlobal.valueId;
                    }
                }
            }
        }
        else if (decl->kind == DeclKind::Buffer)
        {
            // Buffer declarations (BUFFER[N]) - flatten struct members into individual globals
            // e.g., "u_perVFrame : BUFFER[0]" with struct containing u_viewMatrix, u_projectionMatrix
            // becomes globals: "u_perVFrame.u_viewMatrix", "u_perVFrame.u_projectionMatrix"
            auto* bufferDecl = static_cast<BufferDecl*>(decl.get());
            CgType bufferType = semantic_->resolveType(bufferDecl->type.get());

            if (bufferType.isStruct())
            {
                std::string prefix = bufferDecl->name;

                // Flatten each struct field into a separate global
                for (const auto& field : bufferType.structFields())
                {
                    std::string qualifiedName = prefix + "." + field.name;
                    IRTypeInfo fieldIRType = getIRType(field.type.get());

                    IRGlobal global;
                    global.name = qualifiedName;
                    global.type = fieldIRType;
                    global.valueId = module_->allocateGlobalId();
                    global.storage = StorageQualifier::Uniform;  // Buffer members act as uniforms
                    global.semanticName = "BUFFER";
                    global.semanticIndex = bufferDecl->bufferIndex;

                    module_->addGlobal(global);

                    // Map the qualified name to value for member access lookups
                    nameToValue_[qualifiedName] = global.valueId;
                }
            }
        }
    }
}

// ============================================================================
// Function Building
// ============================================================================

void IRBuilder::buildFunction(FunctionDecl* decl)
{
    currentFunction_ = module_->createFunction(decl->name);
    currentFunction_->returnType = getIRType(decl->returnType.get());
    currentFunction_->isEntryPoint = (decl->name == module_->entryPointName);

    // Build parameters
    for (auto& param : decl->parameters)
    {
        IRParameter irParam;
        irParam.name = param->name;
        irParam.type = getIRType(param->type.get());
        irParam.valueId = currentFunction_->allocateValueId();
        irParam.storage = param->storage;

        if (!param->semantic.isEmpty())
        {
            irParam.semanticName    = param->semantic.name;
            irParam.rawSemanticName = param->semantic.rawName;
            irParam.semanticIndex   = param->semantic.index;
            irParam.inferredSemantic = param->semantic.inferred;
        }

        currentFunction_->parameters.push_back(irParam);

        // Map parameter to value
        declToValue_[param.get()] = irParam.valueId;
        nameToValue_[param->name] = irParam.valueId;
    }

    // Create entry block
    currentBlock_ = currentFunction_->createBlock("entry");

    // Build function body
    if (decl->body)
    {
        buildBlockStmt(decl->body.get());
    }

    // Ensure function has a terminator
    if (!currentBlock_->hasTerminator())
    {
        if (currentFunction_->returnType.baseType == IRType::Void)
        {
            emitReturn(InvalidIRValue);
        }
        else
        {
            // Return undefined value (shouldn't happen in well-formed code)
            auto inst = std::make_unique<IRInstruction>(IROp::Undef,
                currentFunction_->allocateValueId(), currentFunction_->returnType);
            currentBlock_->addInstruction(std::move(inst));
            emitReturn(currentFunction_->nextValueId - 1);
        }
    }

    // Clean up
    declToValue_.clear();
    nameToValue_.clear();
    currentFunction_ = nullptr;
    currentBlock_ = nullptr;
}

// ============================================================================
// Statement Building
// ============================================================================

void IRBuilder::buildStmt(StmtNode* stmt)
{
    if (!stmt) return;

    switch (stmt->kind)
    {
    case StmtKind::Block:
        buildBlockStmt(static_cast<BlockStmt*>(stmt));
        break;
    case StmtKind::If:
        buildIfStmt(static_cast<IfStmt*>(stmt));
        break;
    case StmtKind::For:
        buildForStmt(static_cast<ForStmt*>(stmt));
        break;
    case StmtKind::While:
        buildWhileStmt(static_cast<WhileStmt*>(stmt));
        break;
    case StmtKind::DoWhile:
        buildDoWhileStmt(static_cast<DoWhileStmt*>(stmt));
        break;
    case StmtKind::Switch:
        buildSwitchStmt(static_cast<SwitchStmt*>(stmt));
        break;
    case StmtKind::Return:
        buildReturnStmt(static_cast<ReturnStmt*>(stmt));
        break;
    case StmtKind::Break:
        buildBreakStmt(static_cast<BreakStmt*>(stmt));
        break;
    case StmtKind::Continue:
        buildContinueStmt(static_cast<ContinueStmt*>(stmt));
        break;
    case StmtKind::Discard:
        buildDiscardStmt(static_cast<DiscardStmt*>(stmt));
        break;
    case StmtKind::Expr:
        buildExprStmt(static_cast<ExprStmt*>(stmt));
        break;
    case StmtKind::Decl:
        buildDeclStmt(static_cast<DeclStmt*>(stmt));
        break;
    default:
        error("Unknown statement kind");
        break;
    }
}

void IRBuilder::buildBlockStmt(BlockStmt* stmt)
{
    for (auto& s : stmt->statements)
    {
        buildStmt(s.get());
    }
}

void IRBuilder::buildIfStmt(IfStmt* stmt)
{
    // Evaluate condition
    IRValueID condValue = buildExpr(stmt->condition.get());

    // Create blocks
    IRBasicBlock* thenBlock = currentFunction_->createBlock(makeLabel("if.then"));
    IRBasicBlock* elseBlock = stmt->elseBranch
        ? currentFunction_->createBlock(makeLabel("if.else"))
        : nullptr;
    IRBasicBlock* mergeBlock = currentFunction_->createBlock(makeLabel("if.end"));

    // Emit conditional branch
    emitCondBranch(condValue, thenBlock, elseBlock ? elseBlock : mergeBlock);

    // Build then block
    currentBlock_ = thenBlock;
    buildStmt(stmt->thenBranch.get());
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(mergeBlock);
    }

    // Build else block
    if (stmt->elseBranch)
    {
        currentBlock_ = elseBlock;
        buildStmt(stmt->elseBranch.get());
        if (!currentBlock_->hasTerminator())
        {
            emitBranch(mergeBlock);
        }
    }

    // Continue from merge block
    currentBlock_ = mergeBlock;
}

namespace {

bool stmtContainsBreakOrContinue(StmtNode* stmt)
{
    if (!stmt) return false;
    switch (stmt->kind)
    {
    case StmtKind::Break:
    case StmtKind::Continue:
        return true;
    case StmtKind::Block: {
        auto* b = static_cast<BlockStmt*>(stmt);
        for (auto& s : b->statements)
            if (stmtContainsBreakOrContinue(s.get())) return true;
        return false;
    }
    case StmtKind::If: {
        auto* i = static_cast<IfStmt*>(stmt);
        return stmtContainsBreakOrContinue(i->thenBranch.get()) ||
               stmtContainsBreakOrContinue(i->elseBranch.get());
    }
    // Inner loops/switch shadow the enclosing loop's break/continue
    // targets, so anything found there does not escape.
    case StmtKind::For:
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::Switch:
        return false;
    default:
        return false;
    }
}

bool exprIsIntLiteral(ExprNode* e, int32_t* out)
{
    if (!e || e->kind != ExprKind::Literal) return false;
    auto* lit = static_cast<LiteralExpr*>(e);
    if (lit->literalKind != LiteralExpr::LiteralKind::Int) return false;
    *out = static_cast<int32_t>(std::get<int64_t>(lit->value));
    return true;
}

}  // namespace

bool IRBuilder::tryUnrollStaticFor(ForStmt* stmt)
{
    if (!stmt->init || !stmt->condition || !stmt->increment || !stmt->body)
        return false;

    std::string varName;
    int32_t curVal = 0;

    // Init: `int i = K` (DeclStmt) or `i = K` (ExprStmt assignment).
    if (stmt->init->kind == StmtKind::Decl)
    {
        auto* declStmt = static_cast<DeclStmt*>(stmt->init.get());
        if (!declStmt->declaration ||
            declStmt->declaration->kind != DeclKind::Variable)
            return false;
        auto* var = static_cast<VarDecl*>(declStmt->declaration.get());
        if (!var->initializer) return false;
        if (!exprIsIntLiteral(var->initializer.get(), &curVal)) return false;
        varName = var->name;
    }
    else if (stmt->init->kind == StmtKind::Expr)
    {
        auto* exprStmt = static_cast<ExprStmt*>(stmt->init.get());
        if (!exprStmt->expr || exprStmt->expr->kind != ExprKind::Binary)
            return false;
        auto* be = static_cast<BinaryExpr*>(exprStmt->expr.get());
        if (be->op != BinaryOp::Assign) return false;
        if (be->left->kind != ExprKind::Identifier) return false;
        varName = static_cast<IdentifierExpr*>(be->left.get())->name;
        if (!exprIsIntLiteral(be->right.get(), &curVal)) return false;
    }
    else
    {
        return false;
    }

    // Condition: `varName CMP int_literal`.
    if (stmt->condition->kind != ExprKind::Binary) return false;
    auto* cond = static_cast<BinaryExpr*>(stmt->condition.get());
    BinaryOp cmpOp = cond->op;
    if (cmpOp != BinaryOp::Less && cmpOp != BinaryOp::LessEqual &&
        cmpOp != BinaryOp::Greater && cmpOp != BinaryOp::GreaterEqual &&
        cmpOp != BinaryOp::Equal && cmpOp != BinaryOp::NotEqual)
        return false;
    if (cond->left->kind != ExprKind::Identifier) return false;
    if (static_cast<IdentifierExpr*>(cond->left.get())->name != varName)
        return false;
    int32_t cmpVal = 0;
    if (!exprIsIntLiteral(cond->right.get(), &cmpVal)) return false;

    // Increment: `i++`, `++i`, `i--`, `--i`, `i += K`, `i -= K`.
    int32_t incDelta = 0;
    auto* incExpr = stmt->increment.get();
    if (incExpr->kind == ExprKind::Unary)
    {
        auto* un = static_cast<UnaryExpr*>(incExpr);
        if (un->operand->kind != ExprKind::Identifier) return false;
        if (static_cast<IdentifierExpr*>(un->operand.get())->name != varName)
            return false;
        switch (un->op)
        {
        case UnaryOp::PreIncrement:
        case UnaryOp::PostIncrement: incDelta = 1; break;
        case UnaryOp::PreDecrement:
        case UnaryOp::PostDecrement: incDelta = -1; break;
        default: return false;
        }
    }
    else if (incExpr->kind == ExprKind::Binary)
    {
        auto* be = static_cast<BinaryExpr*>(incExpr);
        if (be->left->kind != ExprKind::Identifier) return false;
        if (static_cast<IdentifierExpr*>(be->left.get())->name != varName)
            return false;
        int32_t k = 0;
        if (!exprIsIntLiteral(be->right.get(), &k)) return false;
        if (be->op == BinaryOp::AddAssign) incDelta = k;
        else if (be->op == BinaryOp::SubAssign) incDelta = -k;
        else return false;
    }
    else
    {
        return false;
    }
    if (incDelta == 0) return false;

    // Body must not break/continue out of this loop level.
    if (stmtContainsBreakOrContinue(stmt->body.get())) return false;

    auto evalCmp = [&](int32_t lhs, int32_t rhs) -> bool {
        switch (cmpOp)
        {
        case BinaryOp::Less:         return lhs <  rhs;
        case BinaryOp::LessEqual:    return lhs <= rhs;
        case BinaryOp::Greater:      return lhs >  rhs;
        case BinaryOp::GreaterEqual: return lhs >= rhs;
        case BinaryOp::Equal:        return lhs == rhs;
        case BinaryOp::NotEqual:     return lhs != rhs;
        default:                     return false;
        }
    };

    constexpr int kMaxUnroll = 64;
    int32_t simulated = curVal;
    int trip = 0;
    while (evalCmp(simulated, cmpVal))
    {
        ++trip;
        if (trip > kMaxUnroll) return false;
        simulated += incDelta;
    }

    for (int iter = 0; iter < trip; ++iter)
    {
        nameToValue_[varName] = createConstant(curVal);
        buildStmt(stmt->body.get());
        if (currentBlock_->hasTerminator()) return true;
        curVal += incDelta;
    }
    nameToValue_[varName] = createConstant(curVal);
    return true;
}

void IRBuilder::buildForStmt(ForStmt* stmt)
{
    // Static-count loops with literal init/condition/increment and no
    // break/continue are fully unrolled into straight-line IR. This
    // matches sce-cgc's lowering and lets the existing emit pipeline
    // handle each iteration as ordinary scalar code.
    if (tryUnrollStaticFor(stmt)) return;

    // Build initializer
    if (stmt->init)
    {
        buildStmt(stmt->init.get());
    }

    // Create blocks
    IRBasicBlock* condBlock = currentFunction_->createBlock(makeLabel("for.cond"));
    IRBasicBlock* bodyBlock = currentFunction_->createBlock(makeLabel("for.body"));
    IRBasicBlock* incBlock = currentFunction_->createBlock(makeLabel("for.inc"));
    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("for.end"));

    // Push loop context
    loopStack_.push({incBlock, endBlock});

    // Jump to condition
    emitBranch(condBlock);

    // Build condition
    currentBlock_ = condBlock;
    if (stmt->condition)
    {
        IRValueID condValue = buildExpr(stmt->condition.get());
        emitCondBranch(condValue, bodyBlock, endBlock);
    }
    else
    {
        // No condition = infinite loop
        emitBranch(bodyBlock);
    }

    // Build body
    currentBlock_ = bodyBlock;
    buildStmt(stmt->body.get());
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(incBlock);
    }

    // Build increment
    currentBlock_ = incBlock;
    if (stmt->increment)
    {
        buildExpr(stmt->increment.get());
    }
    emitBranch(condBlock);

    // Pop loop context
    loopStack_.pop();

    // Continue from end block
    currentBlock_ = endBlock;
}

void IRBuilder::buildWhileStmt(WhileStmt* stmt)
{
    // Create blocks
    IRBasicBlock* condBlock = currentFunction_->createBlock(makeLabel("while.cond"));
    IRBasicBlock* bodyBlock = currentFunction_->createBlock(makeLabel("while.body"));
    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("while.end"));

    // Push loop context
    loopStack_.push({condBlock, endBlock});

    // Jump to condition
    emitBranch(condBlock);

    // Build condition
    currentBlock_ = condBlock;
    IRValueID condValue = buildExpr(stmt->condition.get());
    emitCondBranch(condValue, bodyBlock, endBlock);

    // Build body
    currentBlock_ = bodyBlock;
    buildStmt(stmt->body.get());
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(condBlock);
    }

    // Pop loop context
    loopStack_.pop();

    // Continue from end block
    currentBlock_ = endBlock;
}

void IRBuilder::buildDoWhileStmt(DoWhileStmt* stmt)
{
    // Create blocks
    IRBasicBlock* bodyBlock = currentFunction_->createBlock(makeLabel("dowhile.body"));
    IRBasicBlock* condBlock = currentFunction_->createBlock(makeLabel("dowhile.cond"));
    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("dowhile.end"));

    // Push loop context
    loopStack_.push({condBlock, endBlock});

    // Jump to body
    emitBranch(bodyBlock);

    // Build body
    currentBlock_ = bodyBlock;
    buildStmt(stmt->body.get());
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(condBlock);
    }

    // Build condition
    currentBlock_ = condBlock;
    IRValueID condValue = buildExpr(stmt->condition.get());
    emitCondBranch(condValue, bodyBlock, endBlock);

    // Pop loop context
    loopStack_.pop();

    // Continue from end block
    currentBlock_ = endBlock;
}

void IRBuilder::buildSwitchStmt(SwitchStmt* stmt)
{
    // Switch statements in shaders are relatively rare and complex to lower properly.
    // For now, we emit the switch expression and body as-is, treating it like a block.
    // TODO: Implement proper switch lowering with case value matching

    IRValueID switchValue = buildExpr(stmt->expr.get());
    (void)switchValue;  // Value would be used for case matching

    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("switch.end"));

    // Push loop context (for break)
    loopStack_.push({nullptr, endBlock});

    // For now, just build the body block directly
    // This won't handle case labels correctly, but allows basic compilation
    if (stmt->body)
    {
        buildBlockStmt(stmt->body.get());
    }

    // Pop loop context
    loopStack_.pop();

    // Branch to end if not already terminated
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(endBlock);
    }

    // Continue from end block
    currentBlock_ = endBlock;
}

void IRBuilder::buildReturnStmt(ReturnStmt* stmt)
{
    if (stmt->value)
    {
        IRValueID retValue = buildExpr(stmt->value.get());

        // Note: StoreOutput instructions are already emitted during member assignment
        // (e.g., output.position = value). No need to emit them again at return time
        // since that would create duplicate store instructions.

        emitReturn(retValue);
    }
    else
    {
        emitReturn(InvalidIRValue);
    }
}

void IRBuilder::buildBreakStmt(BreakStmt* stmt)
{
    if (loopStack_.empty())
    {
        error(stmt->loc, "break statement outside of loop");
        return;
    }

    emitBranch(loopStack_.top().breakTarget);
}

void IRBuilder::buildContinueStmt(ContinueStmt* stmt)
{
    if (loopStack_.empty() || !loopStack_.top().continueTarget)
    {
        error(stmt->loc, "continue statement outside of loop");
        return;
    }

    emitBranch(loopStack_.top().continueTarget);
}

void IRBuilder::buildDiscardStmt(DiscardStmt* stmt)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Discard, InvalidIRValue, IRTypeInfo::Void());
    currentBlock_->addInstruction(std::move(inst));
}

void IRBuilder::buildExprStmt(ExprStmt* stmt)
{
    if (stmt->expr)
    {
        buildExpr(stmt->expr.get());
    }
}

void IRBuilder::buildDeclStmt(DeclStmt* stmt)
{
    if (!stmt->declaration) return;

    if (stmt->declaration->kind == DeclKind::Variable)
    {
        auto* varDecl = static_cast<VarDecl*>(stmt->declaration.get());

        // Allocate a value for the variable
        IRValueID varId = currentFunction_->allocateValueId();
        declToValue_[varDecl] = varId;
        nameToValue_[varDecl->name] = varId;

        // If there's an initializer, evaluate it
        if (varDecl->initializer)
        {
            IRValueID initValue = buildExpr(varDecl->initializer.get());
            // For now, just use the initializer value as the variable value
            nameToValue_[varDecl->name] = initValue;
            declToValue_[varDecl] = initValue;
        }
    }
}

// ============================================================================
// Expression Building
// ============================================================================

IRValueID IRBuilder::buildExpr(ExprNode* expr)
{
    if (!expr) return InvalidIRValue;

    switch (expr->kind)
    {
    case ExprKind::Literal:
        return buildLiteralExpr(static_cast<LiteralExpr*>(expr));
    case ExprKind::Identifier:
        return buildIdentifierExpr(static_cast<IdentifierExpr*>(expr));
    case ExprKind::Binary:
        return buildBinaryExpr(static_cast<BinaryExpr*>(expr));
    case ExprKind::Unary:
        return buildUnaryExpr(static_cast<UnaryExpr*>(expr));
    case ExprKind::Call:
        return buildCallExpr(static_cast<CallExpr*>(expr));
    case ExprKind::MemberAccess:
        return buildMemberAccessExpr(static_cast<MemberAccessExpr*>(expr));
    case ExprKind::Index:
        return buildIndexExpr(static_cast<IndexExpr*>(expr));
    case ExprKind::Ternary:
        return buildTernaryExpr(static_cast<TernaryExpr*>(expr));
    case ExprKind::Cast:
        return buildCastExpr(static_cast<CastExpr*>(expr));
    case ExprKind::Constructor:
        return buildConstructorExpr(static_cast<ConstructorExpr*>(expr));
    default:
        error(expr->loc, "Unknown expression kind");
        return InvalidIRValue;
    }
}

IRValueID IRBuilder::buildLiteralExpr(LiteralExpr* expr)
{
    switch (expr->literalKind)
    {
    case LiteralExpr::LiteralKind::Int:
        return createConstant(static_cast<int32_t>(std::get<int64_t>(expr->value)));
    case LiteralExpr::LiteralKind::Float:
        return createConstant(static_cast<float>(std::get<double>(expr->value)));
    case LiteralExpr::LiteralKind::Bool:
        return createConstant(std::get<bool>(expr->value));
    default:
        error(expr->loc, "Unknown literal kind");
        return InvalidIRValue;
    }
}

IRValueID IRBuilder::buildIdentifierExpr(IdentifierExpr* expr)
{
    // Look up in local names first
    auto it = nameToValue_.find(expr->name);
    if (it != nameToValue_.end())
    {
        return it->second;
    }

    // Look up in globals
    IRGlobal* global = module_->findGlobal(expr->name);
    if (global)
    {
        // Emit load from global
        auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
            currentFunction_->allocateValueId(), global->type);
        inst->targetName = global->name;  // Use targetName for uniform name
        currentBlock_->addInstruction(std::move(inst));
        return currentFunction_->nextValueId - 1;
    }

    error(expr->loc, "Unknown identifier: " + expr->name);
    return InvalidIRValue;
}

IRValueID IRBuilder::buildBinaryExpr(BinaryExpr* expr)
{
    // Handle assignment specially
    if (TypeOperations::isAssignmentOp(expr->op))
    {
        IRValueID rhsValue = buildExpr(expr->right.get());

        // For compound assignments, compute the new value
        if (expr->op != BinaryOp::Assign)
        {
            IRValueID lhsValue = buildExpr(expr->left.get());
            IROp op;
            switch (expr->op)
            {
            case BinaryOp::AddAssign: op = IROp::Add; break;
            case BinaryOp::SubAssign: op = IROp::Sub; break;
            case BinaryOp::MulAssign: op = IROp::Mul; break;
            case BinaryOp::DivAssign: op = IROp::Div; break;
            case BinaryOp::ModAssign: op = IROp::Mod; break;
            default: op = IROp::Add; break;
            }
            rhsValue = emitBinaryOp(op, getExprType(expr->left.get()), lhsValue, rhsValue);
        }

        return buildAssignment(expr->left.get(), rhsValue);
    }

    // Regular binary expression
    IRValueID leftValue = buildExpr(expr->left.get());
    IRValueID rightValue = buildExpr(expr->right.get());

    IROp op = binaryOpToIROp(expr->op);
    IRTypeInfo resultType = getExprType(expr);

    return emitBinaryOp(op, resultType, leftValue, rightValue);
}

IRValueID IRBuilder::buildUnaryExpr(UnaryExpr* expr)
{
    IRValueID operandValue = buildExpr(expr->operand.get());

    // Handle increment/decrement specially
    if (expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PreDecrement ||
        expr->op == UnaryOp::PostIncrement || expr->op == UnaryOp::PostDecrement)
    {
        IRTypeInfo type = getExprType(expr->operand.get());
        IRValueID one = createConstant(1.0f);

        IROp op = (expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PostIncrement)
            ? IROp::Add : IROp::Sub;

        IRValueID newValue = emitBinaryOp(op, type, operandValue, one);

        // Store back
        buildAssignment(expr->operand.get(), newValue);

        // Return old or new value based on pre/post
        if (expr->op == UnaryOp::PostIncrement || expr->op == UnaryOp::PostDecrement)
        {
            return operandValue;
        }
        return newValue;
    }

    IROp op = unaryOpToIROp(expr->op);
    IRTypeInfo resultType = getExprType(expr);

    return emitUnaryOp(op, resultType, operandValue);
}

IRValueID IRBuilder::buildCallExpr(CallExpr* expr)
{
    // Check for built-in function
    auto builtinOp = builtinToIROp(expr->functionName);

    // Build arguments
    std::vector<IRValueID> argValues;
    for (auto& arg : expr->arguments)
    {
        argValues.push_back(buildExpr(arg.get()));
    }

    IRTypeInfo resultType = getExprType(expr);

    if (builtinOp)
    {
        // Special handling for 'mul' - determine correct operation based on argument types
        if (expr->functionName == "mul" && expr->arguments.size() == 2)
        {
            IRTypeInfo arg0Type = getExprType(expr->arguments[0].get());
            IRTypeInfo arg1Type = getExprType(expr->arguments[1].get());

            bool arg0IsMatrix = arg0Type.isMatrix();
            bool arg1IsMatrix = arg1Type.isMatrix();
            bool arg0IsVector = arg0Type.isVector();
            bool arg1IsVector = arg1Type.isVector();

            IROp mulOp = IROp::MatVecMul;  // Default

            if (arg0IsMatrix && arg1IsMatrix)
            {
                // Matrix * Matrix
                mulOp = IROp::MatMul;
            }
            else if (arg0IsMatrix && arg1IsVector)
            {
                // Matrix * Vector
                mulOp = IROp::MatVecMul;
            }
            else if (arg0IsVector && arg1IsMatrix)
            {
                // Vector * Matrix
                mulOp = IROp::VecMatMul;
            }

            return emitInstruction(mulOp, resultType, argValues);
        }

        // Emit built-in operation
        return emitInstruction(*builtinOp, resultType, argValues);
    }

    // User function call
    return emitCall(expr->functionName, resultType, argValues);
}

IRValueID IRBuilder::buildMemberAccessExpr(MemberAccessExpr* expr)
{
    // Check for swizzle first
    if (expr->swizzleLength > 0)
    {
        IRValueID objectValue = buildExpr(expr->object.get());
        IRTypeInfo resultType = getExprType(expr);

        auto inst = std::make_unique<IRInstruction>(IROp::VecShuffle,
            currentFunction_->allocateValueId(), resultType);
        inst->addOperand(objectValue);
        inst->swizzleMask = IRUtils::encodeSwizzle(expr->member);
        currentBlock_->addInstruction(std::move(inst));

        return currentFunction_->nextValueId - 1;
    }

    // Struct member access - build a composite name
    // For local variables, use "varname.member" as the key
    if (expr->object->kind == ExprKind::Identifier)
    {
        auto* ident = static_cast<IdentifierExpr*>(expr->object.get());
        std::string compositeName = ident->name + "." + expr->member;

        // Look up in local names
        auto it = nameToValue_.find(compositeName);
        if (it != nameToValue_.end())
        {
            return it->second;
        }

        // If not found, this is the first read - allocate a value
        IRValueID valueId = currentFunction_->allocateValueId();
        nameToValue_[compositeName] = valueId;

        // Emit a load for input parameters with semantics
        if (ident->resolvedDecl && ident->resolvedDecl->kind == DeclKind::Parameter)
        {
            auto* param = static_cast<ParamDecl*>(ident->resolvedDecl);
            if (param->type && param->type->baseType == BaseType::Struct)
            {
                // Look up semantic info for this struct member using helper
                const std::vector<StructField>* fields = getStructFields(param->type.get());
                if (fields)
                {
                    for (const auto& field : *fields)
                    {
                        if (field.name == expr->member)
                        {
                            IRTypeInfo fieldType = getIRType(field.type.get());
                            auto inst = std::make_unique<IRInstruction>(IROp::LoadAttribute,
                                valueId, fieldType);
                            inst->semanticName     = field.semantic.name;
                            inst->rawSemanticName  = field.semantic.rawName;
                            inst->semanticIndex    = field.semantic.index;
                            inst->structParamName  = param->name;       // e.g. "input"
                            inst->fieldName        = expr->member;       // e.g. "pos"
                            currentBlock_->addInstruction(std::move(inst));
                            return valueId;
                        }
                    }
                }
            }
        }

        // Check if this is a uniform struct global - emit LoadUniform with qualified name
        IRGlobal* global = module_->findGlobal(ident->name);
        if (global && global->storage == StorageQualifier::Uniform)
        {
            IRTypeInfo memberType = getExprType(expr);
            auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
                valueId, memberType);
            inst->targetName = compositeName;  // Use qualified name e.g. "u_data.color"
            currentBlock_->addInstruction(std::move(inst));
            return valueId;
        }

        return valueId;
    }

    // For nested member access (a.b.c), recursively resolve
    if (expr->object->kind == ExprKind::MemberAccess)
    {
        // Build name by walking the member access chain
        std::string baseName;
        ExprNode* current = expr->object.get();
        std::vector<std::string> members;
        members.push_back(expr->member);

        while (current->kind == ExprKind::MemberAccess)
        {
            auto* memberExpr = static_cast<MemberAccessExpr*>(current);
            members.push_back(memberExpr->member);
            current = memberExpr->object.get();
        }

        if (current->kind == ExprKind::Identifier)
        {
            baseName = static_cast<IdentifierExpr*>(current)->name;
        }

        // Build composite name (reverse order)
        std::string compositeName = baseName;
        for (auto it = members.rbegin(); it != members.rend(); ++it)
        {
            compositeName += "." + *it;
        }

        auto nameIt = nameToValue_.find(compositeName);
        if (nameIt != nameToValue_.end())
        {
            return nameIt->second;
        }

        IRValueID valueId = currentFunction_->allocateValueId();
        nameToValue_[compositeName] = valueId;
        return valueId;
    }

    // Fallback - evaluate object and return placeholder
    IRValueID objectValue = buildExpr(expr->object.get());
    (void)objectValue;

    IRTypeInfo resultType = getExprType(expr);
    IRValueID valueId = currentFunction_->allocateValueId();
    return valueId;
}

IRValueID IRBuilder::buildIndexExpr(IndexExpr* expr)
{
    // Check if this is a uniform array access (e.g., u_colors[2])
    // For uniform arrays with constant indices, emit LoadUniform with componentIndex
    // instead of VecExtract (which is for vector component extraction)
    if (expr->array->kind == ExprKind::Identifier)
    {
        auto* ident = static_cast<IdentifierExpr*>(expr->array.get());
        IRGlobal* global = module_->findGlobal(ident->name);

        // Debug: uniform array index access detection
        // printf("DEBUG buildIndexExpr: ident=%s global=%p isArray=%d\n",
        //        ident->name.c_str(), (void*)global,
        //        global ? (int)global->type.isArray() : -1);
        // fflush(stdout);

        if (global && global->type.isArray())
        {
            // Check if index is a compile-time constant
            if (expr->index->kind == ExprKind::Literal)
            {
                auto* lit = static_cast<LiteralExpr*>(expr->index.get());
                if (lit->literalKind == LiteralExpr::LiteralKind::Int)
                {
                    int arrayIndex = static_cast<int>(std::get<int64_t>(lit->value));

                    // Debug: uniform array LoadUniform emission
                    // printf("DEBUG buildIndexExpr: emitting LoadUniform for %s[%d]\n",
                    //        global->name.c_str(), arrayIndex);
                    // fflush(stdout);

                    IRTypeInfo resultType = getExprType(expr);
                    auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
                        currentFunction_->allocateValueId(), resultType);
                    inst->targetName = global->name;
                    inst->componentIndex = arrayIndex;
                    currentBlock_->addInstruction(std::move(inst));
                    return currentFunction_->nextValueId - 1;
                }
            }
        }
    }

    // Fallback: vector component extraction (e.g., vec.x, vec[i])
    IRValueID arrayValue = buildExpr(expr->array.get());
    IRValueID indexValue = buildExpr(expr->index.get());

    IRTypeInfo resultType = getExprType(expr);

    auto inst = std::make_unique<IRInstruction>(IROp::VecExtract,
        currentFunction_->allocateValueId(), resultType);
    inst->addOperand(arrayValue);
    inst->addOperand(indexValue);
    currentBlock_->addInstruction(std::move(inst));

    return currentFunction_->nextValueId - 1;
}

IRValueID IRBuilder::buildTernaryExpr(TernaryExpr* expr)
{
    IRValueID condValue = buildExpr(expr->condition.get());
    IRValueID thenValue = buildExpr(expr->thenExpr.get());
    IRValueID elseValue = buildExpr(expr->elseExpr.get());

    IRTypeInfo resultType = getExprType(expr);

    auto inst = std::make_unique<IRInstruction>(IROp::Select,
        currentFunction_->allocateValueId(), resultType);
    inst->addOperand(condValue);
    inst->addOperand(thenValue);
    inst->addOperand(elseValue);
    currentBlock_->addInstruction(std::move(inst));

    return currentFunction_->nextValueId - 1;
}

IRValueID IRBuilder::buildCastExpr(CastExpr* expr)
{
    IRValueID operandValue = buildExpr(expr->operand.get());
    IRTypeInfo targetType = getIRType(expr->targetType.get());
    IRTypeInfo sourceType = getExprType(expr->operand.get());

    // Determine conversion operation
    IROp op = IROp::Bitcast;  // Default

    if (sourceType.baseType == IRType::Int32 && targetType.baseType == IRType::Float32)
    {
        op = IROp::IntToFloat;
    }
    else if (sourceType.baseType == IRType::Float32 && targetType.baseType == IRType::Int32)
    {
        op = IROp::FloatToInt;
    }
    else if (sourceType.baseType == IRType::Float32 && targetType.baseType == IRType::Float16)
    {
        op = IROp::FloatToHalf;
    }
    else if (sourceType.baseType == IRType::Float16 && targetType.baseType == IRType::Float32)
    {
        op = IROp::HalfToFloat;
    }

    return emitUnaryOp(op, targetType, operandValue);
}

IRValueID IRBuilder::buildConstructorExpr(ConstructorExpr* expr)
{
    IRTypeInfo resultType = getIRType(expr->constructedType.get());

    // Build argument values
    std::vector<IRValueID> argValues;
    for (auto& arg : expr->arguments)
    {
        argValues.push_back(buildExpr(arg.get()));
    }

    // Single argument handling
    if (argValues.size() == 1)
    {
        IRTypeInfo argType = getExprType(expr->arguments[0].get());
        if (argType.componentCount() == resultType.componentCount())
        {
            // Same type - just return it
            if (argType.baseType == resultType.baseType)
            {
                return argValues[0];
            }

            // Different base type - need type conversion (e.g., half(1.0) = float->half)
            IROp convOp = IROp::Bitcast;  // Default
            if (argType.baseType == IRType::Float32 && resultType.baseType == IRType::Float16)
            {
                convOp = IROp::FloatToHalf;
            }
            else if (argType.baseType == IRType::Float16 && resultType.baseType == IRType::Float32)
            {
                convOp = IROp::HalfToFloat;
            }
            else if (argType.baseType == IRType::Int32 && resultType.baseType == IRType::Float32)
            {
                convOp = IROp::IntToFloat;
            }
            else if (argType.baseType == IRType::Float32 && resultType.baseType == IRType::Int32)
            {
                convOp = IROp::FloatToInt;
            }

            return emitUnaryOp(convOp, resultType, argValues[0]);
        }
    }

    // Vector construction
    if (resultType.isVector())
    {
        auto inst = std::make_unique<IRInstruction>(IROp::VecConstruct,
            currentFunction_->allocateValueId(), resultType);
        for (IRValueID arg : argValues)
        {
            inst->addOperand(arg);
        }
        currentBlock_->addInstruction(std::move(inst));
        return currentFunction_->nextValueId - 1;
    }

    // Matrix construction from vectors (e.g., half3x3(v1, v2, v3))
    if (resultType.isMatrix())
    {
        // Verify we have the right number of row/column vectors
        int expectedVectors = resultType.matrixRows; // Assume row-major: N rows of M-component vectors
        if (argValues.size() == (size_t)expectedVectors)
        {
            auto inst = std::make_unique<IRInstruction>(IROp::MatConstruct,
                currentFunction_->allocateValueId(), resultType);
            for (IRValueID arg : argValues)
            {
                inst->addOperand(arg);
            }
            currentBlock_->addInstruction(std::move(inst));
            return currentFunction_->nextValueId - 1;
        }
        // Could also be constructed from all scalar components
        int totalComponents = resultType.matrixRows * resultType.matrixCols;
        if (argValues.size() == (size_t)totalComponents)
        {
            auto inst = std::make_unique<IRInstruction>(IROp::MatConstruct,
                currentFunction_->allocateValueId(), resultType);
            for (IRValueID arg : argValues)
            {
                inst->addOperand(arg);
            }
            currentBlock_->addInstruction(std::move(inst));
            return currentFunction_->nextValueId - 1;
        }
    }

    // Single scalar broadcast
    if (argValues.size() == 1)
    {
        return argValues[0];
    }

    error(expr->loc, "Complex constructor not yet implemented");
    return InvalidIRValue;
}

IRValueID IRBuilder::buildAssignment(ExprNode* target, IRValueID value)
{
    if (target->kind == ExprKind::Identifier)
    {
        auto* ident = static_cast<IdentifierExpr*>(target);
        nameToValue_[ident->name] = value;

        // Check if this is an out parameter with a semantic - emit StoreOutput
        if (ident->resolvedDecl && ident->resolvedDecl->kind == DeclKind::Parameter)
        {
            auto* param = static_cast<ParamDecl*>(ident->resolvedDecl);
            if ((param->storage == StorageQualifier::Out ||
                 param->storage == StorageQualifier::InOut) &&
                !param->semantic.isEmpty())
            {
                // Emit a store output instruction for the out parameter
                auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                    InvalidIRValue, IRTypeInfo::Void());
                inst->addOperand(value);
                inst->semanticName    = param->semantic.name;
                inst->rawSemanticName = param->semantic.rawName;
                inst->semanticIndex   = param->semantic.index;
                currentBlock_->addInstruction(std::move(inst));
            }
        }
        return value;
    }

    // Handle struct member assignment
    if (target->kind == ExprKind::MemberAccess)
    {
        auto* memberExpr = static_cast<MemberAccessExpr*>(target);

        // Build the composite name for the member
        if (memberExpr->object->kind == ExprKind::Identifier)
        {
            auto* ident = static_cast<IdentifierExpr*>(memberExpr->object.get());
            std::string compositeName = ident->name + "." + memberExpr->member;
            nameToValue_[compositeName] = value;

            // Check if this is an output struct - emit StoreOutput
            if (ident->resolvedDecl)
            {
                // Look up struct type info for the target
                TypeNode* typeNode = nullptr;
                if (ident->resolvedDecl->kind == DeclKind::Variable)
                {
                    typeNode = static_cast<VarDecl*>(ident->resolvedDecl)->type.get();
                }

                // Use helper to resolve struct fields (may need to look up from semantic analyzer)
                const std::vector<StructField>* fields = getStructFields(typeNode);
                if (fields)
                {
                    // Look up semantic info for this struct member
                    for (const auto& field : *fields)
                    {
                        if (field.name == memberExpr->member && !field.semantic.isEmpty())
                        {
                            // Emit a store output instruction
                            auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                                InvalidIRValue, IRTypeInfo::Void());
                            inst->addOperand(value);
                            inst->semanticName    = field.semantic.name;
                            inst->rawSemanticName = field.semantic.rawName;
                            inst->semanticIndex   = field.semantic.index;
                            inst->fieldName       = memberExpr->member;
                            currentBlock_->addInstruction(std::move(inst));
                            break;
                        }
                    }
                }
            }

            return value;
        }

        // Handle nested member access (a.b.c = value)
        if (memberExpr->object->kind == ExprKind::MemberAccess)
        {
            std::string baseName;
            ExprNode* current = memberExpr->object.get();
            std::vector<std::string> members;
            members.push_back(memberExpr->member);

            while (current->kind == ExprKind::MemberAccess)
            {
                auto* nested = static_cast<MemberAccessExpr*>(current);
                members.push_back(nested->member);
                current = nested->object.get();
            }

            if (current->kind == ExprKind::Identifier)
            {
                baseName = static_cast<IdentifierExpr*>(current)->name;
            }

            // Build composite name (reverse order)
            std::string compositeName = baseName;
            for (auto it = members.rbegin(); it != members.rend(); ++it)
            {
                compositeName += "." + *it;
            }

            nameToValue_[compositeName] = value;
            return value;
        }
    }

    // Handle array index assignment
    if (target->kind == ExprKind::Index)
    {
        // For now, just store the value and return
        // TODO: Proper array element store
        return value;
    }

    error(target->loc, "Complex assignment target not yet implemented");
    return value;
}

// ============================================================================
// Instruction Emission
// ============================================================================

IRValueID IRBuilder::emitInstruction(IROp op, const IRTypeInfo& resultType,
                                      const std::vector<IRValueID>& operands)
{
    IRValueID result = (resultType.baseType != IRType::Void)
        ? currentFunction_->allocateValueId()
        : InvalidIRValue;

    auto inst = std::make_unique<IRInstruction>(op, result, resultType);
    for (IRValueID opnd : operands)
    {
        inst->addOperand(opnd);
    }
    currentBlock_->addInstruction(std::move(inst));

    return result;
}

IRValueID IRBuilder::emitBinaryOp(IROp op, const IRTypeInfo& resultType,
                                   IRValueID left, IRValueID right)
{
    return emitInstruction(op, resultType, {left, right});
}

IRValueID IRBuilder::emitUnaryOp(IROp op, const IRTypeInfo& resultType, IRValueID operand)
{
    return emitInstruction(op, resultType, {operand});
}

IRValueID IRBuilder::emitCall(const std::string& funcName, const IRTypeInfo& resultType,
                               const std::vector<IRValueID>& args)
{
    IRValueID result = (resultType.baseType != IRType::Void)
        ? currentFunction_->allocateValueId()
        : InvalidIRValue;

    auto inst = std::make_unique<IRInstruction>(IROp::Call, result, resultType);
    inst->targetName = funcName;
    for (IRValueID arg : args)
    {
        inst->addOperand(arg);
    }
    currentBlock_->addInstruction(std::move(inst));

    return result;
}

void IRBuilder::emitBranch(IRBasicBlock* target)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Branch, InvalidIRValue, IRTypeInfo::Void());
    inst->targetName = target->name;
    currentBlock_->addInstruction(std::move(inst));

    // Update CFG
    currentBlock_->successors.push_back(target);
    target->predecessors.push_back(currentBlock_);
}

void IRBuilder::emitCondBranch(IRValueID condition, IRBasicBlock* trueTarget, IRBasicBlock* falseTarget)
{
    auto inst = std::make_unique<IRInstruction>(IROp::CondBranch, InvalidIRValue, IRTypeInfo::Void());
    inst->addOperand(condition);
    inst->targetName = trueTarget->name + "," + falseTarget->name;
    currentBlock_->addInstruction(std::move(inst));

    // Update CFG
    currentBlock_->successors.push_back(trueTarget);
    currentBlock_->successors.push_back(falseTarget);
    trueTarget->predecessors.push_back(currentBlock_);
    falseTarget->predecessors.push_back(currentBlock_);
}

void IRBuilder::emitReturn(IRValueID value)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Return, InvalidIRValue, IRTypeInfo::Void());
    if (value != InvalidIRValue)
    {
        inst->addOperand(value);
    }
    currentBlock_->addInstruction(std::move(inst));
}

void IRBuilder::emitStore(IRValueID address, IRValueID value)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Store, InvalidIRValue, IRTypeInfo::Void());
    inst->addOperand(address);
    inst->addOperand(value);
    currentBlock_->addInstruction(std::move(inst));
}

// ============================================================================
// Type Helpers
// ============================================================================

IRTypeInfo IRBuilder::getIRType(const CgType& cgType)
{
    return IRTypeInfo::fromCgType(cgType);
}

IRTypeInfo IRBuilder::getIRType(TypeNode* typeNode)
{
    if (!typeNode) return IRTypeInfo::Void();
    return IRTypeInfo::fromCgType(CgType(std::make_shared<TypeNode>(*typeNode)));
}

IRTypeInfo IRBuilder::getExprType(ExprNode* expr)
{
    if (!expr || !expr->resolvedType) return IRTypeInfo::Void();
    return getIRType(expr->resolvedType.get());
}

const std::vector<StructField>* IRBuilder::getStructFields(TypeNode* typeNode)
{
    if (!typeNode || typeNode->baseType != BaseType::Struct)
        return nullptr;

    // First check if fields are directly on the TypeNode
    if (!typeNode->structFields.empty())
        return &typeNode->structFields;

    // Otherwise look up the struct declaration by name from symbol table
    if (!typeNode->structName.empty() && semantic_)
    {
        std::optional<CgType> cgTypeOpt = semantic_->symbolTable().lookupType(typeNode->structName);
        if (cgTypeOpt && cgTypeOpt->isStruct() && !cgTypeOpt->structFields().empty())
        {
            // Store fields in the TypeNode for future lookups (avoid repeated lookups)
            typeNode->structFields = cgTypeOpt->structFields();
            return &typeNode->structFields;
        }
    }
    return nullptr;
}

void IRBuilder::emitStructOutputs(ExprNode* structExpr, const std::vector<StructField>& fields)
{
    // For each field with a semantic, emit a StoreOutput
    for (const auto& field : fields)
    {
        if (field.semantic.isEmpty())
            continue;

        // Look up the value for this field from nameToValue_
        // The format is "varname.fieldname"
        std::string fieldKey;
        if (structExpr->kind == ExprKind::Identifier)
        {
            auto* ident = static_cast<IdentifierExpr*>(structExpr);
            fieldKey = ident->name + "." + field.name;
        }

        auto it = nameToValue_.find(fieldKey);
        if (it != nameToValue_.end())
        {
            IRValueID fieldValue = it->second;

            auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                InvalidIRValue, IRTypeInfo::Void());
            inst->addOperand(fieldValue);
            inst->semanticName    = field.semantic.name;
            inst->rawSemanticName = field.semantic.rawName;
            inst->semanticIndex   = field.semantic.index;
            inst->fieldName       = field.name;
            currentBlock_->addInstruction(std::move(inst));
        }
    }
}

// ============================================================================
// Operator Mapping
// ============================================================================

IROp IRBuilder::binaryOpToIROp(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::Add: return IROp::Add;
    case BinaryOp::Sub: return IROp::Sub;
    case BinaryOp::Mul: return IROp::Mul;
    case BinaryOp::Div: return IROp::Div;
    case BinaryOp::Mod: return IROp::Mod;
    case BinaryOp::BitwiseAnd: return IROp::And;
    case BinaryOp::BitwiseOr: return IROp::Or;
    case BinaryOp::BitwiseXor: return IROp::Xor;
    case BinaryOp::ShiftLeft: return IROp::Shl;
    case BinaryOp::ShiftRight: return IROp::Shr;
    case BinaryOp::Equal: return IROp::CmpEq;
    case BinaryOp::NotEqual: return IROp::CmpNe;
    case BinaryOp::Less: return IROp::CmpLt;
    case BinaryOp::LessEqual: return IROp::CmpLe;
    case BinaryOp::Greater: return IROp::CmpGt;
    case BinaryOp::GreaterEqual: return IROp::CmpGe;
    case BinaryOp::LogicalAnd: return IROp::LogicalAnd;
    case BinaryOp::LogicalOr: return IROp::LogicalOr;
    default: return IROp::Nop;
    }
}

IROp IRBuilder::unaryOpToIROp(UnaryOp op)
{
    switch (op)
    {
    case UnaryOp::Negate: return IROp::Neg;
    case UnaryOp::LogicalNot: return IROp::LogicalNot;
    case UnaryOp::BitwiseNot: return IROp::Not;
    default: return IROp::Nop;
    }
}

std::optional<IROp> IRBuilder::builtinToIROp(const std::string& name)
{
    static const std::unordered_map<std::string, IROp> builtins = {
        {"abs", IROp::Abs},
        {"sign", IROp::Sign},
        {"floor", IROp::Floor},
        {"ceil", IROp::Ceil},
        {"frac", IROp::Frac},
        {"round", IROp::Round},
        {"trunc", IROp::Trunc},
        {"min", IROp::Min},
        {"max", IROp::Max},
        {"clamp", IROp::Clamp},
        {"saturate", IROp::Saturate},
        {"lerp", IROp::Lerp},
        {"step", IROp::Step},
        {"smoothstep", IROp::SmoothStep},
        {"sin", IROp::Sin},
        {"cos", IROp::Cos},
        {"tan", IROp::Tan},
        {"asin", IROp::Asin},
        {"acos", IROp::Acos},
        {"atan", IROp::Atan},
        {"atan2", IROp::Atan2},
        {"pow", IROp::Pow},
        {"exp", IROp::Exp},
        {"exp2", IROp::Exp2},
        {"log", IROp::Log},
        {"log2", IROp::Log2},
        {"sqrt", IROp::Sqrt},
        {"rsqrt", IROp::RSqrt},
        {"dot", IROp::Dot},
        {"cross", IROp::Cross},
        {"length", IROp::Length},
        {"distance", IROp::Distance},
        {"normalize", IROp::Normalize},
        {"reflect", IROp::Reflect},
        {"refract", IROp::Refract},
        {"faceforward", IROp::FaceForward},
        {"mul", IROp::MatVecMul},
        {"tex2D", IROp::TexSample},
        {"tex2Dlod", IROp::TexSampleLod},
        {"tex2Dproj", IROp::TexSampleProj},
        {"texCUBE", IROp::TexSample},
    };

    auto it = builtins.find(name);
    if (it != builtins.end())
    {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================================
// Constant Creation
// ============================================================================

IRValueID IRBuilder::createConstant(bool value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::Bool(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(int32_t value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::Int(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(uint32_t value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::UInt(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(float value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::Float(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(const IRTypeInfo& type, const std::vector<float>& values)
{
    auto* constant = currentFunction_->createConstant(type, values);
    return constant->id;
}
