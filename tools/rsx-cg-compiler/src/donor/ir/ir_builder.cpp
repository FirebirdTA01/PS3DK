#include "ir_builder.h"
#include <cmath>
#include <sstream>
#include <unordered_set>

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

            // Cg implicitly treats file-scope variables with no explicit
            // storage qualifier as `uniform` (e.g. `float4x4 gMtx;`).
            if (varDecl->storage == StorageQualifier::None)
                varDecl->storage = StorageQualifier::Uniform;

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
    currentFunctionDecl_ = decl;
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
    currentFunctionDecl_ = nullptr;
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

    // Snapshot nameToValue_ before either branch runs so we can detect
    // which variables get redefined in then/else and insert merge-time
    // Selects.  Without this, code like `if (cond) c = c * tex;` leaves
    // nameToValue_["c"] pointing at the THEN-only SSA value, which is
    // undefined when the false branch is taken — invalid SSA.
    auto preIfMap = nameToValue_;

    // Build then block
    currentBlock_ = thenBlock;
    buildStmt(stmt->thenBranch.get());
    const bool thenTerminated = currentBlock_->hasTerminator();
    if (!thenTerminated)
    {
        emitBranch(mergeBlock);
    }
    auto postThenMap = nameToValue_;

    // Build else block
    bool elseTerminated = false;
    decltype(nameToValue_) postElseMap;
    if (stmt->elseBranch)
    {
        nameToValue_ = preIfMap;  // restore before processing else
        currentBlock_ = elseBlock;
        buildStmt(stmt->elseBranch.get());
        elseTerminated = currentBlock_->hasTerminator();
        if (!elseTerminated)
        {
            emitBranch(mergeBlock);
        }
        postElseMap = nameToValue_;
    }

    // Continue from merge block.  Insert Select(cond, thenVal, elseVal)
    // for each variable that diverged.  When a branch terminated early
    // (return / break / continue inside the body), its nameToValue_
    // doesn't reach the merge — fall back to the pre-if value for
    // that side so the merge keeps the not-terminated branch's update.
    currentBlock_ = mergeBlock;
    nameToValue_ = preIfMap;

    auto valueOrPre = [&](const std::unordered_map<std::string, IRValueID>& m,
                          const std::string& name) -> IRValueID
    {
        auto it = m.find(name);
        if (it != m.end()) return it->second;
        auto pre = preIfMap.find(name);
        return (pre != preIfMap.end()) ? pre->second : InvalidIRValue;
    };

    auto getValueType = [&](IRValueID id) -> IRTypeInfo
    {
        if (id == InvalidIRValue) return IRTypeInfo::Void();
        // First check parameters / constants.
        if (IRValue* v = currentFunction_->getValue(id))
            return v->type;
        // Then walk instructions for a result match.
        for (const auto& bp : currentFunction_->blocks)
        {
            if (!bp) continue;
            for (const auto& ip : bp->instructions)
            {
                if (ip && ip->result == id) return ip->resultType;
            }
        }
        return IRTypeInfo::Void();
    };

    // Collect every name that appears in either post-map.
    std::unordered_set<std::string> seen;
    for (const auto& kv : postThenMap) seen.insert(kv.first);
    for (const auto& kv : postElseMap) seen.insert(kv.first);

    for (const std::string& name : seen)
    {
        IRValueID preVal = InvalidIRValue;
        if (auto pre = preIfMap.find(name); pre != preIfMap.end())
            preVal = pre->second;

        IRValueID thenVal = thenTerminated ? preVal : valueOrPre(postThenMap, name);
        IRValueID elseVal;
        if (stmt->elseBranch)
            elseVal = elseTerminated ? preVal : valueOrPre(postElseMap, name);
        else
            elseVal = preVal;

        if (thenVal == InvalidIRValue) thenVal = preVal;
        if (elseVal == InvalidIRValue) elseVal = preVal;
        if (thenVal == elseVal)
        {
            // Both branches converge on the same SSA value (or only one
            // branch redefined and the other kept pre-if).  Just record
            // the final value — no Select needed.
            if (thenVal != InvalidIRValue)
                nameToValue_[name] = thenVal;
            continue;
        }

        IRTypeInfo selType = getValueType(thenVal);
        if (selType.baseType == IRType::Void)
            selType = getValueType(elseVal);

        IRValueID selId = currentFunction_->allocateValueId();
        auto sel = std::make_unique<IRInstruction>(
            IROp::Select, selId, selType);
        sel->addOperand(condValue);
        sel->addOperand(thenVal);
        sel->addOperand(elseVal);
        currentBlock_->addInstruction(std::move(sel));
        nameToValue_[name] = selId;
    }
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
        // For struct returns of a local variable (`return OUT;` where
        // `OUT` is a stack-temp struct populated by member assignments),
        // the StoreOutput emits are deferred to here so the outputs
        // land in struct-field declaration order — matching the
        // reference compiler's parameter-table ordering.  Source
        // statement order would otherwise leak through and reorder
        // the synthetic output entries.
        TypeNode* structType = nullptr;
        std::string baseName;
        if (stmt->value->kind == ExprKind::Identifier)
        {
            auto* retIdent = static_cast<IdentifierExpr*>(stmt->value.get());
            if (retIdent->resolvedDecl &&
                retIdent->resolvedDecl->kind == DeclKind::Variable)
            {
                auto* vd = static_cast<VarDecl*>(retIdent->resolvedDecl);
                if (vd->type && vd->type->baseType == BaseType::Struct)
                {
                    structType = vd->type.get();
                    baseName   = retIdent->name;
                }
            }
        }

        IRValueID retValue = buildExpr(stmt->value.get());

        // Non-struct return with a semantic on the return type itself
        // (e.g. `float4 main(...) : COLOR { return color; }`) — emit a
        // StoreOutput keyed off the function's return semantic.  The
        // entry-point parameter path covers the `out`-keyword shape;
        // this covers the value-return shape.
        if (!structType && currentFunctionDecl_ &&
            !currentFunctionDecl_->returnSemantic.isEmpty() &&
            currentFunction_->isEntryPoint)
        {
            const Semantic& sem = currentFunctionDecl_->returnSemantic;
            auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                InvalidIRValue, currentFunction_->returnType);
            inst->addOperand(retValue);
            inst->semanticName    = sem.name;
            inst->rawSemanticName = sem.rawName;
            inst->semanticIndex   = sem.index;
            currentBlock_->addInstruction(std::move(inst));
        }

        if (structType)
        {
            const std::vector<StructField>* fields = getStructFields(structType);
            if (fields)
            {
                for (const auto& field : *fields)
                {
                    if (field.semantic.isEmpty()) continue;
                    auto it = nameToValue_.find(baseName + "." + field.name);
                    if (it == nameToValue_.end()) continue;
                    // Stash the field's source type on the StoreOutput
                    // (in `resultType`) so the container emit can size
                    // the synthetic output param entry correctly — e.g.
                    // a `float2 texCoord : TEXCOORD0;` field gets a
                    // float2-typed param slot, not a float4.
                    IRTypeInfo fieldTy = getIRType(field.type.get());
                    auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                        InvalidIRValue, fieldTy);
                    inst->addOperand(it->second);
                    inst->semanticName    = field.semantic.name;
                    inst->rawSemanticName = field.semantic.rawName;
                    inst->semanticIndex   = field.semantic.index;
                    inst->fieldName       = field.name;
                    currentBlock_->addInstruction(std::move(inst));
                }
            }
        }

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

// Constant-folding helpers.  Inputs are SSA value ids; outputs are
// fresh IRConstant ids when folding succeeds (or InvalidIRValue when
// it doesn't).  Folding handles float scalars + float vectors today —
// FragmentProgram.cg's `const float3 freq = 2.0 * 3.14159 / wave;`
// chain stays in the IRConstant domain throughout, so the back-end
// never sees the divide / multiply on a runtime value.

namespace {
// Extract the float components from an IRConstant.  For scalars,
// returns a single-element vector; for vec types, returns the
// stored components.  Returns false for non-constant or non-float
// types (bool / int / etc. — outside scope today).
bool extractFloatComponents(IRFunction& fn, IRValueID id,
                            std::vector<float>& out)
{
    IRValue* v = fn.getValue(id);
    if (!v) return false;
    auto* c = dynamic_cast<IRConstant*>(v);
    if (!c) return false;
    if (std::holds_alternative<float>(c->value))
    {
        out = { std::get<float>(c->value) };
        return true;
    }
    if (std::holds_alternative<std::vector<float>>(c->value))
    {
        out = std::get<std::vector<float>>(c->value);
        return true;
    }
    return false;
}

// Broadcast a 1-component vector to N components by repeating the
// scalar.  No-op when src.size() == n.  Returns false on length
// mismatch (e.g. mixing vec3 + vec4 with no obvious broadcast).
bool broadcastTo(std::vector<float>& v, size_t n)
{
    if (v.size() == n) return true;
    if (v.size() == 1)
    {
        v.resize(n, v[0]);
        return true;
    }
    return false;
}
}  // namespace

IRValueID IRBuilder::tryFoldBinaryOp(IROp op, const IRTypeInfo& resultType,
                                      IRValueID lhs, IRValueID rhs)
{
    std::vector<float> a, b;
    if (!extractFloatComponents(*currentFunction_, lhs, a)) return InvalidIRValue;
    if (!extractFloatComponents(*currentFunction_, rhs, b)) return InvalidIRValue;

    const size_t n = std::max(a.size(), b.size());
    if (!broadcastTo(a, n) || !broadcastTo(b, n)) return InvalidIRValue;

    std::vector<float> r(n, 0.0f);
    for (size_t i = 0; i < n; ++i)
    {
        switch (op)
        {
        case IROp::Add: r[i] = a[i] + b[i]; break;
        case IROp::Sub: r[i] = a[i] - b[i]; break;
        case IROp::Mul: r[i] = a[i] * b[i]; break;
        case IROp::Div: r[i] = a[i] / b[i]; break;
        case IROp::Min: r[i] = std::fmin(a[i], b[i]); break;
        case IROp::Max: r[i] = std::fmax(a[i], b[i]); break;
        default: return InvalidIRValue;
        }
    }

    // Materialise the folded value as a fresh IRConstant.  Scalar
    // results stay as a single float; vector results carry the full
    // component list.
    if (n == 1)
        return createConstant(r[0]);
    return createConstant(resultType, r);
}

IRValueID IRBuilder::tryFoldUnaryOp(IROp op, const IRTypeInfo& resultType,
                                     IRValueID operand)
{
    std::vector<float> a;
    if (!extractFloatComponents(*currentFunction_, operand, a)) return InvalidIRValue;
    std::vector<float> r(a.size(), 0.0f);
    for (size_t i = 0; i < a.size(); ++i)
    {
        switch (op)
        {
        case IROp::Neg:  r[i] = -a[i]; break;
        case IROp::Abs:  r[i] = std::fabs(a[i]); break;
        case IROp::Sqrt:
            if (a[i] < 0.0f) return InvalidIRValue;  // leave NaN to runtime
            r[i] = std::sqrt(a[i]); break;
        case IROp::RSqrt:
            if (a[i] <= 0.0f) return InvalidIRValue;
            r[i] = 1.0f / std::sqrt(a[i]); break;
        case IROp::Floor: r[i] = std::floor(a[i]); break;
        case IROp::Ceil:  r[i] = std::ceil(a[i]); break;
        default: return InvalidIRValue;
        }
    }
    if (a.size() == 1)
        return createConstant(r[0]);
    return createConstant(resultType, r);
}

IRValueID IRBuilder::tryFoldVecConstruct(const IRTypeInfo& resultType,
                                          const std::vector<IRValueID>& args)
{
    std::vector<float> all;
    all.reserve(static_cast<size_t>(resultType.vectorSize));
    for (IRValueID a : args)
    {
        std::vector<float> comps;
        if (!extractFloatComponents(*currentFunction_, a, comps))
            return InvalidIRValue;
        all.insert(all.end(), comps.begin(), comps.end());
    }
    if (all.size() != static_cast<size_t>(resultType.vectorSize))
        return InvalidIRValue;
    return createConstant(resultType, all);
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

    if (IRValueID folded = tryFoldBinaryOp(op, resultType, leftValue, rightValue);
        folded != InvalidIRValue)
    {
        return folded;
    }

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

    if (IRValueID folded = tryFoldUnaryOp(op, resultType, operandValue);
        folded != InvalidIRValue)
    {
        return folded;
    }

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

        // Constant-fold swizzles of constant vectors — e.g. `v.y`
        // where `v` was const-initialised lifts to a scalar
        // IRConstant.  Multi-lane swizzles (e.g. `.xz` from a vec3)
        // collapse to a vec IRConstant of `swizzleLength` components.
        std::vector<float> objComps;
        if (extractFloatComponents(*currentFunction_, objectValue, objComps))
        {
            std::vector<float> picked;
            picked.reserve(static_cast<size_t>(expr->swizzleLength));
            bool ok = true;
            for (int s = 0; s < expr->swizzleLength; ++s)
            {
                const size_t lane = static_cast<size_t>(expr->swizzleIndices[s]);
                if (lane >= objComps.size()) { ok = false; break; }
                picked.push_back(objComps[lane]);
            }
            if (ok)
            {
                if (picked.size() == 1)
                    return createConstant(picked[0]);
                return createConstant(resultType, picked);
            }
        }

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
        if (IRValueID folded = tryFoldVecConstruct(resultType, argValues);
            folded != InvalidIRValue)
        {
            return folded;
        }
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

        // Single-lane swizzle write-back: `c.a = scalar;` rewrites a
        // single lane of an existing vector value.  Lower as VecInsert
        // against the variable's prior SSA value.  Multi-lane swizzle
        // assignment (`c.rgb = vec3;`) is split into per-lane VecInserts
        // — one per swizzle index — so the back-end sees a sequence of
        // single-lane overrides.
        if (memberExpr->isSwizzle && memberExpr->swizzleLength >= 1 &&
            memberExpr->object->kind == ExprKind::Identifier)
        {
            auto* ident = static_cast<IdentifierExpr*>(memberExpr->object.get());
            auto nvIt = nameToValue_.find(ident->name);
            if (nvIt != nameToValue_.end())
            {
                IRValueID currentVec = nvIt->second;
                IRTypeInfo vecType = getExprType(memberExpr->object.get());

                for (int s = 0; s < memberExpr->swizzleLength; ++s)
                {
                    const int lane = memberExpr->swizzleIndices[s];
                    IRValueID scalar = value;
                    if (memberExpr->swizzleLength > 1)
                    {
                        // For multi-lane swizzle, RHS is a vector; pull out
                        // the matching scalar lane via VecExtract.
                        auto extractInst = std::make_unique<IRInstruction>(
                            IROp::VecExtract,
                            currentFunction_->allocateValueId(),
                            IRTypeInfo::Float());
                        extractInst->addOperand(value);
                        extractInst->componentIndex = s;
                        scalar = extractInst->result;
                        currentBlock_->addInstruction(std::move(extractInst));
                    }

                    auto insertInst = std::make_unique<IRInstruction>(
                        IROp::VecInsert,
                        currentFunction_->allocateValueId(),
                        vecType);
                    insertInst->addOperand(currentVec);
                    insertInst->addOperand(scalar);
                    insertInst->componentIndex = lane;
                    currentVec = insertInst->result;
                    currentBlock_->addInstruction(std::move(insertInst));
                }

                nameToValue_[ident->name] = currentVec;

                // If the underlying identifier is an out parameter
                // carrying a semantic, fall through to the regular
                // identifier-assignment StoreOutput emit so the lane-
                // overridden vec value propagates to the output.
                if (ident->resolvedDecl &&
                    ident->resolvedDecl->kind == DeclKind::Parameter)
                {
                    auto* param = static_cast<ParamDecl*>(ident->resolvedDecl);
                    if ((param->storage == StorageQualifier::Out ||
                         param->storage == StorageQualifier::InOut) &&
                        !param->semantic.isEmpty())
                    {
                        auto inst = std::make_unique<IRInstruction>(
                            IROp::StoreOutput,
                            InvalidIRValue, IRTypeInfo::Void());
                        inst->addOperand(currentVec);
                        inst->semanticName    = param->semantic.name;
                        inst->rawSemanticName = param->semantic.rawName;
                        inst->semanticIndex   = param->semantic.index;
                        currentBlock_->addInstruction(std::move(inst));
                    }
                }
                return currentVec;
            }
        }

        // Build the composite name for the member
        if (memberExpr->object->kind == ExprKind::Identifier)
        {
            auto* ident = static_cast<IdentifierExpr*>(memberExpr->object.get());
            std::string compositeName = ident->name + "." + memberExpr->member;
            nameToValue_[compositeName] = value;

            // Check if this is an output struct - emit StoreOutput.
            // For local struct vars that get returned (`OUT.field = ...;
            // return OUT;`), the StoreOutput emit is deferred to
            // buildReturnStmt so the output param table follows the
            // struct's *field declaration order* rather than the
            // source statement order — that's what the reference
            // compiler emits.  For `out` parameter struct fields we
            // still emit inline (no return statement to anchor).
            if (ident->resolvedDecl)
            {
                const bool isLocalVar =
                    (ident->resolvedDecl->kind == DeclKind::Variable);

                // Look up struct type info for the target
                TypeNode* typeNode = nullptr;
                if (ident->resolvedDecl->kind == DeclKind::Variable)
                {
                    typeNode = static_cast<VarDecl*>(ident->resolvedDecl)->type.get();
                }

                if (!isLocalVar)
                {
                    const std::vector<StructField>* fields = getStructFields(typeNode);
                    if (fields)
                    {
                        for (const auto& field : *fields)
                        {
                            if (field.name == memberExpr->member && !field.semantic.isEmpty())
                            {
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
