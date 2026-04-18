#include "ir_passes.h"
#include <algorithm>
#include <cmath>
#include <iostream>

// ============================================================================
// Utility Functions
// ============================================================================

namespace IRPassUtils
{

bool isTerminator(IROp op)
{
    switch (op)
    {
    case IROp::Branch:
    case IROp::CondBranch:
    case IROp::Return:
    case IROp::Discard:
        return true;
    default:
        return false;
    }
}

bool isPure(IROp op)
{
    switch (op)
    {
    // Side-effecting operations
    case IROp::Store:
    case IROp::StoreOutput:
    case IROp::StoreVarying:
    case IROp::Branch:
    case IROp::CondBranch:
    case IROp::Return:
    case IROp::Discard:
    case IROp::Call:
    case IROp::TexSample:
    case IROp::TexSampleLod:
    case IROp::TexSampleGrad:
    case IROp::TexSampleProj:
    case IROp::TexFetch:
        return false;
    default:
        return true;
    }
}

bool isCopy(const IRInstruction* inst)
{
    // A copy is a VecShuffle with identity swizzle or a simple assignment
    if (inst->op == IROp::VecShuffle)
    {
        // Identity swizzle: xyzw = 0x1B (0b00011011)
        return inst->swizzleMask == 0x1B || inst->swizzleMask == 0xE4;
    }
    return false;
}

IRInstruction* getDefinition(IRFunction& func, IRValueID id)
{
    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            if (inst->result == id)
            {
                return inst.get();
            }
        }
    }
    return nullptr;
}

int countUses(IRFunction& func, IRValueID id)
{
    int count = 0;
    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            for (IRValueID op : inst->operands)
            {
                if (op == id) count++;
            }
        }
    }
    return count;
}

void replaceAllUses(IRFunction& func, IRValueID oldVal, IRValueID newVal)
{
    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            for (auto& op : inst->operands)
            {
                if (op == oldVal)
                {
                    op = newVal;
                }
            }
        }
    }
}

} // namespace IRPassUtils

// ============================================================================
// Dead Code Elimination
// ============================================================================

bool DeadCodeElimination::runOnFunction(IRFunction& func)
{
    m_stats.reset();
    m_usedValues.clear();
    m_isEntryPoint = func.isEntryPoint;

    // Step 1: Find all values that are actually used
    computeUsedValues(func);

    // Step 2: Remove instructions whose results are never used
    bool changed = false;
    for (auto& block : func.blocks)
    {
        auto it = block->instructions.begin();
        while (it != block->instructions.end())
        {
            IRInstruction* inst = it->get();

            // Keep instructions with no result (side effects) or with used results
            bool keep = true;
            if (inst->result != InvalidIRValue)
            {
                // Has a result - only keep if used
                if (m_usedValues.find(inst->result) == m_usedValues.end())
                {
                    // Result not used - check for side effects or potential outputs
                    if (!hasSideEffects(inst) && !mightBeShaderOutput(inst))
                    {
                        keep = false;
                    }
                }
            }
            else
            {
                // No result - always keep (terminators, stores, etc.)
            }

            if (!keep)
            {
                it = block->instructions.erase(it);
                m_stats.instructionsRemoved++;
                changed = true;
            }
            else
            {
                ++it;
            }
        }
    }

    return changed;
}

void DeadCodeElimination::computeUsedValues(IRFunction& func)
{
    // Start with outputs and work backwards
    std::vector<IRValueID> worklist;

    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            // All operands of side-effecting instructions are used
            if (hasSideEffects(inst.get()) || inst->result == InvalidIRValue)
            {
                for (IRValueID op : inst->operands)
                {
                    if (op != InvalidIRValue && m_usedValues.find(op) == m_usedValues.end())
                    {
                        m_usedValues.insert(op);
                        worklist.push_back(op);
                    }
                }
            }
        }
    }

    // Propagate: if a value is used, all its operands are used
    while (!worklist.empty())
    {
        IRValueID id = worklist.back();
        worklist.pop_back();

        // Find the instruction that defines this value
        IRInstruction* def = IRPassUtils::getDefinition(func, id);
        if (def)
        {
            for (IRValueID op : def->operands)
            {
                if (op != InvalidIRValue && m_usedValues.find(op) == m_usedValues.end())
                {
                    m_usedValues.insert(op);
                    worklist.push_back(op);
                }
            }
        }
    }
}

bool DeadCodeElimination::hasSideEffects(const IRInstruction* inst) const
{
    return !IRPassUtils::isPure(inst->op);
}

bool DeadCodeElimination::mightBeShaderOutput(const IRInstruction* inst) const
{
    // Now that IR builder properly emits StoreOutput instructions,
    // DCE can track outputs through normal use-def chains.
    // No need for conservative heuristics.
    (void)inst;  // Suppress unused parameter warning
    return false;
}

// ============================================================================
// Constant Folding
// ============================================================================

bool ConstantFolding::runOnFunction(IRFunction& func)
{
    m_stats.reset();
    m_replacements.clear();

    bool changed = false;

    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            if (tryFoldInstruction(func, inst.get()))
            {
                changed = true;
            }
        }
    }

    // Apply replacements
    if (!m_replacements.empty())
    {
        for (auto& block : func.blocks)
        {
            for (auto& inst : block->instructions)
            {
                for (auto& op : inst->operands)
                {
                    auto it = m_replacements.find(op);
                    if (it != m_replacements.end())
                    {
                        op = it->second;
                    }
                }
            }
        }
    }

    return changed;
}

bool ConstantFolding::tryFoldInstruction(IRFunction& func, IRInstruction* inst)
{
    // Check if all operands are constants
    std::vector<const IRConstant*> constOperands;
    bool allConst = true;

    for (IRValueID op : inst->operands)
    {
        const IRConstant* c = getConstant(func, op);
        if (c)
        {
            constOperands.push_back(c);
        }
        else
        {
            allConst = false;
            break;
        }
    }

    if (!allConst || constOperands.empty())
    {
        return false;
    }

    IRConstant* result = nullptr;

    // Try to fold based on operation type
    switch (inst->op)
    {
    case IROp::Add:
    case IROp::Sub:
    case IROp::Mul:
    case IROp::Div:
        if (constOperands.size() == 2)
        {
            result = foldBinaryOp(func, inst->op, constOperands[0], constOperands[1]);
        }
        break;

    case IROp::Neg:
        if (constOperands.size() == 1)
        {
            result = foldUnaryOp(func, inst->op, constOperands[0]);
        }
        break;

    case IROp::Sin:
    case IROp::Cos:
    case IROp::Sqrt:
    case IROp::RSqrt:
    case IROp::Abs:
    case IROp::Floor:
    case IROp::Ceil:
    case IROp::Frac:
        result = foldMathOp(func, inst->op, constOperands);
        break;

    default:
        break;
    }

    if (result)
    {
        m_replacements[inst->result] = result->id;
        m_stats.constantsFolded++;
        return true;
    }

    return false;
}

IRConstant* ConstantFolding::foldBinaryOp(IRFunction& func, IROp op,
                                          const IRConstant* lhs, const IRConstant* rhs)
{
    // Only handle float constants for now
    if (!std::holds_alternative<float>(lhs->value) ||
        !std::holds_alternative<float>(rhs->value))
    {
        return nullptr;
    }

    float a = std::get<float>(lhs->value);
    float b = std::get<float>(rhs->value);
    float result;

    switch (op)
    {
    case IROp::Add: result = a + b; break;
    case IROp::Sub: result = a - b; break;
    case IROp::Mul: result = a * b; break;
    case IROp::Div: result = (b != 0.0f) ? a / b : 0.0f; break;
    default: return nullptr;
    }

    return func.createConstant(IRTypeInfo::Float(), result);
}

IRConstant* ConstantFolding::foldUnaryOp(IRFunction& func, IROp op, const IRConstant* operand)
{
    if (!std::holds_alternative<float>(operand->value))
    {
        return nullptr;
    }

    float a = std::get<float>(operand->value);
    float result;

    switch (op)
    {
    case IROp::Neg: result = -a; break;
    default: return nullptr;
    }

    return func.createConstant(IRTypeInfo::Float(), result);
}

IRConstant* ConstantFolding::foldMathOp(IRFunction& func, IROp op,
                                        const std::vector<const IRConstant*>& args)
{
    if (args.empty() || !std::holds_alternative<float>(args[0]->value))
    {
        return nullptr;
    }

    float a = std::get<float>(args[0]->value);
    float result;

    switch (op)
    {
    case IROp::Sin: result = std::sin(a); break;
    case IROp::Cos: result = std::cos(a); break;
    case IROp::Sqrt: result = (a >= 0.0f) ? std::sqrt(a) : 0.0f; break;
    case IROp::RSqrt: result = (a > 0.0f) ? 1.0f / std::sqrt(a) : 0.0f; break;
    case IROp::Abs: result = std::abs(a); break;
    case IROp::Floor: result = std::floor(a); break;
    case IROp::Ceil: result = std::ceil(a); break;
    case IROp::Frac: result = a - std::floor(a); break;
    default: return nullptr;
    }

    return func.createConstant(IRTypeInfo::Float(), result);
}

const IRConstant* ConstantFolding::getConstant(IRFunction& func, IRValueID id) const
{
    IRValue* val = func.getValue(id);
    if (val)
    {
        return dynamic_cast<const IRConstant*>(val);
    }
    return nullptr;
}

// ============================================================================
// Copy Propagation
// ============================================================================

bool CopyPropagation::runOnFunction(IRFunction& func)
{
    m_stats.reset();
    m_copyMap.clear();

    // Step 1: Find all copy relationships
    findCopies(func);

    if (m_copyMap.empty())
    {
        return false;
    }

    // Step 2: Replace uses of copies with original sources
    bool changed = false;
    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            for (auto& op : inst->operands)
            {
                IRValueID root = getRootSource(op);
                if (root != op)
                {
                    op = root;
                    m_stats.copiesPropagated++;
                    changed = true;
                }
            }
        }
    }

    return changed;
}

void CopyPropagation::findCopies(IRFunction& func)
{
    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            // VecShuffle with identity swizzle is a copy
            if (inst->op == IROp::VecShuffle && !inst->operands.empty())
            {
                // Check for identity swizzle (xyzw)
                if (inst->swizzleMask == 0xE4)  // 0b11100100 = xyzw
                {
                    m_copyMap[inst->result] = inst->operands[0];
                }
            }
        }
    }
}

IRValueID CopyPropagation::getRootSource(IRValueID id) const
{
    // Follow the copy chain to find the root
    IRValueID current = id;
    while (true)
    {
        auto it = m_copyMap.find(current);
        if (it == m_copyMap.end())
        {
            return current;
        }
        current = it->second;

        // Safety check to avoid infinite loops
        if (current == id)
        {
            return id;
        }
    }
}

// ============================================================================
// Common Subexpression Elimination
// ============================================================================

size_t CommonSubexprElimination::InstrHash::operator()(const IRInstruction* inst) const
{
    size_t hash = static_cast<size_t>(inst->op);
    for (IRValueID op : inst->operands)
    {
        hash ^= std::hash<IRValueID>()(op) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    hash ^= std::hash<int>()(inst->swizzleMask);
    hash ^= std::hash<int>()(inst->componentIndex) + 0x9e3779b9 + (hash << 6) + (hash >> 2);

    // Include targetName in hash for LoadUniform/Call instructions
    if (!inst->targetName.empty())
    {
        hash ^= std::hash<std::string>()(inst->targetName) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    // Include semanticName in hash for LoadAttribute/StoreOutput instructions
    if (!inst->semanticName.empty())
    {
        hash ^= std::hash<std::string>()(inst->semanticName) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<int>()(inst->semanticIndex);
    }
    return hash;
}

bool CommonSubexprElimination::InstrEqual::operator()(const IRInstruction* a,
                                                       const IRInstruction* b) const
{
    if (a->op != b->op) return false;
    if (a->operands.size() != b->operands.size()) return false;
    if (a->swizzleMask != b->swizzleMask) return false;
    if (a->componentIndex != b->componentIndex) return false;

    // Compare targetName for LoadUniform/Call instructions
    if (a->targetName != b->targetName) return false;
    // Compare semanticName/Index for LoadAttribute/StoreOutput instructions
    if (a->semanticName != b->semanticName) return false;
    if (a->semanticIndex != b->semanticIndex) return false;

    for (size_t i = 0; i < a->operands.size(); ++i)
    {
        if (a->operands[i] != b->operands[i]) return false;
    }

    return true;
}

bool CommonSubexprElimination::runOnFunction(IRFunction& func)
{
    m_stats.reset();
    m_exprMap.clear();

    std::unordered_map<IRValueID, IRValueID> replacements;

    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            // Only consider pure operations
            if (!IRPassUtils::isPure(inst->op) || inst->result == InvalidIRValue)
            {
                continue;
            }

            // Look for an existing equivalent expression
            auto it = m_exprMap.find(inst.get());
            if (it != m_exprMap.end())
            {
                // Found a duplicate - record replacement
                replacements[inst->result] = it->second;
                m_stats.instructionsRemoved++;
            }
            else
            {
                // New expression - record it
                m_exprMap[inst.get()] = inst->result;
            }
        }
    }

    // Apply replacements
    if (!replacements.empty())
    {
        for (auto& block : func.blocks)
        {
            for (auto& inst : block->instructions)
            {
                for (auto& op : inst->operands)
                {
                    auto it = replacements.find(op);
                    if (it != replacements.end())
                    {
                        op = it->second;
                    }
                }
            }
        }
        return true;
    }

    return false;
}

// ============================================================================
// Algebraic Simplification
// ============================================================================

bool AlgebraicSimplification::runOnFunction(IRFunction& func)
{
    m_stats.reset();
    m_replacements.clear();

    bool changed = false;

    for (auto& block : func.blocks)
    {
        for (auto& inst : block->instructions)
        {
            if (trySimplify(func, inst.get()))
            {
                changed = true;
            }
        }
    }

    // Apply replacements
    if (!m_replacements.empty())
    {
        IRPassUtils::replaceAllUses(func, m_replacements.begin()->first,
                                     m_replacements.begin()->second);
    }

    return changed;
}

bool AlgebraicSimplification::trySimplify(IRFunction& func, IRInstruction* inst)
{
    switch (inst->op)
    {
    case IROp::Add: return simplifyAdd(func, inst);
    case IROp::Mul: return simplifyMul(func, inst);
    case IROp::Sub: return simplifySub(func, inst);
    case IROp::Div: return simplifyDiv(func, inst);
    default: return false;
    }
}

bool AlgebraicSimplification::simplifyAdd(IRFunction& func, IRInstruction* inst)
{
    if (inst->operands.size() != 2) return false;

    // x + 0 = x
    if (isZero(func, inst->operands[1]))
    {
        m_replacements[inst->result] = inst->operands[0];
        m_stats.instructionsRemoved++;
        return true;
    }

    // 0 + x = x
    if (isZero(func, inst->operands[0]))
    {
        m_replacements[inst->result] = inst->operands[1];
        m_stats.instructionsRemoved++;
        return true;
    }

    return false;
}

bool AlgebraicSimplification::simplifyMul(IRFunction& func, IRInstruction* inst)
{
    if (inst->operands.size() != 2) return false;

    // x * 1 = x
    if (isOne(func, inst->operands[1]))
    {
        m_replacements[inst->result] = inst->operands[0];
        m_stats.instructionsRemoved++;
        return true;
    }

    // 1 * x = x
    if (isOne(func, inst->operands[0]))
    {
        m_replacements[inst->result] = inst->operands[1];
        m_stats.instructionsRemoved++;
        return true;
    }

    // x * 0 = 0
    if (isZero(func, inst->operands[0]) || isZero(func, inst->operands[1]))
    {
        IRConstant* zero = func.createConstant(inst->resultType, 0.0f);
        m_replacements[inst->result] = zero->id;
        m_stats.instructionsRemoved++;
        return true;
    }

    return false;
}

bool AlgebraicSimplification::simplifySub(IRFunction& func, IRInstruction* inst)
{
    if (inst->operands.size() != 2) return false;

    // x - 0 = x
    if (isZero(func, inst->operands[1]))
    {
        m_replacements[inst->result] = inst->operands[0];
        m_stats.instructionsRemoved++;
        return true;
    }

    // x - x = 0 (same value)
    if (inst->operands[0] == inst->operands[1])
    {
        IRConstant* zero = func.createConstant(inst->resultType, 0.0f);
        m_replacements[inst->result] = zero->id;
        m_stats.instructionsRemoved++;
        return true;
    }

    return false;
}

bool AlgebraicSimplification::simplifyDiv(IRFunction& func, IRInstruction* inst)
{
    if (inst->operands.size() != 2) return false;

    // x / 1 = x
    if (isOne(func, inst->operands[1]))
    {
        m_replacements[inst->result] = inst->operands[0];
        m_stats.instructionsRemoved++;
        return true;
    }

    // x / x = 1 (same value, assume non-zero)
    if (inst->operands[0] == inst->operands[1])
    {
        IRConstant* one = func.createConstant(inst->resultType, 1.0f);
        m_replacements[inst->result] = one->id;
        m_stats.instructionsRemoved++;
        return true;
    }

    return false;
}

bool AlgebraicSimplification::isZero(IRFunction& func, IRValueID id) const
{
    IRValue* val = func.getValue(id);
    if (auto* c = dynamic_cast<const IRConstant*>(val))
    {
        if (std::holds_alternative<float>(c->value))
        {
            return std::get<float>(c->value) == 0.0f;
        }
        if (std::holds_alternative<int32_t>(c->value))
        {
            return std::get<int32_t>(c->value) == 0;
        }
    }
    return false;
}

bool AlgebraicSimplification::isOne(IRFunction& func, IRValueID id) const
{
    IRValue* val = func.getValue(id);
    if (auto* c = dynamic_cast<const IRConstant*>(val))
    {
        if (std::holds_alternative<float>(c->value))
        {
            return std::get<float>(c->value) == 1.0f;
        }
        if (std::holds_alternative<int32_t>(c->value))
        {
            return std::get<int32_t>(c->value) == 1;
        }
    }
    return false;
}

bool AlgebraicSimplification::isNegOne(IRFunction& func, IRValueID id) const
{
    IRValue* val = func.getValue(id);
    if (auto* c = dynamic_cast<const IRConstant*>(val))
    {
        if (std::holds_alternative<float>(c->value))
        {
            return std::get<float>(c->value) == -1.0f;
        }
        if (std::holds_alternative<int32_t>(c->value))
        {
            return std::get<int32_t>(c->value) == -1;
        }
    }
    return false;
}

// ============================================================================
// Dead Block Elimination
// ============================================================================

bool DeadBlockElimination::runOnFunction(IRFunction& func)
{
    m_stats.reset();
    m_reachableBlocks.clear();

    if (func.blocks.empty()) return false;

    // Find all reachable blocks
    findReachableBlocks(func);

    // Remove unreachable blocks
    auto it = func.blocks.begin();
    while (it != func.blocks.end())
    {
        if (m_reachableBlocks.find(it->get()) == m_reachableBlocks.end())
        {
            it = func.blocks.erase(it);
            m_stats.blocksRemoved++;
        }
        else
        {
            ++it;
        }
    }

    return m_stats.blocksRemoved > 0;
}

void DeadBlockElimination::findReachableBlocks(IRFunction& func)
{
    if (func.blocks.empty()) return;

    std::vector<IRBasicBlock*> worklist;
    worklist.push_back(func.blocks[0].get());
    m_reachableBlocks.insert(func.blocks[0].get());

    while (!worklist.empty())
    {
        IRBasicBlock* block = worklist.back();
        worklist.pop_back();

        for (IRBasicBlock* succ : block->successors)
        {
            if (m_reachableBlocks.find(succ) == m_reachableBlocks.end())
            {
                m_reachableBlocks.insert(succ);
                worklist.push_back(succ);
            }
        }
    }
}

// ============================================================================
// Instruction Combining
// ============================================================================

bool InstructionCombining::runOnFunction(IRFunction& func)
{
    m_stats.reset();
    bool changed = false;

    for (auto& block : func.blocks)
    {
        if (tryCombineMulAdd(func, block.get()))
        {
            changed = true;
        }
    }

    return changed;
}

bool InstructionCombining::tryCombineMulAdd(IRFunction& func, IRBasicBlock* block)
{
    // Look for patterns: add(mul(a, b), c) -> mad(a, b, c)
    // Also matches: add(c, mul(a, b)) -> mad(a, b, c)
    bool changed = false;

    for (auto it = block->instructions.begin(); it != block->instructions.end(); ++it)
    {
        IRInstruction* addInst = it->get();

        // Look for Add instructions
        if (addInst->op != IROp::Add || addInst->operands.size() != 2)
            continue;

        IRValueID addOp0 = addInst->operands[0];
        IRValueID addOp1 = addInst->operands[1];

        // Try to find a Mul instruction that feeds into this Add
        IRInstruction* mulInst = nullptr;
        IRValueID addend = InvalidIRValue;

        // Check if first operand is a Mul
        IRInstruction* def0 = IRPassUtils::getDefinition(func, addOp0);
        if (def0 && def0->op == IROp::Mul && def0->operands.size() == 2)
        {
            // Check if the Mul result is only used by this Add
            if (IRPassUtils::countUses(func, def0->result) == 1)
            {
                mulInst = def0;
                addend = addOp1;
            }
        }

        // If not found, check if second operand is a Mul
        if (!mulInst)
        {
            IRInstruction* def1 = IRPassUtils::getDefinition(func, addOp1);
            if (def1 && def1->op == IROp::Mul && def1->operands.size() == 2)
            {
                // Check if the Mul result is only used by this Add
                if (IRPassUtils::countUses(func, def1->result) == 1)
                {
                    mulInst = def1;
                    addend = addOp0;
                }
            }
        }

        // If we found a fusable pattern, create MAD
        if (mulInst && addend != InvalidIRValue)
        {
            // Transform: add(mul(a, b), c) -> mad(a, b, c)
            // Modify the Add instruction in-place to become a Mad
            addInst->op = IROp::Mad;
            addInst->operands.clear();
            addInst->operands.push_back(mulInst->operands[0]);  // a
            addInst->operands.push_back(mulInst->operands[1]);  // b
            addInst->operands.push_back(addend);                // c

            // The Mul instruction will be removed by DCE since it's no longer used
            m_stats.instructionsRemoved++;
            changed = true;
        }
    }

    return changed;
}

bool InstructionCombining::tryCombineNormalize(IRFunction& func, IRBasicBlock* block)
{
    // Look for patterns: mul(v, rsqrt(dot(v, v))) -> normalize(v)
    return false;
}

// ============================================================================
// Pass Manager
// ============================================================================

PassManager::PassManager(OptimizationLevel level)
{
    configurePasses(level);
}

void PassManager::configurePasses(OptimizationLevel level)
{
    m_passes.clear();

    if (level == OptimizationLevel::O0)
    {
        // No optimization
        return;
    }

    // O1: Basic optimizations
    m_passes.push_back(std::make_unique<DeadCodeElimination>());
    m_passes.push_back(std::make_unique<ConstantFolding>());
    m_passes.push_back(std::make_unique<CopyPropagation>());

    if (level >= OptimizationLevel::O2)
    {
        // O2: Add CSE and algebraic simplification
        m_passes.push_back(std::make_unique<CommonSubexprElimination>());
        m_passes.push_back(std::make_unique<AlgebraicSimplification>());
        m_passes.push_back(std::make_unique<DeadBlockElimination>());
    }

    if (level >= OptimizationLevel::O3)
    {
        // O3: Add instruction combining
        m_passes.push_back(std::make_unique<InstructionCombining>());
        // Run DCE again after combining
        m_passes.push_back(std::make_unique<DeadCodeElimination>());
    }

    if (level >= OptimizationLevel::O4)
    {
        // O4: Multiple iterations
        m_maxIterations = 8;
    }
}

void PassManager::addPass(std::unique_ptr<IRPass> pass)
{
    m_passes.push_back(std::move(pass));
}

bool PassManager::runOnModule(IRModule& module)
{
    bool changed = false;

    for (auto& func : module.functions)
    {
        // Only optimize the entry point function to avoid breaking builtins
        if (!func->isEntryPoint)
        {
            continue;
        }

        if (runOnFunction(*func))
        {
            changed = true;
        }
    }

    return changed;
}

bool PassManager::runOnFunction(IRFunction& func)
{
    bool changed = false;

    for (int iter = 0; iter < m_maxIterations; ++iter)
    {
        bool iterChanged = false;

        for (auto& pass : m_passes)
        {
            if (m_verbose)
            {
                std::cout << "Running pass: " << pass->getName() << std::endl;
            }

            if (pass->runOnFunction(func))
            {
                iterChanged = true;
                const auto& stats = pass->getStats();
                m_totalStats.instructionsRemoved += stats.instructionsRemoved;
                m_totalStats.constantsFolded += stats.constantsFolded;
                m_totalStats.copiesPropagated += stats.copiesPropagated;
                m_totalStats.blocksRemoved += stats.blocksRemoved;

                if (m_verbose)
                {
                    std::cout << "  Removed: " << stats.instructionsRemoved
                              << " instructions" << std::endl;
                }
            }
        }

        if (iterChanged)
        {
            changed = true;
        }
        else
        {
            // No changes in this iteration - converged
            break;
        }
    }

    return changed;
}
