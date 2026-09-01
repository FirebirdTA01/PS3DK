# General-path control flow (`br`/`brc`/`discard`) — design note

Status: SCOPED, not started.  Board task: t_91bbd575.
Blocks 25 corpus shaders (brc/br) + 9 more (discard) — the largest
remaining unlock after the slice A/B straight-line work.

## 1. The measurement that shapes everything

Classifying all 25 branch-blocked corpus shaders by CFG shape
(2026-08-31, entry-function block graphs from `--dump-ir`):

```
back-edges (loops):        0
forward-only (if/else):   25
```

**There are no loops anywhere in the corpus that reach lowering.**
Every blocked shader is structured if/else from Cg source.  The IR
models the join with `phi` nodes over a reducible, frontend-generated
CFG (`brc %c -> then,else`, arms `br -> join`, `phi` at the join).

Consequence: the milestone does NOT need branch instructions, loop
encodings, or CFG reconstruction.  It needs one IR-level pass.

## 2. Design: flatten to predicated writes

A forward-only structured CFG can be executed UNCONDITIONALLY — both
arms always run — with joins resolved by PREDICATED COMMIT rather
than by branching (no branch instructions; a condition code gates
the write):

1. Compute each block's **path condition**: the entry has condition
   `true`; a `brc %c -> T,E` gives T the condition `parent AND c` and
   E `parent AND NOT c`.  Structured Cg input keeps these expressions
   trivial (no arbitrary boolean network — a chain per nesting level).
2. Topologically order the blocks and concatenate their instructions
   into one straight line.
3. Resolve every `phi [T: a, E: b]` with a **PREDICATED WRITE**, not
   the arithmetic select: MOV the default (`b`) into the join
   register unconditionally, set the condition code from
   `pathcond(T)`, then commit `a`'s producing op against a CC-gated
   destination (`op dst(NE.x), ...`) so it lands only where the
   predicate holds.  This is exactly the `PredCarry` shape the IR
   already models, and it is what the reference emits for the guarded
   idioms (measured: `SGTRC RC.x` then `RCPR R0.x(NE.x)`).
   The arithmetic select (`d = a-b; dst = cond*d + b`) is the OPT-IN
   special case, allowed only when both arms are provably finite
   (e.g. both literals or direct varying reads) — because it is NOT a
   conditional move: if the untaken arm yields inf/NaN, `0 * NaN`
   poisons the blend even though the arm "was not taken", and the
   canonical guarded-divide (`if (x>0) r = 1/x; else r = 0;`) is
   exactly the shape real shaders write.  Purity excludes side
   effects; it does not exclude VALUE CONTAMINATION.  (Found in
   design review, with the reference's predicated emission as the
   settling evidence.)
4. Drop the `br`/`brc` terminators.

Safety argument for executing both arms: arm instructions are pure
ALU/texture reads (samples have no side effects), and with predicated
commits an arm's value cannot contaminate the join.  The two impure
cases are handled separately: `discard` (below) and outputs — the IR
writes outputs only at the exit block in frontend-generated code, and
the pass must REFUSE (loudly, per house rule) if it ever sees a
`stout` off the exit path rather than assume.

Open hardware question, deliberately NOT load-bearing: NV40 FP is not
fully IEEE and may flush `0 * inf` to 0, which would make the
arithmetic select accidentally safe.  The design does not rest on
that; the guarded-divide arm of the §5 readback row answers the
hardware question and the lowering question in one measurement.

Reach note: slice B's landed `?:` select lowering shares the
contamination property for risky arms (Cg's `?:` evaluates both
sides, but the reference PREDICATES the guarded form rather than
blending) — upgrading it to the predicated form for non-provably-
finite arms is filed as its own follow-up; the flatten pass simply
never introduces the arithmetic form where it is unsafe.

This mirrors what NV40 drivers do anyway: the inherited nvfx header
notes the proprietary driver goes far out of its way to avoid native
branching (loops unrolled up to 500 executed instructions).  Real
IF/ELSE/ENDIF encodings (`NV40_FP_OP_OPCODE_IS_BRANCH`, BRA opcodes)
stay unused until a shader arrives that predication cannot express —
none exists in the corpus today.

## 3. discard / KIL

`discard` has NO result value, which is how it slipped through as a
silent drop before the unsupported-op refusal (it was the witness for
that fix).  After flattening, a discard carries its path condition:

- unconditional discard: `KIL` with condition TR.
- conditional: set the condition-code register from the path condition
  (any op with `COND_WRITE_ENABLE` writing a scratch destination),
  then `KIL` predicated `GT` zero — the CC system
  (`NVFX_FP_OP_COND_*` + per-lane condition swizzle) exists on every
  FP instruction, no branch needed.

Fragment-only by definition; a VP discard is a frontend error.

## 4. Slices

- **CF-1**: the flattening pass, if/else only, refuse loudly on
  back-edges or off-exit stores (proven-empty style: the refusal is
  the guard against the shapes the pass does not handle).  Unlocks up
  to 25 shaders modulo co-blockers.
- **CF-2**: discard via CC+KIL (needs CF-1's path conditions).  +9.
- **CF-3** (unscheduled): native branching / loops — no corpus
  demand; opens only if a real shader shows up with a dynamic loop.

## 5. Testing

- Tier a: diamond fixture, nested-diamond fixture, back-edge REFUSAL
  fixture (a hand-written loop must refuse loudly, not flatten wrong).
- Corpus acceptance: delta sweep, newly-passing all explained,
  proven-empty on the passing set, default path byte-unmoved.
- Tier c: a readback row with an if/else whose two arms produce
  different constants per pixel half (`u < 0.5 ? A : B` via real
  `if`) — judged in pixels like everything else; extends t_665f641c's
  row or stands alone.  PLUS the guarded-divide arm
  (`if (u > 0.5) r = 0.25/u; else r = 0.0;`): its expected values are
  finite everywhere, so a contaminating lowering fails the judge on
  the else-half pixels — one row answers both the NV40 `0*inf`
  hardware question and the predication-lowering question.
- discard row: pattern-kill (checkerboard discard), judged against the
  clear color where killed.

## 6. Cost

CF-1 is an IR-to-IR pass plus zero new hardware encodings; CF-2 is
one encoder feature (CC write + KIL).  The expensive thing this note
retires is the imagined need for IF/ELSE/ENDIF encoding, loop
handling, and general CFG reconstruction — the corpus measurement
says none of it is needed to reach 135/135.
