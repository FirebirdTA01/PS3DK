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
Every blocked shader is structured if/else from Cg source.

CORRECTED 2026-09-01 (measured, was wrong as first written): the IR
does NOT model joins with `phi` nodes.  `IROp::Phi` has no producer
anywhere in the tree — the only references are a printer case and a
defensive consumer in nv40_if_convert.  What the frontend actually
emits (verified by `--dump-ir` on a nested-if fixture) is a
reducible CFG with `brc %c -> then,else` / arms `br -> join`, and
joins resolved by **`select` instructions the frontend inserts at
the merge block** (ir_builder's buildIfStmt diverged-variable merge):

```
entry:    brc %9 -> if.then0,if.else1
if.then0: %16 = select %13, %15, %6      ; inner if, in-arm
          br -> if.end2
if.else1: br -> if.end2
if.end2:  %19 = select %9, %16, %6       ; join, pre-resolved
```

Consequence: the milestone does NOT need branch instructions, loop
encodings, CFG reconstruction, or phi resolution.  Flattening itself
is small (topologically concatenate, drop terminators, refuse the
shapes §2 names); the correctness weight of the milestone moves
almost entirely onto HOW THE JOIN SELECTS LOWER, per §2 step 3.

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
3. (REWRITTEN with the §1 correction — joins arrive from the
   frontend already resolved as `select cond, thenVal, elseVal`; the
   pass resolves nothing itself.)  The requirement transfers intact
   to SELECT LOWERING: a join select whose arms are not provably
   finite MUST lower as a **PREDICATED WRITE**, not the arithmetic
   blend — MOV the default (`elseVal`) into the destination
   unconditionally, set the condition code from `cond`, then commit
   `thenVal` against a CC-gated destination (`op dst(NE.x), ...`) so
   it lands only where the predicate holds.  This is what the
   reference emits for the guarded idioms (measured: `SGTRC RC.x`
   then `RCPR R0.x(NE.x)`), and the general path's VInstr already
   carries the fields (`ccUpdate`, `predicate`).
   The arithmetic blend (`d = a-b; dst = cond*d + b`) — which is what
   slice B's `lowerSelectGeneral` does today — is the OPT-IN special
   case, allowed only when both arms are provably finite (e.g. both
   literals or direct varying reads) — because it is NOT a
   conditional move: if the untaken arm yields inf/NaN, `0 * NaN`
   poisons the blend even though the arm "was not taken", and the
   canonical guarded-divide (`if (x>0) r = 1/x; else r = 0;`) is
   exactly the shape real shaders write.  Purity excludes side
   effects; it does not exclude VALUE CONTAMINATION.  (Found in
   design review, with the reference's predicated emission as the
   settling evidence.)  NOTE the scope change the correction forces:
   the predicated select upgrade was filed as an optional follow-up
   when it looked like flatten would emit PredCarry itself; with
   joins arriving as selects it IS the milestone's correctness core,
   and CF-1 without it unlocks only shaders whose join arms are
   provably finite (the pass must REFUSE the rest, loudly, until the
   predicated lowering lands).
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

Reach note (SUPERSEDED by the §1 correction, kept for the history):
this originally filed the predicated-select upgrade as an optional
follow-up on the theory that the flatten pass would emit its own
predicated joins.  Joins arrive AS selects, so the upgrade is CF-1's
correctness core (see step 3) — and it fixes the same contamination
for source-level `?:`, which shares the lowering.

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

- **CF-1a**: the flattening pass (topo-concatenate, drop
  terminators), if/else only, refuse loudly on back-edges, off-exit
  stores, discard, and — per the corrected step 3 — any join select
  whose arms are not provably finite (proven-empty style: the
  refusal is the guard against the shapes the pass does not handle).
  Gated to `--general-lowering`; the default path must stay
  byte-unmoved, and a flattened shader newly compiling on the
  default path would be a verdict change the fence rejects.
  Unlocks the provably-finite-arm subset.
- **CF-1b**: predicated select lowering in the general path
  (`ccUpdate` + `predicate` on VInstr; MOV default, CC-set from
  cond, CC-gated commit).  Lifts the finite-arm refusal; the
  guarded-divide readback row is its acceptance.  Together 1a+1b
  unlock up to 25 shaders modulo co-blockers.
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
