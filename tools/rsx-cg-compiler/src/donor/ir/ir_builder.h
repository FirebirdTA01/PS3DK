#pragma once

#include "ir.h"
#include "ast.h"
#include "semantic.h"
#include <cstdint>
#include <unordered_map>
#include <stack>

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

private:
    std::unique_ptr<IRModule> module_;
    IRFunction* currentFunction_ = nullptr;
    FunctionDecl* currentFunctionDecl_ = nullptr;
    IRBasicBlock* currentBlock_ = nullptr;
    const SemanticAnalyzer* semantic_ = nullptr;

    // Value mapping from AST to IR
    std::unordered_map<DeclNode*, IRValueID> declToValue_;
    std::unordered_map<std::string, IRValueID> nameToValue_;

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
    IRValueID buildMemberAccessExpr(MemberAccessExpr* expr);
    IRValueID buildIndexExpr(IndexExpr* expr);
    IRValueID buildTernaryExpr(TernaryExpr* expr);
    IRValueID buildCastExpr(CastExpr* expr);
    IRValueID buildConstructorExpr(ConstructorExpr* expr);

    // Assignment building (separate because it modifies lvalues)
    IRValueID buildAssignment(ExprNode* target, IRValueID value);

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
                              const std::vector<IRValueID>& operands = {});
    IRValueID emitBinaryOp(IROp op, const IRTypeInfo& resultType,
                           IRValueID left, IRValueID right);
    IRValueID emitUnaryOp(IROp op, const IRTypeInfo& resultType, IRValueID operand);

    // Constant-folding helpers — return a fresh IRConstant id when
    // both operands are IRConstants, else InvalidIRValue.  Handle
    // float scalars + float vectors with implicit scalar→vector
    // broadcast.  See ir_builder.cpp for the supported op set.
    IRValueID tryFoldBinaryOp(IROp op, const IRTypeInfo& resultType,
                               IRValueID lhs, IRValueID rhs);
    IRValueID tryFoldUnaryOp(IROp op, const IRTypeInfo& resultType,
                              IRValueID operand);
    IRValueID tryFoldVecConstruct(const IRTypeInfo& resultType,
                                   const std::vector<IRValueID>& args);
    IRValueID emitCall(const std::string& funcName, const IRTypeInfo& resultType,
                       const std::vector<IRValueID>& args);

    void emitBranch(IRBasicBlock* target);
    void emitCondBranch(IRValueID condition, IRBasicBlock* trueTarget, IRBasicBlock* falseTarget);
    void emitReturn(IRValueID value);
    void emitStore(IRValueID address, IRValueID value);

    // Type conversion helpers
    IRTypeInfo getIRType(const CgType& cgType);
    IRTypeInfo getIRType(TypeNode* typeNode);

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
    IRValueID createConstant(const IRTypeInfo& type, const std::vector<float>& values);
};
