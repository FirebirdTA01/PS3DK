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
#include <functional>

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
    // `module` lets an identifier in the initialiser resolve to a file-scope
    // const that was already folded (file-scope-initializer-fold); without it identifiers are
    // not constants.
    static bool evaluateConstInitializerTyped(const ExprNode* init,
                                              const TypeNode* declType,
                                              std::vector<float>& floatOut,
                                              std::vector<int64_t>& intOut,
                                              IRModule* module = nullptr,
                                              bool allowBuiltinCalls = true);

    // Value mapping from AST to IR
    std::unordered_map<DeclNode*, IRValueID> declToValue_;
    // ================================================================
    // PER-SCOPE STATE, in ONE place (inline-scope-state).  Every map that binds a
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
    // (local-array-conditional-store - a conditional element store applied unconditionally);
    // the two stash maps were added and missed the SAME way within an hour
    // (user-function-inlining review).  The aliases below keep the historical member
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
        // An inlined helper's array PARAMETER bound to a file-scope uniform
        // array by name: indexing the parameter reads the global's element.
        // Valid only while the parameter still holds the value it was bound
        // to (a write or a shadowing local ends it).  param -> (global, value).
        std::unordered_map<std::string, std::pair<std::string, IRValueID>> uniformArrayAliases;

        void clear()
        {
            names.clear(); arrays.clear();
            shadowedGlobals.clear(); shadowedGlobalArrays.clear();
            uniformArrayAliases.clear();
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
    // Implicit index of each semantic-less member of the entry's returned
    // struct that a return writes (TEXCOORD in a vertex program, COLOR in a
    // fragment program), fixed at its first return so every return agrees.
    std::unordered_map<std::string, int> implicitOutputIndex_;
    // Uniform struct ENTRY parameters flattened into one uniform per member
    // (uniform-struct-entry-parameter): the name has no whole-struct value,
    // only `name.member...` bindings, so a whole-struct copy copies those.
    std::unordered_set<std::string> flattenedUniformStructParams_;
    // Values an inlined call returns (t_8c4a982e): the inputs they depend
    // on claim an implicit TEXCOORD even when the call's result is dead.
    std::vector<IRValueID> implicitClaimRoots_;
    // Argument i of the call is a flattened uniform struct parameter passed
    // whole to a struct-typed in parameter: bound member by member.
    bool flattenedStructArgument(const CallExpr* call, size_t i);
    // The expression an expression STATEMENT is evaluating: a whole-struct
    // copy of a flattened parameter has no value, so it is accepted only
    // there (or as a link of that statement's assignment chain).
    const ExprNode* statementExpr_ = nullptr;
    std::unordered_map<std::string, std::vector<IRValueID>>& localArrayValues_ = scope_.arrays;
    std::unordered_map<std::string, IRValueID>& shadowedGlobals_ = scope_.shadowedGlobals;
    std::unordered_map<std::string, std::vector<IRValueID>>& shadowedGlobalArrays_ = scope_.shadowedGlobalArrays;
    // Move a file-scope variable's binding into the stash when the current
    // function binds that name itself (a local or a parameter); no-op if
    // the name is not a global or is already stashed.
    void stashShadowedGlobal(const std::string& name);
    // BLOCK EXIT (dead-branch-local-predication): the names each open block DECLARED, one set per
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
    int inlineReturnDepth_ = 0;   // nested run-time returns being lowered (runInlineStatements)
    // Statements an enclosing arm / constant branch still runs after the
    // current list (a nested run-time return runs them inside its arms),
    // and whether that happened.  Per inlined helper.
    std::vector<StmtNode*> inlineContinuation_;
    bool inlineContinuationRun_ = false;
    size_t inlineContinuationFrame_ = 0;   // blockDeclared_ depth the continuation was captured at
    // A helper that RETURNED A STRUCT BY VALUE carries its tracked fields
    // (the per-element arrays and leaf bindings of `return p;`) back to the
    // caller instead of losing them at the scope restore below the call.
    //
    // `inlineStructReturnBaseSet_` is the CUMULATIVE set of struct-local
    // names this inliner body RETURNS BY VALUE across every return statement
    // it builds (single-`return p;` shape: one name; `if (c) return p;
    // return r;` shape: two).  Each inlining frame snapshots the set before
    // building its body, and takes the difference with the set after, to
    // scope this body's base names.  A nested helper called from a body
    // (e.g. `compute` inside `computePlan`) has its own snapshot boundary
    // and never leaks into the outer callee's base set.
    //
    // The remap under the caller's destination is installed ONLY when this
    // body's base set has exactly ONE name AND the frame built ZERO non-local
    // returns (see inlineStructReturnNonlocalCount_) - the callee returns
    // exactly one struct local, and no other arm (a call, a constructor, a
    // member, a ternary, or a parameter) is being picked at run time.
    // When the set has MORE THAN ONE name (a per-arm choice the runtime
    // makes), each arm names a DIFFERENT local and the caller cannot know
    // which arm the inliner's `result` corresponds to; in that case the
    // inliner must NOT remap (leaving the caller's copy to fire the correct
    // member-array-storage refusal), never installing one arm's storage on
    // top of the other's.
    //
    // `structReturnDest_` binds the caller's destination local (`q` in
    // `Plan q = compute(...)`) to the EXACT CallExpr being initialised.
    // buildDeclStmt records it, keyed by the CallExpr it is evaluating; an
    // inlining frame reads it only for its own `expr`.  A nested inliner -
    // a helper called from a body, or a call that is an ARGUMENT (e.g.
    // compute in `Plan q = wrap(compute(t))`, inlined while wrap's argument
    // is being built, ahead of wrap's own frame) - looks its own CallExpr
    // up and finds nothing, so it can never claim the caller's
    // destination.  A single global name (the earlier design) would let
    // that nested inliner run first, consume the name, and install ITS
    // returned struct's storage onto the caller's `q` - a silent wrong
    // value.
    // `structReturnInstalled_` records which CallExprs had their struct
    // return installed, so buildDeclStmt skips the caller-side copy for
    // exactly that call.
    // `inlineStructReturnNonlocalCount_` counts every struct return a frame
    // builds that does NOT name a local (a call, a constructor, a member, a
    // ternary, or a parameter).  Such a return adds no base name, yet it is
    // one of "the struct returns this frame builds" - the arm the caller
    // cannot see.  The remap is installed only when the frame built exactly
    // ONE base-name return AND ZERO non-locals (inlineStructReturnBaseSet
    // size one, this counter zero).  A mixed frame like `if (c) return mk(a);
    // return r;` (one base name, one non-local) refuses instead of installing
    // r on top of the mk(a) arm.
    std::unordered_set<std::string> inlineStructReturnBaseSet_;
    std::unordered_map<const CallExpr*, std::string> structReturnDest_;
    std::unordered_set<const CallExpr*> structReturnInstalled_;
    size_t inlineStructReturnNonlocalCount_ = 0;
    // Declaration identity prevents a local shadow from using a global's initializer.
    std::unordered_set<const DeclNode*> globalDeclarations_;
    // Mutable statics whose initialiser the constant evaluator could not
    // fold; built at the entry's start instead (buildFunction).
    std::vector<VarDecl*> runtimeStaticInits_;
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
    // it (implicit-colour-output).  Consulted wherever a parameter's semantic decides
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
    // A fragment `for` that cannot be expanded becomes a hardware loop
    // region (IROp::LoopCarry .. LoopOut, ir.h).  Returns false, having
    // built nothing, when the loop is outside the admitted shape.
    bool tryBuildHardwareLoop(ForStmt* stmt);
    // arrayStorageKey resolving a row index bound to an integer constant.
    bool storageKey(ExprNode* expr, std::string& key);
    // Values bound to names a hardware loop's body assigned without
    // carrying them: reading one after the loop is refused by name.
    std::unordered_set<IRValueID> loopPoison_;
    int hardwareLoopDepth_ = 0;
    // The run-time conditions of the if-arms being built, innermost last,
    // with the arm's polarity (then = true).  A flattened if executes both
    // arms, so a vertex branch loop inside one ANDs these into its exit
    // test (VP-CF, codex S3-CONTROL): an untaken arm's loop takes zero trips
    // instead of running - unboundedly - for a result the join discards.
    std::vector<std::pair<IRValueID, bool>> armConditions_;
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
    IRValueID materialiseInitialiser(const IRTypeInfo& type, const std::vector<float>& values,
                                     const std::vector<int64_t>& intValues);
    IRValueID foldStaticConstArrayElement(const IRGlobal& global, int index);
    IRValueID buildBinaryExpr(BinaryExpr* expr);
    IRValueID buildUnaryExpr(UnaryExpr* expr);
    IRValueID buildCallExpr(CallExpr* expr);
    IRValueID buildCallExprImpl(CallExpr* expr);
    void computeClaimedImplicitInputs();
    using StructLeafBindings = std::vector<std::pair<std::string, IRValueID>>;
    bool inlineUserFunctionCall(CallExpr* expr, const std::vector<IRValueID>& args,
                                IRValueID& result,
                                const std::vector<StructLeafBindings>& structArguments);
    bool buildInlineFunctionBody(FunctionDecl* callee, IRValueID& result);
    void buildConditional(ExprNode* condition, SourceLocation loc,
                          const std::function<void()>& thenArm,
                          const std::function<void()>* elseArm);
    // The truth of an if-condition that is a compile-time constant, by the
    // file-scope initialiser evaluator (typed values, static consts only),
    // when no name in it is shadowed by a local or parameter.  nullopt for
    // anything else.
    std::optional<bool> constCondition(const ExprNode* e);
    // True when a constant-evaluable part of a run-time condition is one
    // constCondition declines to fold (precision the reference does not use).
    bool conditionFoldHazard(const ExprNode* e);
    bool runInlineStatements(FunctionDecl* callee, const std::vector<StmtNode*>& statements,
                             IRValueID& result, bool& sawReturn);
    IRValueID buildMemberAccessExpr(MemberAccessExpr* expr);
    bool emitInputMemberLoad(const ParamDecl* param, const std::string& member, IRValueID valueId);
    IRValueID buildIndexExpr(IndexExpr* expr);
    IRValueID buildTernaryExpr(TernaryExpr* expr);
    IRValueID buildCastExpr(CastExpr* expr);
    IRValueID buildConstructorExpr(ConstructorExpr* expr);

    // Assignment building (separate because it modifies lvalues)
    IRValueID buildAssignment(ExprNode* target, IRValueID value);
    bool resolveTrackedArrayElement(IndexExpr* expr, std::string& key, int32_t& index);
    IRValueID readTrackedArrayElement(IndexExpr* expr, const std::string& key, int32_t index);
    bool copyArrayAggregate(const std::string& destination, ExprNode* source, TypeNode* type);
    bool buildLocalStructInitializer(const std::string& destination, TypeNode* type, ExprNode* source,
                                     bool replaceExisting = false,
                                     StructLeafBindings* captured = nullptr);
    void emitEntryStructOutputs(ExprNode* target);
    IRValueID emitScalarNarrowing(const IRTypeInfo& sourceType, const IRTypeInfo& targetType,
                                  IRValueID value, const SourceLocation& loc);
    IRValueID narrowToScalar(TypeNode* declared, ExprNode* valueExpr, IRValueID value);
    IRValueID emitScalarConversion(const IRTypeInfo& fromType, const IRTypeInfo& targetType,
                                   IRValueID lane, const SourceLocation& loc);
    IRValueID coerceAssignmentValue(ExprNode* target, IRValueID value);
    IRValueID foldFixedConstantConversion(TypeNode* target, IRValueID value);
    IRValueID emitScalarVectorBroadcast(const IRTypeInfo& sourceType, const IRTypeInfo& targetType,
                                        IRValueID value, const SourceLocation& loc,
                                        std::optional<BaseType> targetBase = std::nullopt);
    IRValueID emitScalarMatrixBroadcast(const IRTypeInfo& sourceType, const IRTypeInfo& targetType,
                                        IRValueID value, const SourceLocation& loc);
    IRValueID emitMatrixComponents(const IRTypeInfo& sourceType, const IRTypeInfo& targetType,
                                   IRValueID value, const SourceLocation& loc);
    IRValueID prepareFloatMatrixScalar(const IRTypeInfo& sourceType, IRValueID value,
                                      const SourceLocation& loc);

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
    IRValueID constructedMatrixRow(IRValueID matrix, int row,
                                    const IRTypeInfo& rowType) const;
    IRValueID emitInstruction(IROp op, const IRTypeInfo& resultType,
                              const std::vector<IRValueID>& operands = {},
                              const SourceLocation& loc = {});
    IRValueID emitBinaryOp(IROp op, const IRTypeInfo& resultType,
                           IRValueID left, IRValueID right,
                           const SourceLocation& loc = {});
    IRValueID emitUnaryOp(IROp op, const IRTypeInfo& resultType, IRValueID operand,
                          const SourceLocation& loc = {});
    IRValueID emitNumericToBool(const IRTypeInfo& sourceType, const IRTypeInfo& targetType,
                               IRValueID operand, const SourceLocation& loc);
    IRValueID normalizeCondition(ExprNode* expr, IRValueID value);

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
    bool containsMatrixField(TypeNode* typeNode);
    const IRGlobal* uniformStructSourceRoot(ExprNode* source);
    bool hasBoundStructLeaves(TypeNode* type, ExprNode* source);
    bool captureUniformStructElement(TypeNode* type, IndexExpr* source,
                                     IRValueID index, StructLeafBindings& leaves);

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

    // Narrowing helpers for vector and matrix casts and constructors (narrowing-aggregate-casts)
    IRValueID emitVectorNarrowing(const IRTypeInfo& sourceType, const IRTypeInfo& targetType, IRValueID operandValue, SourceLocation loc, std::optional<BaseType> baseTypeOverride = std::nullopt);
    IRValueID emitMatrixNarrowing(const IRTypeInfo& sourceType, const IRTypeInfo& targetType, IRValueID operandValue, SourceLocation loc);
};
