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

// True for IR ops that are pure (no side effects, no memory / I/O)
// and therefore safe to hoist out of a conditional arm into the
// preceding entry block: they will run unconditionally instead of
// only when the predicate fires, but their result is the same in
// either case.  Restricted set today — extend as new shapes need it.
bool isHoistableOp(IROp op)
{
    switch (op)
    {
    case IROp::Const:
    case IROp::VecConstruct:
    case IROp::VecExtract:
    case IROp::VecInsert:
    case IROp::VecShuffle:
        return true;
    default:
        return false;
    }
}

// Broader pure-op predicate for whole-block hoisting (Shape 4): any op
// that has no side effect and produces a deterministic result given
// its operands.  Excludes control flow, stores, and discard — these
// can't be hoisted out of a conditional arm without changing program
// semantics.  Calls are conservatively excluded today; once the IR
// inliner runs before this pass they should be flattened away.
bool isPureOp(IROp op)
{
    switch (op)
    {
    case IROp::Branch:
    case IROp::CondBranch:
    case IROp::Return:
    case IROp::Discard:
    case IROp::Store:
    case IROp::StoreOutput:
    case IROp::StoreVarying:
    case IROp::Call:
    case IROp::Phi:
        return false;
    default:
        return true;
    }
}

// A block has up to N hoistable instructions, then a single
// non-terminator StoreOutput, then an unconditional Branch to
// `expectedJoin`.  When the shape matches, returns the StoreOutput
// pointer and pushes any pre-stout hoistables (in order) into
// `outHoistable`; otherwise returns nullptr.  The 2-instruction
// case (stout + br only, no hoistables) is the original
// matchSingleStoutThenBranch shape preserved verbatim.
IRInstruction* matchSingleStoutThenBranch(IRBasicBlock* block,
                                          IRBasicBlock* expectedJoin,
                                          std::vector<IRInstruction*>* outHoistable = nullptr)
{
    if (!block) return nullptr;
    if (block->instructions.size() < 2) return nullptr;

    // Last instruction must be the unconditional Branch to the join.
    IRInstruction* term = block->instructions.back().get();
    if (!term || term->op != IROp::Branch) return nullptr;
    if (block->successors.size() != 1) return nullptr;
    if (block->successors[0] != expectedJoin) return nullptr;

    // Second-to-last must be the StoreOutput.
    const size_t stoutIdx = block->instructions.size() - 2;
    IRInstruction* stout  = block->instructions[stoutIdx].get();
    if (!stout || stout->op != IROp::StoreOutput) return nullptr;
    if (stout->operands.size() != 1) return nullptr;

    // Anything between block start and the stout must be hoistable.
    for (size_t i = 0; i < stoutIdx; ++i)
    {
        IRInstruction* p = block->instructions[i].get();
        if (!p || !isHoistableOp(p->op)) return nullptr;
    }

    if (outHoistable)
    {
        outHoistable->clear();
        for (size_t i = 0; i < stoutIdx; ++i)
            outHoistable->push_back(block->instructions[i].get());
    }
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

// Inner ops sce-cgc allows inside a multi-insn predicated THEN chain
// today.  Anything else triggers a clean fail-out so we keep the
// existing single-stout fast paths byte-exact.
bool isPredicatableInnerOp(IROp op)
{
    switch (op)
    {
    case IROp::Add:
    case IROp::Sub:
    case IROp::Mul:
    case IROp::Mad:
        return true;
    default:
        return false;
    }
}

// Attempt if-conversion on a single block's CondBranch terminator.
// Returns true iff the diamond was rewritten.
//
// Three patterns handled (see header):
//   1. Full diamond  — entry's last-non-terminator is the cmp; terminator
//                       branches to two blocks that each do a single
//                       stout-to-same-semantic and merge at a return.
//   2. If-only      — entry has an early StoreOutput (the default) BEFORE
//                       the compare, then branches to a then-block that
//                       does a single stout-to-same-semantic and falls
//                       through to the return block.
//   3. If-only multi — same as (2), but the then-block contains N>1
//                       (compute, stout) pairs forming a running-update
//                       chain.  Lowers to a chain of PredCarry ops.
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
        std::vector<IRInstruction*> thenHoistable, elseHoistable;
        IRInstruction* thenStout =
            matchSingleStoutThenBranch(thenBlock,  joinBlock, &thenHoistable);
        IRInstruction* elseStout =
            matchSingleStoutThenBranch(falseTarget, joinBlock, &elseHoistable);
        if (thenStout && elseStout && sameSemantic(thenStout, elseStout))
        {
            IRValueID trueVal  = thenStout->operands[0];
            IRValueID falseVal = elseStout->operands[0];

            // Pop the CondBranch (last instruction of entry).
            entry->instructions.pop_back();

            // Hoist any pre-stout pure ops from the THEN/ELSE arms
            // into entry before synthesising the Select.  These ran
            // conditionally inside the arm; running them
            // unconditionally is safe because isHoistableOp restricts
            // the set to side-effect-free instructions.  Steal the
            // unique_ptr from each arm's vector so the IRInstruction
            // owners get re-rooted under entry.
            auto stealPreStout = [](IRBasicBlock* arm,
                                    IRBasicBlock* dst,
                                    size_t hoistableCount)
            {
                for (size_t i = 0; i < hoistableCount; ++i)
                {
                    dst->addInstruction(std::move(arm->instructions[i]));
                }
                arm->instructions.erase(
                    arm->instructions.begin(),
                    arm->instructions.begin() +
                        static_cast<std::ptrdiff_t>(hoistableCount));
            };
            stealPreStout(thenBlock,   entry, thenHoistable.size());
            stealPreStout(falseTarget, entry, elseHoistable.size());

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
    // StoreOutput in the entry block itself.  Falls through to Shape 3
    // when the then-block has more than one StoreOutput.
    IRInstruction* thenStout =
        isReturnOnly(falseTarget)
            ? matchSingleStoutThenBranch(thenBlock, falseTarget)
            : nullptr;
    if (thenStout)
    {

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

    // Shape 3: if-only with multi-instruction THEN.  Same outer
    // shape as Shape 2 (entry has explicit default stout, ELSE
    // target is return-only) but the then-block has N >= 1
    // (compute, stout) pairs forming a running-update chain on
    // the same semantic.  Each pair becomes a PredCarry; the
    // FP emitter lowers the chain to alternating R-temp predicated
    // writes (see header for the full picture).
    if (isReturnOnly(falseTarget) && thenBlock)
    {
        // The then-block must end with `br -> falseTarget` after
        // K pairs of (computational, StoreOutput).  Each StoreOutput
        // must operate on the same semantic and consume its
        // immediately-preceding computational instruction's result.
        if (thenBlock->successors.size() != 1) return false;
        if (thenBlock->successors[0] != falseTarget) return false;
        if (thenBlock->instructions.size() < 3)      return false;  // need >= 1 pair + Branch
        if ((thenBlock->instructions.size() % 2) != 1) return false; // pairs + 1 terminator

        // Last instruction must be Branch.
        IRInstruction* term = thenBlock->instructions.back().get();
        if (!term || term->op != IROp::Branch) return false;

        struct ChainLink {
            IRInstruction* compute;
            IRInstruction* stout;
        };
        std::vector<ChainLink> chain;
        const size_t pairCount = (thenBlock->instructions.size() - 1) / 2;
        chain.reserve(pairCount);
        for (size_t i = 0; i < pairCount; ++i)
        {
            IRInstruction* compute = thenBlock->instructions[2 * i].get();
            IRInstruction* stout   = thenBlock->instructions[2 * i + 1].get();
            if (!compute || !stout) return false;
            if (!isPredicatableInnerOp(compute->op)) return false;
            if (compute->result == InvalidIRValue)   return false;
            if (stout->op != IROp::StoreOutput)      return false;
            if (stout->operands.size() != 1)         return false;
            if (stout->operands[0] != compute->result) return false;
            chain.push_back({compute, stout});
        }
        if (chain.empty()) return false;

        // Reject if any chain compute references an SSA value
        // produced inside this then-block other than the
        // immediately-prior link's compute (would need full
        // dependency resolution we don't do yet).  For chain[i],
        // any operand that isn't the prior compute's result, the
        // default value, or a value defined OUTSIDE the then-block
        // is unsupported today.
        std::unordered_map<IRValueID, IRValueID> innerProducer;
        for (const auto& link : chain)
            innerProducer[link.compute->result] = link.compute->result;

        for (size_t i = 0; i < chain.size(); ++i)
        {
            for (IRValueID operand : chain[i].compute->operands)
            {
                if (innerProducer.count(operand) == 0) continue;
                // operand was produced inside the then-block.
                // Allowed only if it's the immediately-prior link.
                if (i == 0) return false;
                if (operand != chain[i - 1].compute->result) return false;
            }
        }

        // Every link's StoreOutput must hit the same semantic.
        for (size_t i = 1; i < chain.size(); ++i)
            if (!sameSemantic(chain[i].stout, chain[0].stout)) return false;

        // Find the entry's default stout for the same semantic.
        IRInstruction* defaultStout = nullptr;
        size_t         defaultIdx   = 0;
        for (size_t i = 0; i + 1 < entry->instructions.size(); ++i)
        {
            IRInstruction* p = entry->instructions[i].get();
            if (p && p->op == IROp::StoreOutput && sameSemantic(p, chain[0].stout))
            {
                defaultStout = p;
                defaultIdx   = i;
            }
        }
        if (!defaultStout) return false;
        if (defaultStout->operands.empty()) return false;

        IRValueID defaultVal = defaultStout->operands[0];

        // Erase the entry default stout + the CondBranch.
        entry->instructions.pop_back();                     // CondBranch
        entry->instructions.erase(entry->instructions.begin() + defaultIdx);

        // Build the PredCarry chain.  For each link: predOp = the
        // original compute op, operand[0] = cond, operand[1] =
        // running value (defaultVal for the first link, prior
        // PredCarry's result thereafter), operand[2..] = the
        // original compute's operands with prior-link result IDs
        // remapped to the corresponding PredCarry IDs.
        std::unordered_map<IRValueID, IRValueID> remap;
        IRValueID running = defaultVal;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const IRInstruction* compute = chain[i].compute;
            IRValueID predId = fn.allocateValueId();
            auto pred = std::make_unique<IRInstruction>(
                IROp::PredCarry, predId, compute->resultType);
            pred->predOp = compute->op;
            pred->addOperand(condId);
            pred->addOperand(running);
            for (IRValueID o : compute->operands)
            {
                auto it = remap.find(o);
                pred->addOperand(it != remap.end() ? it->second : o);
            }
            entry->addInstruction(std::move(pred));
            remap[compute->result] = predId;
            running = predId;
        }

        // Final stout %running SEM (carries the last PredCarry's value).
        const IRInstruction* lastStout = chain.back().stout;
        auto stout = std::make_unique<IRInstruction>(
            IROp::StoreOutput, InvalidIRValue, IRTypeInfo::Void());
        stout->addOperand(running);
        stout->semanticName    = lastStout->semanticName;
        stout->rawSemanticName = lastStout->rawSemanticName;
        stout->semanticIndex   = lastStout->semanticIndex;
        stout->fieldName       = lastStout->fieldName;
        entry->addInstruction(std::move(stout));

        auto ret = std::make_unique<IRInstruction>(
            IROp::Return, InvalidIRValue, IRTypeInfo::Void());
        entry->addInstruction(std::move(ret));

        entry->successors.clear();
        eraseBlock(fn, thenBlock);
        eraseBlock(fn, falseTarget);
        return true;
    }

    // Shape 4: if-only with no per-branch StoreOutput.  The IR builder
    // synthesises a Select(cond, thenVal, preVal) at the merge block
    // for any variable redefined inside THEN; the merge block then
    // carries the StoreOutput / Return.  This shape covers the
    // uniform-conditional pattern from older SDK Water samples:
    //   if (gFunctionSwitch != 0.0) { c = c * tex2D(...); }
    //   c.a = 1.0;  return c;
    //
    // Transformation:
    //   - THEN must contain only pure ops (isPureOp) terminated by an
    //     unconditional Branch to the merge block.
    //   - falseTarget must equal that same merge block (i.e. ELSE is
    //     "fall through to merge").
    //   - Hoist all of THEN's instructions (except the trailing Branch)
    //     into entry just before its CondBranch.
    //   - Drop the CondBranch and the THEN block entirely; entry now
    //     branches unconditionally to merge.
    //   - Inline merge into entry (single-predecessor merge after the
    //     drop), preserving its instructions verbatim.
    if (thenBlock && falseTarget &&
        thenBlock->successors.size() == 1 &&
        thenBlock->successors[0] == falseTarget)
    {
        IRBasicBlock* merge = falseTarget;

        // THEN must end with unconditional Branch to merge, and every
        // preceding instruction must be pure.
        if (thenBlock->instructions.empty()) return false;
        IRInstruction* thenTerm = thenBlock->instructions.back().get();
        if (!thenTerm || thenTerm->op != IROp::Branch) return false;
        for (size_t i = 0; i + 1 < thenBlock->instructions.size(); ++i)
        {
            IRInstruction* p = thenBlock->instructions[i].get();
            if (!p || !isPureOp(p->op)) return false;
        }

        // Merge must have exactly two predecessors (entry-via-falseTarget
        // and thenBlock-via-Branch).  After the drop, merge's only
        // predecessor is entry — safe to inline.
        if (merge->predecessors.size() != 2) return false;

        // Hoist THEN's pre-terminator instructions into entry, just
        // before the CondBranch.  Steal ownership of each unique_ptr.
        const size_t condBranchIdx = entry->instructions.size() - 1;
        std::vector<std::unique_ptr<IRInstruction>> hoist;
        hoist.reserve(thenBlock->instructions.size() - 1);
        for (size_t i = 0; i + 1 < thenBlock->instructions.size(); ++i)
            hoist.push_back(std::move(thenBlock->instructions[i]));
        entry->instructions.insert(
            entry->instructions.begin() +
                static_cast<std::ptrdiff_t>(condBranchIdx),
            std::make_move_iterator(hoist.begin()),
            std::make_move_iterator(hoist.end()));

        // Pop the CondBranch.  Entry's terminator is gone for now;
        // merge's instructions will provide the new terminator.
        entry->instructions.pop_back();

        // Inline merge's instructions into entry (skip duplicates of
        // any synthetic Select that already happen to live at the
        // start of merge — they reference `condValue` which is still
        // live in entry).
        for (auto& mp : merge->instructions)
            entry->addInstruction(std::move(mp));

        // CFG cleanup: entry no longer has successors (the merge ended
        // in Return).  Drop the THEN and merge blocks.
        entry->successors.clear();
        eraseBlock(fn, thenBlock);
        eraseBlock(fn, merge);
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
