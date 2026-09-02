#include "nv40_discard_guards.h"

#include "../donor/ir/ir.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nv40
{
namespace
{

// GuardLiteral is declared in the header: the DEFAULT path asks the same
// question with the read-only query at the bottom of this file, and one
// rule with one implementation is the point.
inline bool sameLiteral(const GuardLiteral& a, const GuardLiteral& b)
{
    return a.cond == b.cond && a.taken == b.taken;
}

using GuardSet = std::vector<GuardLiteral>;

bool contains(const GuardSet& s, const GuardLiteral& l)
{
    for (const GuardLiteral& x : s)
        if (sameLiteral(x, l)) return true;
    return false;
}

// Intersection that PRESERVES the order of the first operand, so the
// conjunction we materialise reads in the order the branches were taken
// rather than in whatever order a hash container produced.
GuardSet intersect(const GuardSet& a, const GuardSet& b)
{
    GuardSet out;
    for (const GuardLiteral& l : a)
        if (contains(b, l))
            out.push_back(l);
    return out;
}

class FunctionPass
{
public:
    FunctionPass(IRFunction& fn, DiscardGuardResult& result)
        : fn_(fn), result_(result) {}

    void run()
    {
        if (!hasDiscard())
            return;
        if (fn_.blocks.size() <= 1) {
            // Straight-line: every discard is unconditional.  (A single
            // block cannot carry a brc, so there is nothing to collect.)
            for (auto& b : fn_.blocks)
                for (auto& instPtr : b->instructions)
                    if (instPtr && instPtr->op == IROp::Discard)
                        ++result_.unconditional;
            return;
        }
        if (!closeFallthroughs()) return;
        if (!buildEdges()) return;
        if (!orderBlocks()) return;
        if (!computeGuards()) return;
        materialise();
    }

private:
    IRFunction& fn_;
    DiscardGuardResult& result_;
    std::vector<IRBasicBlock*> blocks_;
    std::unordered_map<const IRBasicBlock*, std::vector<IRBasicBlock*>> succs_;
    std::unordered_map<const IRBasicBlock*, std::vector<IRBasicBlock*>> preds_;
    // The brc that owns each edge, so the merge check can require the two
    // cancelled literals to come from the SAME branch and not merely to be
    // each other's complement by coincidence.
    std::unordered_map<const IRBasicBlock*,
                       std::unordered_map<const IRBasicBlock*, GuardLiteral>>
        edgeLiteral_;
    std::unordered_map<const IRBasicBlock*, bool> hasEdgeLiteral_;
    std::vector<IRBasicBlock*> order_;
    std::unordered_map<const IRBasicBlock*, GuardSet> guard_;
    std::unordered_map<IRValueID, const IRInstruction*> defMap_;

    bool refuse(const std::string& what)
    {
        result_.ok = false;
        result_.diagnostics.push_back("nv40-discard-guards: " + what);
        return false;
    }

    bool hasDiscard() const
    {
        for (const auto& b : fn_.blocks) {
            if (!b) continue;
            for (const auto& instPtr : b->instructions)
                if (instPtr && instPtr->op == IROp::Discard)
                    return true;
        }
        return false;
    }

    // A block whose last instruction is a `discard` has NO terminator:
    // `isTerminator(Discard)` is true, so the frontend's buildIfStmt sees
    // the arm as already terminated and emits no branch to the merge.
    // That is a lie about the hardware - the KIL is not a terminator and
    // the work after a discard runs, which is exactly what th06_notex
    // proved (t_72810bd7) - and it leaves the block without the edge the
    // flatten needs.  Close it with the branch the frontend elided.
    //
    // The target is the NEXT block in creation order.  buildIfStmt makes
    // then / else / merge in that order and appends them in that order,
    // so the block after an arm is that arm's merge; if_convert's shape 5
    // has relied on the same fact ("join must be the next block in the
    // function") since it was written.  A wrong target here CANNOT
    // corrupt a kill - the guard is attached from the branch conditions,
    // not from position - it can only mis-order blocks, and CF-1a's
    // back-edge, single-return and exit-is-the-sink checks refuse that
    // rather than emitting it.
    bool closeFallthroughs()
    {
        for (size_t i = 0; i < fn_.blocks.size(); ++i) {
            IRBasicBlock* b = fn_.blocks[i].get();
            if (!b || b->instructions.empty()) continue;
            const IRInstruction* last = b->instructions.back().get();
            if (!last || last->op != IROp::Discard) continue;
            if (i + 1 >= fn_.blocks.size())
                return refuse("block '" + b->name +
                              "' ends in a discard and has no block to "
                              "fall through to");
            IRBasicBlock* next = fn_.blocks[i + 1].get();
            if (!next)
                return refuse("block '" + b->name +
                              "' falls through to a null block");
            auto br = std::make_unique<IRInstruction>(
                IROp::Branch, InvalidIRValue, IRTypeInfo::Void());
            br->targetName = next->name;
            br->loc = last->loc;
            b->addInstruction(std::move(br));
        }
        return true;
    }

    // Successors come from the branch INSTRUCTIONS, never from
    // IRBasicBlock::successors: the instruction stream is what gets
    // emitted, so it is what gets trusted (CF-1a's rule, kept here
    // because the two have already disagreed once, in if_convert).
    bool buildEdges()
    {
        for (auto& b : fn_.blocks)
            if (b) blocks_.push_back(b.get());

        std::unordered_map<std::string, IRBasicBlock*> byName;
        for (IRBasicBlock* b : blocks_)
            byName[b->name] = b;

        for (IRBasicBlock* b : blocks_) {
            bool terminated = false;
            for (const auto& instPtr : b->instructions) {
                if (!instPtr) continue;
                IRInstruction& inst = *instPtr;
                if (inst.result != InvalidIRValue)
                    defMap_[inst.result] = &inst;
                if (terminated)
                    return refuse("block '" + b->name +
                                  "' has instructions past its terminator");
                if (inst.op == IROp::Return) { terminated = true; continue; }
                if (inst.op != IROp::Branch && inst.op != IROp::CondBranch)
                    continue;
                terminated = true;
                std::vector<std::string> names;
                const std::string& t = inst.targetName;
                size_t start = 0;
                while (true) {
                    const size_t comma = t.find(',', start);
                    names.push_back(comma == std::string::npos
                                        ? t.substr(start)
                                        : t.substr(start, comma - start));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                if (inst.op == IROp::CondBranch) {
                    if (names.size() != 2)
                        return refuse("conditional branch in '" + b->name +
                                      "' does not name two targets");
                    if (inst.operands.empty())
                        return refuse("conditional branch in '" + b->name +
                                      "' has no condition operand");
                } else if (names.size() != 1) {
                    return refuse("branch in '" + b->name +
                                  "' does not name one target");
                }
                for (size_t i = 0; i < names.size(); ++i) {
                    auto it = byName.find(names[i]);
                    if (it == byName.end())
                        return refuse("branch to unknown block '" +
                                      names[i] + "'");
                    IRBasicBlock* target = it->second;
                    succs_[b].push_back(target);
                    preds_[target].push_back(b);
                    if (inst.op == IROp::CondBranch) {
                        GuardLiteral lit;
                        lit.cond = inst.operands[0];
                        lit.taken = (i == 0);   // targetName is "then,else"
                        edgeLiteral_[b][target] = lit;
                    }
                }
            }
            if (!terminated)
                return refuse("block '" + b->name + "' has no terminator");
        }
        return true;
    }

    // Reverse post-order of the forward DAG.  A grey revisit is a
    // back-edge: a discard inside a loop is refused here, and the general
    // path refuses the loop itself a moment later for the same reason.
    bool orderBlocks()
    {
        std::unordered_map<const IRBasicBlock*, int> color;
        std::vector<IRBasicBlock*> post;
        struct Frame { IRBasicBlock* b; size_t next; };
        std::vector<Frame> stack;
        stack.push_back({blocks_.front(), 0});
        color[blocks_.front()] = 1;
        while (!stack.empty()) {
            const Frame f = stack.back();
            const auto& ss = succs_[f.b];
            if (f.next < ss.size()) {
                stack.back().next = f.next + 1;
                IRBasicBlock* nb = ss[f.next];
                int& c = color[nb];
                if (c == 1)
                    return refuse("control flow has a back-edge (loop)");
                if (c == 0) { c = 1; stack.push_back({nb, 0}); }
            } else {
                color[f.b] = 2;
                post.push_back(f.b);
                stack.pop_back();
            }
        }
        for (IRBasicBlock* b : blocks_) {
            if (color[b] == 2) continue;
            for (const auto& instPtr : b->instructions)
                if (instPtr && instPtr->op == IROp::Discard)
                    return refuse("discard in unreachable block '" +
                                  b->name + "'");
        }
        order_.assign(post.rbegin(), post.rend());
        return true;
    }

    // guard(entry) = {} (true).  For any other block, the intersection
    // over its predecessors of (their guard plus the edge's literal) —
    // VERIFIED, not trusted: intersection alone is a must-analysis and is
    // weaker than "this block is reached", which for an unstructured
    // merge would hand a discard an emptier guard than the truth and kill
    // every fragment.  The check below makes the result exact.
    bool computeGuards()
    {
        for (size_t i = 0; i < order_.size(); ++i) {
            IRBasicBlock* b = order_[i];
            const auto& ps = preds_[b];
            if (i == 0 || ps.empty()) {
                guard_[b] = GuardSet{};
                continue;
            }
            std::vector<GuardSet> incoming;
            incoming.reserve(ps.size());
            for (IRBasicBlock* p : ps) {
                auto git = guard_.find(p);
                if (git == guard_.end())
                    return refuse("predecessor of '" + b->name +
                                  "' was not ordered before it");
                GuardSet s = git->second;
                auto eit = edgeLiteral_.find(p);
                if (eit != edgeLiteral_.end()) {
                    auto lit = eit->second.find(b);
                    if (lit != eit->second.end() && !contains(s, lit->second))
                        s.push_back(lit->second);
                }
                incoming.push_back(std::move(s));
            }
            GuardSet g = incoming.front();
            for (size_t k = 1; k < incoming.size(); ++k)
                g = intersect(g, incoming[k]);

            if (!verifyMerge(b, incoming, g))
                return false;
            guard_[b] = std::move(g);
        }
        return true;
    }

    // Exactness check.  One predecessor: nothing was dropped, so the
    // guard is the predecessor's plus the edge.  Two: the ONLY literals
    // the intersection may drop are one complementary pair from the SAME
    // branch, which is a well-formed diamond and makes
    // (G AND c) OR (G AND NOT c) == G.  Anything else refuses.
    //
    // The three-or-more arm and the failed-check arm are DEFENSIVE CODE
    // WITH NO WITNESS: the frontend nests `else if` into binary merges
    // and Cg has no goto, so nothing in the language reaches them today.
    // They are not claimed as guards - an unwitnessed guard is not a
    // guard - they are here so the pass cannot be wrong quietly if the
    // frontend ever grows a shape that produces one.
    bool verifyMerge(const IRBasicBlock* b,
                     const std::vector<GuardSet>& incoming,
                     const GuardSet& g)
    {
        if (incoming.size() <= 1)
            return true;
        if (!blockOrBelowHasDiscard(b))
            return true;   // no discard depends on this guard; do not refuse
        if (incoming.size() != 2)
            return refuse("merge block '" + b->name + "' has " +
                          std::to_string(incoming.size()) +
                          " predecessors; a discard below it would need a "
                          "disjunction this pass does not build");
        GuardSet dropped0, dropped1;
        for (const GuardLiteral& l : incoming[0])
            if (!contains(g, l)) dropped0.push_back(l);
        for (const GuardLiteral& l : incoming[1])
            if (!contains(g, l)) dropped1.push_back(l);
        if (dropped0.size() != 1 || dropped1.size() != 1 ||
            dropped0[0].cond != dropped1[0].cond ||
            dropped0[0].taken == dropped1[0].taken)
            return refuse("merge block '" + b->name +
                          "' is not a complementary pair of arms of one "
                          "branch; a discard below it would get a guard "
                          "weaker than its reachability condition");
        return true;
    }

    // A guard only has to be exact where a discard depends on it.  Merges
    // elsewhere in the shader are none of this pass's business, and
    // refusing on them would refuse shaders that have no discard problem.
    bool blockOrBelowHasDiscard(const IRBasicBlock* b)
    {
        std::unordered_set<const IRBasicBlock*> seen;
        std::vector<const IRBasicBlock*> work{b};
        while (!work.empty()) {
            const IRBasicBlock* cur = work.back();
            work.pop_back();
            if (!seen.insert(cur).second) continue;
            for (const auto& instPtr : cur->instructions)
                if (instPtr && instPtr->op == IROp::Discard)
                    return true;
            auto it = succs_.find(cur);
            if (it == succs_.end()) continue;
            for (const IRBasicBlock* s : it->second)
                work.push_back(s);
        }
        return false;
    }

    // `!x` at the top of a guard is folded into the KIL's condition-code
    // test rather than emitted: the reference keeps the comparison and
    // predicates the kill on EQ.  Peel the whole chain so `!!x` comes
    // back to NE, and so the else-arm of `if (!(a > b))` reads as the
    // plain `a > b` with NE that it is.
    void peelNegations(IRValueID& value, bool& negated)
    {
        for (int depth = 0; depth < 64; ++depth) {
            auto it = defMap_.find(value);
            if (it == defMap_.end()) return;
            const IRInstruction* def = it->second;
            if (def->op != IROp::LogicalNot || def->operands.empty()) return;
            value = def->operands[0];
            negated = !negated;
        }
    }

    void materialise()
    {
        for (IRBasicBlock* b : order_) {
            const GuardSet& g = guard_[b];
            for (size_t i = 0; i < b->instructions.size(); ++i) {
                IRInstruction* inst = b->instructions[i].get();
                if (!inst || inst->op != IROp::Discard) continue;
                if (!inst->operands.empty()) continue;  // already guarded
                if (g.empty()) { ++result_.unconditional; continue; }

                std::vector<std::unique_ptr<IRInstruction>> emitted;
                IRValueID acc = InvalidIRValue;
                bool negated = false;

                if (g.size() == 1) {
                    // The single-literal case is where the reference's
                    // fold applies: keep the comparison, flip the test.
                    acc = g[0].cond;
                    negated = !g[0].taken;
                    peelNegations(acc, negated);
                } else {
                    for (const GuardLiteral& lit : g) {
                        IRValueID v = lit.cond;
                        bool neg = !lit.taken;
                        peelNegations(v, neg);
                        if (neg) {
                            auto n = std::make_unique<IRInstruction>(
                                IROp::LogicalNot, fn_.allocateValueId(),
                                IRTypeInfo::Bool());
                            n->addOperand(v);
                            n->loc = inst->loc;
                            v = n->result;
                            emitted.push_back(std::move(n));
                        }
                        if (acc == InvalidIRValue) {
                            acc = v;
                            continue;
                        }
                        auto a = std::make_unique<IRInstruction>(
                            IROp::LogicalAnd, fn_.allocateValueId(),
                            IRTypeInfo::Bool());
                        a->addOperand(acc);
                        a->addOperand(v);
                        a->loc = inst->loc;
                        acc = a->result;
                        emitted.push_back(std::move(a));
                    }
                }

                inst->addOperand(acc);
                inst->guardIsNegated = negated;
                ++result_.guarded;

                if (!emitted.empty()) {
                    for (auto& e : emitted) e->parentBlock = b;
                    b->instructions.insert(
                        b->instructions.begin() +
                            static_cast<std::ptrdiff_t>(i),
                        std::make_move_iterator(emitted.begin()),
                        std::make_move_iterator(emitted.end()));
                    i += emitted.size();
                }
            }
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------
// Read-only guard query, used by the DEFAULT path.
//
// Same rule as the pass above - intersection over predecessors with the
// complementary-pair verification - computed without touching the IR,
// because the matcher must not have its input rewritten by a question.
//
// It exists because the first attempt wrote this walk a SECOND time in
// nv40_fp_emit.cpp and the copy was wrong at exactly the place a copy
// tends to be: it stopped at a merge and treated that as "no guard
// remains", when a merge cancels only the branch that FORMED it and says
// nothing about a branch enclosing it (codex's counterexample:
// `if (A) { if (B) { .. } else { .. } discard; }` - B cancels, A does
// not).
namespace
{

class GuardQuery
{
public:
    explicit GuardQuery(const IRFunction& fn) : fn_(fn) {}

    BlockGuard forBlock(const IRBasicBlock* target)
    {
        BlockGuard result;
        if (!target) return result;
        if (!buildEdges()) return result;
        if (!orderBlocks()) return result;
        if (!computeGuards()) return result;
        const auto it = guard_.find(target);
        if (it == guard_.end()) return result;
        result.proven = true;
        result.literals = it->second;
        return result;
    }

private:
    const IRFunction& fn_;
    std::vector<const IRBasicBlock*> blocks_;
    std::unordered_map<const IRBasicBlock*,
                       std::vector<const IRBasicBlock*>> succs_;
    std::unordered_map<const IRBasicBlock*,
                       std::vector<const IRBasicBlock*>> preds_;
    std::unordered_map<const IRBasicBlock*,
                       std::unordered_map<const IRBasicBlock*, GuardLiteral>>
        edgeLiteral_;
    std::vector<const IRBasicBlock*> order_;
    std::unordered_map<const IRBasicBlock*, GuardSet> guard_;

    // A block whose last instruction is a `discard` carries no terminator
    // (isTerminator(Discard) is true, so the frontend emitted no branch to
    // the merge).  The pass rewrites that; a query must not, so the edge
    // is inferred here the same way - to the next block in creation order.
    bool buildEdges()
    {
        for (const auto& b : fn_.blocks)
            if (b) blocks_.push_back(b.get());
        if (blocks_.empty()) return false;

        std::unordered_map<std::string, const IRBasicBlock*> byName;
        for (const IRBasicBlock* b : blocks_)
            byName[b->name] = b;

        for (size_t bi = 0; bi < blocks_.size(); ++bi) {
            const IRBasicBlock* b = blocks_[bi];
            bool terminated = false;
            for (const auto& instPtr : b->instructions) {
                if (!instPtr) continue;
                const IRInstruction& inst = *instPtr;
                if (terminated) return false;
                if (inst.op == IROp::Return) { terminated = true; continue; }
                if (inst.op != IROp::Branch && inst.op != IROp::CondBranch)
                    continue;
                terminated = true;
                std::vector<std::string> names;
                const std::string& t = inst.targetName;
                size_t start = 0;
                while (true) {
                    const size_t comma = t.find(',', start);
                    names.push_back(comma == std::string::npos
                                        ? t.substr(start)
                                        : t.substr(start, comma - start));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                if (inst.op == IROp::CondBranch &&
                    (names.size() != 2 || inst.operands.empty()))
                    return false;
                if (inst.op == IROp::Branch && names.size() != 1)
                    return false;
                for (size_t i = 0; i < names.size(); ++i) {
                    auto it = byName.find(names[i]);
                    if (it == byName.end()) return false;
                    succs_[b].push_back(it->second);
                    preds_[it->second].push_back(b);
                    if (inst.op == IROp::CondBranch) {
                        GuardLiteral lit;
                        lit.cond = inst.operands[0];
                        lit.taken = (i == 0);
                        edgeLiteral_[b][it->second] = lit;
                    }
                }
            }
            if (terminated) continue;
            // Unterminated: a trailing discard falls through to the next
            // block, and anything else is a shape this query will not
            // guess at.
            const IRInstruction* last =
                b->instructions.empty() ? nullptr
                                        : b->instructions.back().get();
            if (!last || last->op != IROp::Discard) return false;
            if (bi + 1 >= blocks_.size()) return false;
            const IRBasicBlock* next = blocks_[bi + 1];
            succs_[b].push_back(next);
            preds_[next].push_back(b);
        }
        return true;
    }

    bool orderBlocks()
    {
        std::unordered_map<const IRBasicBlock*, int> color;
        std::vector<const IRBasicBlock*> post;
        struct Frame { const IRBasicBlock* b; size_t next; };
        std::vector<Frame> stack;
        stack.push_back({blocks_.front(), 0});
        color[blocks_.front()] = 1;
        while (!stack.empty()) {
            const Frame f = stack.back();
            const auto& ss = succs_[f.b];
            if (f.next < ss.size()) {
                stack.back().next = f.next + 1;
                const IRBasicBlock* nb = ss[f.next];
                int& c = color[nb];
                if (c == 1) return false;      // back-edge
                if (c == 0) { c = 1; stack.push_back({nb, 0}); }
            } else {
                color[f.b] = 2;
                post.push_back(f.b);
                stack.pop_back();
            }
        }
        order_.assign(post.rbegin(), post.rend());
        return true;
    }

    bool computeGuards()
    {
        for (size_t i = 0; i < order_.size(); ++i) {
            const IRBasicBlock* b = order_[i];
            const auto& ps = preds_[b];
            if (i == 0 || ps.empty()) { guard_[b] = GuardSet{}; continue; }
            std::vector<GuardSet> incoming;
            for (const IRBasicBlock* p : ps) {
                auto git = guard_.find(p);
                if (git == guard_.end()) return false;
                GuardSet s = git->second;
                auto eit = edgeLiteral_.find(p);
                if (eit != edgeLiteral_.end()) {
                    auto lit = eit->second.find(b);
                    if (lit != eit->second.end() && !contains(s, lit->second))
                        s.push_back(lit->second);
                }
                incoming.push_back(std::move(s));
            }
            GuardSet g = incoming.front();
            for (size_t k = 1; k < incoming.size(); ++k)
                g = intersect(g, incoming[k]);
            if (incoming.size() > 1) {
                // Exactly the pass's verification: two predecessors, and
                // the only literals the intersection dropped are one
                // complementary pair from the same branch.  Anything else
                // is unproven - and unproven must not read as "no guard",
                // which is the mistake this query was written to stop.
                if (incoming.size() != 2) return false;
                GuardSet d0, d1;
                for (const GuardLiteral& l : incoming[0])
                    if (!contains(g, l)) d0.push_back(l);
                for (const GuardLiteral& l : incoming[1])
                    if (!contains(g, l)) d1.push_back(l);
                if (d0.size() != 1 || d1.size() != 1 ||
                    d0[0].cond != d1[0].cond || d0[0].taken == d1[0].taken)
                    return false;
            }
            guard_[b] = std::move(g);
        }
        return true;
    }
};

}  // namespace

BlockGuard guardForBlockContaining(const IRFunction& entry,
                                   const IRInstruction& inst)
{
    const IRBasicBlock* block = nullptr;
    for (const auto& bp : entry.blocks) {
        if (!bp) continue;
        for (const auto& ip : bp->instructions)
            if (ip.get() == &inst) { block = bp.get(); break; }
        if (block) break;
    }
    // NEVER inst.parentBlock: if_convert moves instructions between blocks
    // with a raw vector insert and never updates that field, so a hoisted
    // discard still names a block that has since been erased.
    if (!block) return BlockGuard{};
    return GuardQuery(entry).forBlock(block);
}

DiscardGuardResult materialiseDiscardGuards(IRModule& module)
{
    DiscardGuardResult result;
    for (auto& fn : module.functions) {
        if (!fn || !fn->isEntryPoint) continue;
        FunctionPass(*fn, result).run();
        if (!result.ok) break;
    }
    return result;
}

}  // namespace nv40
