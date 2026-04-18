#pragma once

#include "ir.h"
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <functional>

// ============================================================================
// Optimization Pass Interface
// ============================================================================

// Statistics about what a pass did
struct PassStats
{
    int instructionsRemoved = 0;
    int instructionsAdded = 0;
    int blocksRemoved = 0;
    int constantsFolded = 0;
    int copiesPropagated = 0;

    void reset() {
        instructionsRemoved = instructionsAdded = blocksRemoved = 0;
        constantsFolded = copiesPropagated = 0;
    }

    bool madeChanges() const {
        return instructionsRemoved > 0 || instructionsAdded > 0 ||
               blocksRemoved > 0 || constantsFolded > 0 || copiesPropagated > 0;
    }
};

// Base class for optimization passes
class IRPass
{
public:
    virtual ~IRPass() = default;

    // Get pass name
    virtual const char* getName() const = 0;

    // Run pass on a function, returns true if changes were made
    virtual bool runOnFunction(IRFunction& func) = 0;

    // Get statistics from last run
    const PassStats& getStats() const { return m_stats; }

protected:
    PassStats m_stats;
};

// ============================================================================
// Dead Code Elimination
// ============================================================================

// Removes instructions whose results are never used
class DeadCodeElimination : public IRPass
{
public:
    const char* getName() const override { return "DeadCodeElimination"; }
    bool runOnFunction(IRFunction& func) override;

private:
    // Find all used values (working backwards from outputs)
    void computeUsedValues(IRFunction& func);

    // Check if an instruction has side effects
    bool hasSideEffects(const IRInstruction* inst) const;

    // Check if instruction might produce shader output (conservative for entry points)
    bool mightBeShaderOutput(const IRInstruction* inst) const;

    std::unordered_set<IRValueID> m_usedValues;
    bool m_isEntryPoint = false;
};

// ============================================================================
// Constant Folding
// ============================================================================

// Evaluates constant expressions at compile time
class ConstantFolding : public IRPass
{
public:
    const char* getName() const override { return "ConstantFolding"; }
    bool runOnFunction(IRFunction& func) override;

private:
    // Try to fold a single instruction
    bool tryFoldInstruction(IRFunction& func, IRInstruction* inst);

    // Evaluate arithmetic operations
    IRConstant* foldBinaryOp(IRFunction& func, IROp op,
                             const IRConstant* lhs, const IRConstant* rhs);

    // Evaluate unary operations
    IRConstant* foldUnaryOp(IRFunction& func, IROp op, const IRConstant* operand);

    // Evaluate math functions
    IRConstant* foldMathOp(IRFunction& func, IROp op,
                           const std::vector<const IRConstant*>& args);

    // Check if a value is a constant
    const IRConstant* getConstant(IRFunction& func, IRValueID id) const;

    // Map from folded instruction result to replacement constant
    std::unordered_map<IRValueID, IRValueID> m_replacements;
};

// ============================================================================
// Copy Propagation
// ============================================================================

// Replaces uses of copied values with original values
class CopyPropagation : public IRPass
{
public:
    const char* getName() const override { return "CopyPropagation"; }
    bool runOnFunction(IRFunction& func) override;

private:
    // Find copy relationships
    void findCopies(IRFunction& func);

    // Get the root source of a copy chain
    IRValueID getRootSource(IRValueID id) const;

    // Map from copy destination to copy source
    std::unordered_map<IRValueID, IRValueID> m_copyMap;
};

// ============================================================================
// Common Subexpression Elimination
// ============================================================================

// Removes redundant computations
class CommonSubexprElimination : public IRPass
{
public:
    const char* getName() const override { return "CommonSubexprElimination"; }
    bool runOnFunction(IRFunction& func) override;

private:
    // Hash an instruction for comparison
    struct InstrHash
    {
        size_t operator()(const IRInstruction* inst) const;
    };

    // Check if two instructions are equivalent
    struct InstrEqual
    {
        bool operator()(const IRInstruction* a, const IRInstruction* b) const;
    };

    // Map from instruction signature to existing result
    std::unordered_map<const IRInstruction*, IRValueID, InstrHash, InstrEqual> m_exprMap;
};

// ============================================================================
// Algebraic Simplification
// ============================================================================

// Applies algebraic identities to simplify expressions
class AlgebraicSimplification : public IRPass
{
public:
    const char* getName() const override { return "AlgebraicSimplification"; }
    bool runOnFunction(IRFunction& func) override;

private:
    // Try to simplify a single instruction
    bool trySimplify(IRFunction& func, IRInstruction* inst);

    // Specific simplifications
    bool simplifyAdd(IRFunction& func, IRInstruction* inst);
    bool simplifyMul(IRFunction& func, IRInstruction* inst);
    bool simplifySub(IRFunction& func, IRInstruction* inst);
    bool simplifyDiv(IRFunction& func, IRInstruction* inst);

    // Check if a value is a specific constant
    bool isZero(IRFunction& func, IRValueID id) const;
    bool isOne(IRFunction& func, IRValueID id) const;
    bool isNegOne(IRFunction& func, IRValueID id) const;

    std::unordered_map<IRValueID, IRValueID> m_replacements;
};

// ============================================================================
// Dead Block Elimination
// ============================================================================

// Removes unreachable basic blocks
class DeadBlockElimination : public IRPass
{
public:
    const char* getName() const override { return "DeadBlockElimination"; }
    bool runOnFunction(IRFunction& func) override;

private:
    // Find all reachable blocks from entry
    void findReachableBlocks(IRFunction& func);

    std::unordered_set<IRBasicBlock*> m_reachableBlocks;
};

// ============================================================================
// Instruction Combining
// ============================================================================

// Combines multiple instructions into more efficient forms
class InstructionCombining : public IRPass
{
public:
    const char* getName() const override { return "InstructionCombining"; }
    bool runOnFunction(IRFunction& func) override;

private:
    // Try to combine mul+add into mad
    bool tryCombineMulAdd(IRFunction& func, IRBasicBlock* block);

    // Try to combine normalize (dp + rsq + mul)
    bool tryCombineNormalize(IRFunction& func, IRBasicBlock* block);
};

// ============================================================================
// Pass Manager
// ============================================================================

enum class OptimizationLevel
{
    O0 = 0,     // No optimization
    O1 = 1,     // Basic: DCE, constant folding, copy propagation
    O2 = 2,     // Moderate: O1 + CSE, algebraic simplification
    O3 = 3,     // Aggressive: O2 + instruction combining
    O4 = 4      // Maximum: O3 + multiple iterations
};

class PassManager
{
public:
    explicit PassManager(OptimizationLevel level = OptimizationLevel::O3);

    // Add a custom pass
    void addPass(std::unique_ptr<IRPass> pass);

    // Run all passes on a module
    bool runOnModule(IRModule& module);

    // Run all passes on a function
    bool runOnFunction(IRFunction& func);

    // Get combined statistics
    const PassStats& getStats() const { return m_totalStats; }

    // Set maximum iterations for iterative passes
    void setMaxIterations(int n) { m_maxIterations = n; }

    // Enable/disable verbose logging
    void setVerbose(bool v) { m_verbose = v; }

private:
    // Configure passes based on optimization level
    void configurePasses(OptimizationLevel level);

    std::vector<std::unique_ptr<IRPass>> m_passes;
    PassStats m_totalStats;
    int m_maxIterations = 4;
    bool m_verbose = false;
};

// ============================================================================
// Utility Functions
// ============================================================================

namespace IRPassUtils
{
    // Check if an instruction is a terminator
    bool isTerminator(IROp op);

    // Check if an instruction is a pure operation (no side effects)
    bool isPure(IROp op);

    // Check if an instruction is a copy (move with no modification)
    bool isCopy(const IRInstruction* inst);

    // Get the definition instruction for a value
    IRInstruction* getDefinition(IRFunction& func, IRValueID id);

    // Count uses of a value
    int countUses(IRFunction& func, IRValueID id);

    // Replace all uses of a value with another
    void replaceAllUses(IRFunction& func, IRValueID oldVal, IRValueID newVal);
}
