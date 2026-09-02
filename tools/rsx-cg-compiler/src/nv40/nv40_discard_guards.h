#ifndef NV40_DISCARD_GUARDS_H
#define NV40_DISCARD_GUARDS_H

/*
 * CF-2 (t_91bbd575): give every `discard` its PATH CONDITION, as IR.
 *
 * `IROp::Discard` carries no operand: its guard is the control flow that
 * reaches it.  That is fine while the emitter walks a structured CFG and
 * ruinous once it does not.  The default path recovers the guard as "the
 * last comparison I walked past", which is the wrong polarity for a
 * discard on the FALSE arm of a branch (t_79fc6bf7 — the shipping
 * compiler kills exactly the fragments that must survive), and the
 * general path flattens the blocks and drops the terminators, after
 * which the information is gone entirely.
 *
 * This pass runs while the control flow is still intact and rewrites
 *
 *      brc %c -> then, else
 *      then: discard
 *
 * into `discard %c`, and the else-arm case into `discard %c` with the
 * instruction's `guardIsNegated` flag set — the shape the reference
 * emits, which predicates the KIL on EQ rather than inverting the
 * comparison.
 *
 * Ordering matters twice:
 *   - it must run BEFORE nv40::convertSimpleIfElse.  That pass's shape 5
 *     hoists a then-arm discard into the entry block and DELETES the
 *     CondBranch; measured on an unrolled loop, four discards ended up in
 *     straight-line code with their conditions computed and unused.
 *   - shape 5 in turn declines any discard that already carries a guard,
 *     because it erases the discard's block and would take the guard
 *     instructions with it.
 *
 * General path only, for now: the default path's matcher is unchanged by
 * design, so a shader that newly compiled because of this pass would be a
 * verdict change no fence asked for.
 */

#include <string>
#include <vector>

#include "../donor/ir/ir.h"

class IRModule;

namespace nv40
{

struct DiscardGuardResult
{
    bool ok = true;                        // false: refused, see diagnostics
    int  guarded = 0;                      // discards that gained a condition
    int  unconditional = 0;                // discards reached on every path
    std::vector<std::string> diagnostics;
};

// Rewrites every `discard` in every entry-point function of `module`.
// A module with no discard is untouched and costs one walk.
DiscardGuardResult materialiseDiscardGuards(IRModule& module);

// One branch condition with the polarity of the edge that was taken.
struct GuardLiteral
{
    IRValueID cond = InvalidIRValue;
    bool taken = true;      // true: the THEN arm
};

// What guards the block containing `inst`, computed READ-ONLY.
//
// The same intersection-over-predecessors with the complementary-pair
// verification the pass above uses, exposed because the DEFAULT path needs
// the same answer for the opposite purpose: it recovers a discard's guard
// as a single comparison, so it has to know whether that comparison IS the
// whole guard before it emits a kill (t_79fc6bf7, t_7ae60244).  One rule,
// one implementation - the first attempt wrote the walk a second time in
// the emitter and the copy was wrong at merges.
//
// `proven` false means the shape is one this computation cannot verify: a
// back-edge, more than two predecessors, or a merge whose predecessors do
// not differ by exactly one complementary pair.  A caller deciding whether
// to REFUSE must treat that as "cannot prove", never as "no guard".
struct BlockGuard
{
    bool proven = false;
    std::vector<GuardLiteral> literals;
};

BlockGuard guardForBlockContaining(const IRFunction& entry,
                                   const IRInstruction& inst);

}  // namespace nv40

#endif  // NV40_DISCARD_GUARDS_H
