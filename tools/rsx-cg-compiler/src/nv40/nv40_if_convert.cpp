/*
 * If-conversion pass — see nv40_if_convert.h for the full rewrite
 * contract.  Collapses simple if-else diamonds and "if-only" shapes
 * into an `IROp::Select` + `IROp::StoreOutput` pair so the existing
 * Select emit path handles them without a dedicated IF/ELSE/ENDIF
 * branch encoder.
 */

#include "nv40_if_convert.h"

#include "ir.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace nv40
{

namespace
{

// Build a map of IRValueID → IRTypeInfo covering every value the
// function can reference — parameters, stored values (constants), and
// instruction results across every block.  Used to look up the type
// of an operand so we can stamp the right resultType on a synthesised
// Select.  Walking the full function once is cheap at this scope.
std::unordered_map<IRValueID, IRTypeInfo> buildValueTypeMap(IRFunction& fn)
{
    std::unordered_map<IRValueID, IRTypeInfo> types;
    for (const auto& p : fn.parameters)
        types[p.valueId] = p.type;
    for (const auto& kv : fn.values)
    {
        if (kv.second) types[kv.first] = kv.second->type;
    }
    for (const auto& bp : fn.blocks)
    {
        if (!bp) continue;
        for (const auto& ip : bp->instructions)
        {
            if (!ip) continue;
            if (ip->result != InvalidIRValue)
                types[ip->result] = ip->resultType;
        }
    }
    return types;
}

// Find a block by name, linear scan (block lists are small).
IRBasicBlock* findBlock(IRFunction& fn, const std::string& name)
{
    for (auto& bp : fn.blocks)
    {
        if (bp && bp->name == name) return bp.get();
    }
    return nullptr;
}

// A block has exactly one non-terminator StoreOutput and then an
// unconditional Branch to `expectedJoin`.  Returns the StoreOutput
// instruction pointer iff the shape matches; otherwise nullptr.
IRInstruction* matchSingleStoutThenBranch(IRBasicBlock* block,
                                          IRBasicBlock* expectedJoin)
{
    if (!block) return nullptr;
    if (block->instructions.size() != 2) return nullptr;

    IRInstruction* stout = block->instructions[0].get();
    IRInstruction* term  = block->instructions[1].get();
    if (!stout || !term) return nullptr;
    if (stout->op != IROp::StoreOutput) return nullptr;
    if (stout->operands.size() != 1) return nullptr;
    if (term->op != IROp::Branch) return nullptr;

    // CFG successor check — the Branch must point at the expected join.
    if (block->successors.size() != 1) return nullptr;
    if (block->successors[0] != expectedJoin) return nullptr;

    return stout;
}

// Is this block a pure terminator that only returns?  (Our join blocks
// are typically `ret` after if-else collapses.)
bool isReturnOnly(IRBasicBlock* block)
{
    if (!block) return false;
    if (block->instructions.size() != 1) return false;
    IRInstruction* term = block->instructions[0].get();
    return term && term->op == IROp::Return;
}

// Two StoreOutputs target the same shader semantic iff name and
// index match.  We compare names case-insensitively since downstream
// consumers uppercase them.
bool sameSemantic(const IRInstruction* a, const IRInstruction* b)
{
    if (!a || !b) return false;
    if (a->semanticIndex != b->semanticIndex) return false;
    if (a->semanticName.size() != b->semanticName.size()) return false;
    for (size_t i = 0; i < a->semanticName.size(); ++i)
    {
        char ca = a->semanticName[i];
        char cb = b->semanticName[i];
        if (ca >= 'a' && ca <= 'z') ca = char(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = char(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return true;
}

// Remove a block from the function's blocks vector.  Linear, but
// block lists are small in practice.
void eraseBlock(IRFunction& fn, IRBasicBlock* block)
{
    fn.blocks.erase(std::remove_if(fn.blocks.begin(), fn.blocks.end(),
        [&](const std::unique_ptr<IRBasicBlock>& bp) {
            return bp.get() == block;
        }), fn.blocks.end());
}

// Pick the IRTypeInfo for an operand from the precomputed map,
// defaulting to the fallback when the operand isn't tracked (should
// not happen for well-formed IR, but keep the pass defensive).
IRTypeInfo lookupType(const std::unordered_map<IRValueID, IRTypeInfo>& types,
                      IRValueID id,
                      const IRTypeInfo& fallback)
{
    auto it = types.find(id);
    return (it != types.end()) ? it->second : fallback;
}

// Attempt if-conversion on a single block's CondBranch terminator.
// Returns true iff the diamond was rewritten.
//
// Two patterns handled (see header):
//   1. Full diamond  — entry's last-non-terminator is the cmp; terminator
//                       branches to two blocks that each do a single
//                       stout-to-same-semantic and merge at a return.
//   2. If-only      — entry has an early StoreOutput (the default) BEFORE
//                       the compare, then branches to a then-block that
//                       does a single stout-to-same-semantic and falls
//                       through to the return block.
bool tryConvertBlock(IRFunction& fn,
                     const std::unordered_map<IRValueID, IRTypeInfo>& types,
                     IRBasicBlock* entry)
{
    if (!entry) return false;
    if (entry->instructions.empty()) return false;

    IRInstruction* term = entry->instructions.back().get();
    if (!term || term->op != IROp::CondBranch) return false;
    if (term->operands.empty()) return false;
    if (entry->successors.size() != 2) return false;

    IRValueID     condId      = term->operands[0];
    IRBasicBlock* thenBlock   = entry->successors[0];
    IRBasicBlock* falseTarget = entry->successors[1];

    // Shape 1: full if-else diamond.
    //   then / else each have a single stout + br -> join,
    //   join is return-only.
    IRBasicBlock* joinBlock = nullptr;
    if (thenBlock && thenBlock->successors.size() == 1 &&
        falseTarget && falseTarget->successors.size() == 1 &&
        thenBlock->successors[0] == falseTarget->successors[0])
    {
        joinBlock = thenBlock->successors[0];
    }

    if (joinBlock && isReturnOnly(joinBlock))
    {
        IRInstruction* thenStout = matchSingleStoutThenBranch(thenBlock,  joinBlock);
        IRInstruction* elseStout = matchSingleStoutThenBranch(falseTarget, joinBlock);
        if (thenStout && elseStout && sameSemantic(thenStout, elseStout))
        {
            IRValueID trueVal  = thenStout->operands[0];
            IRValueID falseVal = elseStout->operands[0];

            // Pop the CondBranch (last instruction of entry).
            entry->instructions.pop_back();

            // %sel = select %cond ? %trueVal : %falseVal — pick the
            // result type from the value map, not the StoreOutput
            // (which is always Void).
            //
            // `componentIndex = 1` marks the Select as originating
            // from an if-else statement (as opposed to a ternary).
            // sce-cgc schedules the two differently: ternary preloads
            // the true branch and conditional-writes the false with
            // EQ; if-else preloads the false branch and conditional-
            // writes the true with NE.  The NV40 FP emit path reads
            // this flag into SelectBinding::ifElseSchedule.
            IRValueID selId = fn.allocateValueId();
            IRTypeInfo selType = lookupType(types, trueVal, IRTypeInfo::Void());
            auto sel = std::make_unique<IRInstruction>(
                IROp::Select, selId, selType);
            sel->addOperand(condId);
            sel->addOperand(trueVal);
            sel->addOperand(falseVal);
            sel->componentIndex = 1;  // if-else schedule
            entry->addInstruction(std::move(sel));

            // stout %sel SEM  (copy the semantic off the then-side stout)
            auto stout = std::make_unique<IRInstruction>(
                IROp::StoreOutput, InvalidIRValue, IRTypeInfo::Void());
            stout->addOperand(selId);
            stout->semanticName    = thenStout->semanticName;
            stout->rawSemanticName = thenStout->rawSemanticName;
            stout->semanticIndex   = thenStout->semanticIndex;
            stout->fieldName       = thenStout->fieldName;
            entry->addInstruction(std::move(stout));

            // ret
            auto ret = std::make_unique<IRInstruction>(
                IROp::Return, InvalidIRValue, IRTypeInfo::Void());
            entry->addInstruction(std::move(ret));

            // Drop the diamond blocks (CFG becomes entry-only).
            entry->successors.clear();
            eraseBlock(fn, thenBlock);
            eraseBlock(fn, falseTarget);
            eraseBlock(fn, joinBlock);
            return true;
        }
    }

    // Shape 2: if-only — `falseTarget` is the return-only join block,
    // `thenBlock` overrides.  The default value comes from an earlier
    // StoreOutput in the entry block itself.
    if (isReturnOnly(falseTarget))
    {
        IRInstruction* thenStout = matchSingleStoutThenBranch(thenBlock, falseTarget);
        if (!thenStout) return false;

        // Find the most recent StoreOutput in entry matching the same
        // semantic.  That's the "default" value.
        IRInstruction* defaultStout = nullptr;
        size_t         defaultIdx   = 0;
        // Don't consider the CondBranch itself (last instruction).
        for (size_t i = 0; i + 1 < entry->instructions.size(); ++i)
        {
            IRInstruction* p = entry->instructions[i].get();
            if (p && p->op == IROp::StoreOutput && sameSemantic(p, thenStout))
            {
                defaultStout = p;
                defaultIdx   = i;
            }
        }
        if (!defaultStout) return false;
        if (defaultStout->operands.empty()) return false;

        IRValueID trueVal    = thenStout->operands[0];
        IRValueID defaultVal = defaultStout->operands[0];

        // Erase the default stout and the CondBranch from entry (in
        // reverse order so the earlier index stays valid).
        entry->instructions.pop_back();                     // CondBranch
        entry->instructions.erase(entry->instructions.begin() + defaultIdx);

        // %sel = select %cond ? %trueVal : %defaultVal  (if-only style)
        //
        // Shape 2 shares sce-cgc's "ternary" schedule rather than the
        // if-else one: the unconditional preload lands on `trueVal`
        // (the explicitly-written then branch) and the conditional
        // EQ write restores `defaultVal` when the condition was false.
        // Confirmed against `if (x) { color = a; }` with uniform
        // defaults — sce-cgc emits MOVR R0, a; MOVR R0(EQ.x), default.
        IRValueID selId = fn.allocateValueId();
        IRTypeInfo selType = lookupType(types, trueVal, IRTypeInfo::Void());
        auto sel = std::make_unique<IRInstruction>(
            IROp::Select, selId, selType);
        sel->addOperand(condId);
        sel->addOperand(trueVal);
        sel->addOperand(defaultVal);
        // leave componentIndex = 0 — use ternary schedule
        entry->addInstruction(std::move(sel));

        // stout %sel SEM
        auto stout = std::make_unique<IRInstruction>(
            IROp::StoreOutput, InvalidIRValue, IRTypeInfo::Void());
        stout->addOperand(selId);
        stout->semanticName    = thenStout->semanticName;
        stout->rawSemanticName = thenStout->rawSemanticName;
        stout->semanticIndex   = thenStout->semanticIndex;
        stout->fieldName       = thenStout->fieldName;
        entry->addInstruction(std::move(stout));

        // ret
        auto ret = std::make_unique<IRInstruction>(
            IROp::Return, InvalidIRValue, IRTypeInfo::Void());
        entry->addInstruction(std::move(ret));

        entry->successors.clear();
        eraseBlock(fn, thenBlock);
        eraseBlock(fn, falseTarget);
        return true;
    }

    return false;
}

}  // namespace

int convertSimpleIfElse(IRModule& module)
{
    int totalConverted = 0;

    for (auto& fn : module.functions)
    {
        if (!fn) continue;
        if (!fn->isEntryPoint) continue;

        // Keep trying until no more diamonds are found.  Nested
        // shapes converge in a handful of iterations.  Rebuild the
        // value→type map on each outer pass since new Select values
        // get synthesised as diamonds collapse.
        bool changed = true;
        while (changed)
        {
            changed = false;
            auto types = buildValueTypeMap(*fn);

            // Snapshot the blocks vector — tryConvertBlock mutates it.
            std::vector<IRBasicBlock*> snapshot;
            snapshot.reserve(fn->blocks.size());
            for (auto& bp : fn->blocks) snapshot.push_back(bp.get());

            for (IRBasicBlock* b : snapshot)
            {
                if (tryConvertBlock(*fn, types, b))
                {
                    ++totalConverted;
                    changed = true;
                    break;  // Restart with a fresh snapshot.
                }
            }
        }
    }

    return totalConverted;
}

}  // namespace nv40
