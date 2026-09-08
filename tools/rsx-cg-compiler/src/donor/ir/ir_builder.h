#pragma once

#include "ir.h"
#include "ast.h"
#include "semantic.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <stack>
#include <vector>
#include <optional>

// ============================================================================
// IR Builder - Converts AST to IR
// ============================================================================

class IRBuilder
{
public:
    IRBuilder();
    ~IRBuilder();

    // Build IR from a translation unit after semantic analysis
    std::unique_ptr<IRModule> build(TranslationUnit& unit, const SemanticAnalyzer& semantic);

    // Get errors during IR generation
    const std::vector<std::string>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

    // Type conversion helpers
    static IRTypeInfo getIRType(const CgType& cgType);
    static IRTypeInfo getIRType(TypeNode* typeNode);

private:
    std::unique_ptr<IRModule> module_;
    IRFunction* currentFunction_ = nullptr;
    FunctionDecl* currentFunctionDecl_ = nullptr;
    IRBasicBlock* currentBlock_ = nullptr;
    const SemanticAnalyzer* semantic_ = nullptr;
    // Narrow evaluator for a file-scope const's initialiser.  False means
    // the value could not be computed, which the call site REFUSES rather
    // than compiling as zero.
    static bool evaluateConstInitializer(const ExprNode* init,
                                         std::vector<float>& out);
    static bool evaluateConstIntInitializer(const ExprNode* init,
                                            std::vector<int64_t>& out);
    static bool evaluateConstInitializerTyped(const ExprNode* init,
                                              const TypeNode* declType,
                                              std::vector<float>& floatOut,
                                              std::vector<int64_t>& intOut);

    // Value mapping from AST to IR
    std::unordered_map<DeclNode*, IRValueID> declToValue_;
    std::unordered_map<std::string, IRValueID> nameToValue_;
    // Immutable, function-local identities of unwritten vector-field bases.
    // Assignments replace nameToValue_ bindings; this set needs no branch snapshot.
    std::unordered_set<IRValueID> undefinedFieldBases_;
    std::unordered_map<std::string, std::vector<IRValueID>> localArrayValues_;
    // A file-scope variable a caller-local SHADOWS keeps its own binding here
    // while the local owns the name in nameToValue_ / localArrayValues_: an
    // inlined helper that names the global is bound to these for its body,
    // and its writes come back here, not to the local (t_7a4e3b36 review).
    // InvalidIRValue / an empty vector means "shadowed, never assigned" - the
    // helper's read must then fall through to the global load.
    std::unordered_map<std::string, IRValueID> shadowedGlobals_;
    std::unordered_map<std::string, std::vector<IRValueID>> shadowedGlobalArrays_;
    // One entry per inlined helper on the inline stack: the names it binds
    // itself (parameters and locals).  A helper that names a file-scope
    // variable while an ENCLOSING helper's parameter or local of that name
    // is in scope is refused by name: the flat name map would hand it the
    // enclosing binding (t_7a4e3b36 review rounds 6; the scope model that
    // resolves it properly is t_cf17f501's refactor).
    std::vector<std::unordered_set<std::string>> inlineScopes_;
    static bool functionNamesIdentifier(const FunctionDecl* fn, const std::string& name);
    std::unordered_map<IRValueID, IRValueID> identityPrefixSwizzleBase_;
    std::unordered_map<std::string, std::vector<FunctionDecl*>> functionDefinitionsByName_;
    std::vector<FunctionDecl*> inlineStack_;
    std::vector<std::string> depthDecodeUniforms_;
    // Source text is the wrong boundary for short-circuit hazards:
    // a precomputed sqrt predicate is already eager, while an inlined
    // helper called from a logical RHS is still protected by the RHS.
    // Tag instructions by where they are emitted so lowering can refuse
    // only work source semantics would have skipped.
    int shortCircuitRhsDepth_ = 0;

    // Break/continue targets for loops
    struct LoopContext
    {
        IRBasicBlock* continueTarget;
        IRBasicBlock* breakTarget;
    };
    std::stack<LoopContext> loopStack_;

    // Label counter for unique block names
    int labelCounter_ = 0;

    // Errors
    std::vector<std::string> errors_;

    // Error reporting
    void error(const std::string& msg);
    void error(const SourceLocation& loc, const std::string& msg);

    // Label generation
    std::string makeLabel(const std::string& prefix);

    // Module building
    void buildGlobals(TranslationUnit& unit);
    void buildFunction(FunctionDecl* decl);

    // True for a FRAGMENT entry's `out` parameter declared with no
    // semantic, which binds to COLOR the way the reference compiler binds
    // it (t_a15ec129).  Consulted wherever a parameter's semantic decides
    // whether a store is emitted.
    bool isDefaultedFragmentOutput(const ParamDecl* param) const;

    // Statement building
    void buildStmt(StmtNode* stmt);
    void buildBlockStmt(BlockStmt* stmt);
    void buildIfStmt(IfStmt* stmt);
    void buildForStmt(ForStmt* stmt);
    // Static-count for-loop unroll. Returns true if the loop was fully
    // unrolled into straight-line IR — caller skips the real-branch path.
    bool tryUnrollStaticFor(ForStmt* stmt);
    void buildWhileStmt(WhileStmt* stmt);
    void buildDoWhileStmt(DoWhileStmt* stmt);
    void buildSwitchStmt(SwitchStmt* stmt);
    void buildReturnStmt(ReturnStmt* stmt);
    void buildBreakStmt(BreakStmt* stmt);
    void buildContinueStmt(ContinueStmt* stmt);
    void buildDiscardStmt(DiscardStmt* stmt);
    void buildExprStmt(ExprStmt* stmt);
    void buildDeclStmt(DeclStmt* stmt);

    // Expression building - returns the IR value ID for the expression result
    IRValueID buildExpr(ExprNode* expr);
    IRValueID buildLiteralExpr(LiteralExpr* expr);
    IRValueID buildIdentifierExpr(IdentifierExpr* expr);
    IRValueID buildBinaryExpr(BinaryExpr* expr);
    IRValueID buildUnaryExpr(UnaryExpr* expr);
    IRValueID buildCallExpr(CallExpr* expr);
    bool inlineUserFunctionCall(CallExpr* expr, const std::vector<IRValueID>& args,
                                IRValueID& result);
    bool buildInlineFunctionBody(FunctionDecl* callee, IRValueID& result);
    IRValueID buildMemberAccessExpr(MemberAccessExpr* expr);
    IRValueID buildIndexExpr(IndexExpr* expr);
    IRValueID buildTernaryExpr(TernaryExpr* expr);
    IRValueID buildCastExpr(CastExpr* expr);
    IRValueID buildConstructorExpr(ConstructorExpr* expr);

    // Assignment building (separate because it modifies lvalues)
    IRValueID buildAssignment(ExprNode* target, IRValueID value);
    IRValueID coerceAssignmentValue(ExprNode* target, IRValueID value);

    // Helper to get address/location for lvalue expressions
    struct LValueInfo
    {
        enum class Kind { Local, Global, MemberAccess, ArrayIndex };
        Kind kind;
        IRValueID baseValue;
        std::string memberName;
        int memberIndex;
        IRValueID indexValue;
    };
    LValueInfo getLValueInfo(ExprNode* expr);

    // Instruction emission helpers
    IRValueID emitInstruction(IROp op, const IRTypeInfo& resultType,
                              const std::vector<IRValueID>& operands = {},
                              const SourceLocation& loc = {});
    IRValueID emitBinaryOp(IROp op, const IRTypeInfo& resultType,
                           IRValueID left, IRValueID right,
                           const SourceLocation& loc = {});
    IRValueID emitUnaryOp(IROp op, const IRTypeInfo& resultType, IRValueID operand,
                          const SourceLocation& loc = {});

    // Constant-folding helpers — return a fresh IRConstant id when
    // both operands are IRConstants, else InvalidIRValue.  Handle
    // float scalars + float vectors with implicit scalar→vector
    // broadcast.  See ir_builder.cpp for the supported op set.
    IRValueID tryFoldBinaryOp(IROp op, const IRTypeInfo& resultType,
                               IRValueID lhs, IRValueID rhs);
    IRValueID tryFoldUnaryOp(IROp op, const IRTypeInfo& resultType,
                              IRValueID operand);
    IRValueID tryFoldVecConstruct(const IRTypeInfo& resultType,
                                   const std::vector<IRValueID>& args,
                                   std::optional<BaseType> baseTypeOverride = std::nullopt);
    IRValueID emitCall(const std::string& funcName, const IRTypeInfo& resultType,
                       const std::vector<IRValueID>& args);

    void emitBranch(IRBasicBlock* target);
    void emitCondBranch(IRValueID condition, IRBasicBlock* trueTarget, IRBasicBlock* falseTarget);
    void emitReturn(IRValueID value);
    void emitStore(IRValueID address, IRValueID value);

    // Get IR type for an expression
    IRTypeInfo getExprType(ExprNode* expr);

    // Struct field resolution helper (looks up struct fields from semantic analyzer)
    const std::vector<StructField>* getStructFields(TypeNode* typeNode);

    // Emit StoreOutput instructions for all output struct fields
    void emitStructOutputs(ExprNode* structExpr, const std::vector<StructField>& fields);

    // Map binary AST operator to IR operator
    IROp binaryOpToIROp(BinaryOp op);

    // Map unary AST operator to IR operator
    IROp unaryOpToIROp(UnaryOp op);

    // Map function call to IR operation (for built-in functions)
    std::optional<IROp> builtinToIROp(const std::string& name);

    // Create a constant value
    IRValueID createConstant(bool value);
    IRValueID createConstant(int32_t value);
    IRValueID createConstant(uint32_t value);
    IRValueID createConstant(float value);
    IRValueID createConstant(const IRTypeInfo& type, float value);
    IRValueID createConstant(const IRTypeInfo& type, const std::vector<float>& values, const std::vector<int64_t>& intValues = {});
};
