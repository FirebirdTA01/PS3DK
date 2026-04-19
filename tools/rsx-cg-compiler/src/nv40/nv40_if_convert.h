#ifndef NV40_IF_CONVERT_H
#define NV40_IF_CONVERT_H

/*
 * If-conversion (flow → predication) pass.
 *
 * NV40 FP *does* have IF / ELSE / ENDIF branch opcodes (BRA sub-ops
 * 2 / ELSE / etc.) but sce-cgc rarely emits them — at `-O2 --fastmath`
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
 * Anything else (multi-instruction branches, non-Output writes inside
 * the branches, Phi nodes, nested diamonds) is left alone — the NV40
 * FP emit path will emit a "control flow not yet supported" diagnostic
 * in that case, prompting us to add real IF / ELSE / ENDIF later.
 */

class IRModule;

namespace nv40
{

// Runs the rewrite on every entry-point function in the module.
// Returns the number of diamonds converted.
int convertSimpleIfElse(IRModule& module);

}  // namespace nv40

#endif  // NV40_IF_CONVERT_H
