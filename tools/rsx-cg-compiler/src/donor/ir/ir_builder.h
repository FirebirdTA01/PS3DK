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
    // ================================================================
    // PER-SCOPE STATE, in ONE place (t_4ac44f78).  Every map that binds a
    // NAME to a value lives in ScopeState, and every scope boundary handles
    // the whole struct - the three boundaries are the rows of this table,
    // and a new map is wrong until it has an answer in every cell (block exit
    // is keyed on the names the block DECLARED, never on the keys it touched):
    //
    //   map                   function entry  if-join (buildIfStmt)   inline call                 block exit (buildBlockStmt)
    //   names                 clear()         snapshot/restore/join   save/restore + propagate    declared names restored/unbound
    //   arrays                clear()         join PER ELEMENT "a[i]" save/restore + propagate    declared names restored/unbound
    //   shadowedGlobals       clear()         join "G@"               save/restore + propagate    entries the block created UNSTASHED
    //   shadowedGlobalArrays  clear()         join "B@[i]"            save/restore + propagate    entries the block created UNSTASHED
    //
    // History: nameToValue_ was joined from the start; localArrayValues_
    // was not snapshotted, restored or joined for as long as it existed
    // (t_cf17f501 - a conditional element store applied unconditionally);
    // the two stash maps were added and missed the SAME way within an hour
    // (t_7a4e3b36 review).  The aliases below keep the historical member
    // names so the ~60 binding sites read unchanged; the struct is what the
    // boundaries copy, so a fifth map added HERE is carried everywhere, and
    // one added anywhere else is the bug this comment exists to prevent.
    struct ScopeState
    {
        std::unordered_map<std::string, IRValueID> names;
        std::unordered_map<std::string, std::vector<IRValueID>> arrays;
        // A file-scope variable a caller-local SHADOWS keeps its own binding
        // here while the local owns the name in `names` / `arrays`: an
        // inlined helper that names the global is bound to these for its
        // body, and its writes come back here, not to the local.
        // InvalidIRValue / an empty vector means "shadowed, never assigned"
        // - the helper's read must then fall through to the global load.
        std::unordered_map<std::string, IRValueID> shadowedGlobals;
        std::unordered_map<std::string, std::vector<IRValueID>> shadowedGlobalArrays;

        void clear()
        {
            names.clear(); arrays.clear();
            shadowedGlobals.clear(); shadowedGlobalArrays.clear();
        }
        // The if-join works over ONE flat map of join keys: a name is its
        // own key, an array element is "a[i]", a stashed global "G@" and a
        // stashed element "B@[i]" ('[' and '@' cannot occur in identifiers).
        // Only bound values fold; unfold() puts the joined values back.
        std::unordered_map<std::string, IRValueID> fold() const;
        void unfold(std::unordered_map<std::string, IRValueID>& joined);
    };
    ScopeState scope_;
    std::unordered_map<std::string, IRValueID>& nameToValue_ = scope_.names;
    // Immutable, function-local identities of unwritten vector-field bases.
    // Assignments replace nameToValue_ bindings; this set needs no branch snapshot.
    std::unordered_set<IRValueID> undefinedFieldBases_;
    std::unordered_map<std::string, std::vector<IRValueID>>& localArrayValues_ = scope_.arrays;
    std::unordered_map<std::string, IRValueID>& shadowedGlobals_ = scope_.shadowedGlobals;
    std::unordered_map<std::string, std::vector<IRValueID>>& shadowedGlobalArrays_ = scope_.shadowedGlobalArrays;
    // Move a file-scope variable's binding into the stash when the current
    // function binds that name itself (a local or a parameter); no-op if
    // the name is not a global or is already stashed.
    void stashShadowedGlobal(const std::string& name);
    // BLOCK EXIT (t_7396e0c2): the names each open block DECLARED, one set per
    // block, pushed by buildBlockStmt and filled by buildDeclStmt as it reaches
    // each declarator - never pre-scanned, so a use before the declaration still
    // names the outer binding (oracle: `float4 r = G; float4 G = ...` reads the
    // global).  At block exit exactly these names are undone; every other key
    // the block touched (assignments to outer names, struct-field bases, loop
    // counters, join-time loads) is the block's effect on its enclosing scope
    // and survives.  An inlined helper pushes its own frame so its top-level
    // locals never land in the caller's block.
    std::vector<std::unordered_set<std::string>> blockDeclared_;
    void exitBlockBinding(const std::string& name, const ScopeState& pre);
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
