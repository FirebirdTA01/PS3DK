#ifndef NV40_IF_CONVERT_H
#define NV40_IF_CONVERT_H

/*
 * If-conversion (flow → predication) pass.
 *
 * NV40 FP *does* have IF / ELSE / ENDIF branch opcodes (BRA sub-ops
 * 2 / ELSE / etc.) but the reference compiler rarely emits them — at `-O2 --fastmath`
 * it collapses simple if-else diamonds into straight-line CC-predicated
 * writes, which the back-end already knows how to emit via
 * `IROp::Select`.  This pass rewrites matching IR shapes into that form
 * so the existing Select emit path picks them up verbatim.
 *
 * Rewrites two patterns:
 *
 *   1. Full if-else diamond — each branch is a single StoreOutput to
 *      the same semantic, joined at a block that only returns.
 *
 *         entry { ...cond... brc %c -> then, else }
 *         then  { stout %t SEM; br -> join }
 *         else  { stout %e SEM; br -> join }
 *         join  { ret }
 *
 *      →  entry { ...cond... %s = select %c ? %t : %e; stout %s SEM; ret }
 *
 *   2. If-only (no else) — entry carries a default StoreOutput to the
 *      same semantic before the branch, the then-block overrides it,
 *      and the join is the return.
 *
 *         entry { stout %d SEM; ...cond... brc %c -> then, join }
 *         then  { stout %t SEM; br -> join }
 *         join  { ret }
 *
 *      →  entry { ...cond... %s = select %c ? %t : %d; stout %s SEM; ret }
 *
 *   3. If-only with multi-instruction THEN — entry carries a default
 *      StoreOutput before the branch, then-block computes a chain of
 *      arithmetic instructions each followed by a StoreOutput to the
 *      same semantic, join is return-only.
 *
 *         entry { stout %d SEM; ...cond... brc %c -> then, join }
 *         then  { %r1 = OP1 ...; stout %r1 SEM;
 *                 %r2 = OP2 ... %r1 ...; stout %r2 SEM;
 *                 ...
 *                 %rN = OPN ... %r(N-1) ...; stout %rN SEM;
 *                 br -> join }
 *         join  { ret }
 *
 *      →  entry { ...cond...
 *                 %p1 = predcarry %c, %d, OP1 ...
 *                 %p2 = predcarry %c, %p1, OP2 ... %p1 ...
 *                 ...
 *                 %pN = predcarry %c, %p(N-1), OPN ... %p(N-1) ...
 *                 stout %pN SEM; ret }
 *
 *      Each PredCarry lowers in NV40 FP emit to a 2-instruction pair:
 *      MOVR Rdst, prev_value (carry the running result), then the
 *      inner OP re-executed against Rdst(NE.x) so it only commits
 *      when the predicate fires.  See nv40_fp_emit's PredCarry handler.
 *
 *   4. If-only with NO per-branch StoreOutput.  IRBuilder has already
 *      synthesised a Select at the merge for any local redefined inside
 *      THEN, and the merge carries the StoreOutput / Return.  This is
 *      the uniform-conditional shape from the SDK samples:
 *
 *          if (gFunctionSwitch != 0.0) { c = c * tex2D(...); }
 *          c.a = 1.0;  return c;
 *
 *      THEN must hold only pure ops (isPureOp) ending in an
 *      unconditional Branch to the merge, and the ELSE edge must be
 *      that same merge.  THEN's body is hoisted into entry before the
 *      CondBranch, the CondBranch and THEN are dropped, and the merge -
 *      now single-predecessor - is inlined into entry.
 *
 *   5. If-only whose THEN is a `discard`.  The discard is hoisted into
 *      entry before the CondBranch, the CondBranch becomes an
 *      unconditional Branch to the join, the discard block is removed,
 *      and a single-predecessor join is inlined.
 *
 * THE PASS ITERATES TO A FIXPOINT.  `convertSimpleIfElse` re-runs the
 * whole matcher until a round changes nothing, rebuilding the
 * value→type map each time because collapsing a diamond synthesises new
 * Select values.  So a NESTED diamond does collapse from the inside out,
 * in a handful of rounds - it is not left alone, and the note below
 * saying so was written before the loop existed.
 *
 * What genuinely is left alone: a full if-else whose branches are
 * multi-instruction, non-Output writes inside the branches, and any nest
 * whose INNER arm matches none of the five shapes.  That last one stalls
 * the whole nest, and today the refusal names the OUTER shape - a
 * wrong-cause diagnostic, and its own defect rather than this pass's
 * intent.
 *
 * ORDERING CONSTRAINT, and it is the one an outside reader will miss:
 * `materialiseDiscardGuards` MUST RUN BEFORE THIS PASS (see main.cpp,
 * where it does).  Shape 5 hoists a then-arm discard into the entry
 * block and DELETES the CondBranch, after which the discard's guard is
 * recoverable only by POSITION.  Give every discard its path condition
 * while the control flow is still intact, or a conditional kill quietly
 * becomes an unconditional one.
 *
 * Note the asymmetry that follows from it: this pass runs on BOTH
 * lowering paths, while guard materialisation runs on the general path
 * only.  On the legacy matcher the guard is therefore still recovered by
 * position, which holds for a single-level `if (cond) discard;` - both
 * paths emit the comparison and a KIL under NE - and is exactly the
 * fragility the guard pass was written to remove for anything nested.
 *
 * FINALLY, FOR ANYONE READING THIS AS A SPECIFICATION - and someone
 * has: this pass PREDATES the general lowering path, which resolves
 * diamonds through its own SelPred lowering and discards through guard
 * expression trees.  The live design is there.  This pass is where the
 * shapes were learned, and it still runs, but it is not the whole story.
 */

class IRModule;

namespace nv40
{

// Runs the rewrite on every entry-point function in the module.
// Returns the number of diamonds converted.
int convertSimpleIfElse(IRModule& module);

}  // namespace nv40

#endif  // NV40_IF_CONVERT_H
