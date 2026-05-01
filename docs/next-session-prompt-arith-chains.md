# Next-session prompt — sce-cgc accumulator-fold byte-match (loop_static3_f)

Paste this as the opening user message of a fresh Claude Code session in
the PS3 Custom Toolchain repo. Self-contained — no prior context needed
beyond what's checked into the repo and `~/.claude/projects/.../memory/`.

---

The previous session landed static for-loop unrolling + DCE in the
rsx-cg-compiler (commit pending — see `git status`). 48/48 byte-match
holds with two new tests: `loop_trip1_f.cg` (1-iteration unroll → single
ADDR) and `loop_dead_store_f.cg` (3-iteration unroll where every iter
overwrites the same lvalue → DCE collapses to single ADDR).

The case the previous session **could not** byte-match is the original
`loop_static3_f.cg` from `docs/next-session-prompt-loops.md`:

```cg
void main(
    float4 vcol      : COLOR,
    out float4 color : COLOR)
{
    float acc = 0.0f;
    for (int i = 0; i < 3; i++) {
        acc += vcol.x;
    }
    color = float4(acc, 0, 0, 1);
}
```

Run `bash tools/rsx-cg-compiler/tests/run_diff.sh` first to confirm
48/48 still passes. That's the gate before any change.

## Reference target — what we have to match

sce-cgc at `--O2 --fastmath` produces a 3-instruction (80-byte) ucode.
The pattern equals what it produces for the equivalent flat
`acc += vcol.x; acc += vcol.x; acc += vcol.x;` shape — confirmed in
the previous session by probing both forms.

Reference ucode bytes (offset 0xc0 in the .fpo container):

```
22800140 c8011c9d c8000001 c8003fe1   ← inst 1 (16 B, no inline const)
1c000100 80021c9c c8000001 c8000001   ← inst 2 (32 B with inline const)
00000000 00003f80 00000000 00000000     inline const for inst 2  → (0.0, 1.0, 0.0, 0.0)
02010400 c9001c9d 00020000 c9000001   ← inst 3 (32 B with inline const)
00004000 00000000 00000000 00000000     inline const for inst 3  → (2.0, 0.0, 0.0, 0.0)
```

Apply `halfswap = (w >> 16) | (w << 16)` to each dword to get the
logical bit layout used by `nvfx_shader.h`'s `NVFX_FP_OP_*` defines.
Reference SDK keys:

- `inst 1` opcode = 0x01 = **MOV**, OUT_REG_HALF = 1, OUTMASK = X,
  INPUT_SRC = COL0  →  `MOVH H0.x = f[COL0].x` (preload vcol.x as fp16).
- `inst 2` opcode = 0x01 = **MOV** with inline const block. The const
  has 1.0 in lane Y. Most likely shape is a destination-mask MOV that
  loads pieces of `(acc, 0, 0, 1)` into R0 — but decoding the dst mask
  + swizzle is the first thing to verify.
- `inst 3` opcode = 0x04 = **MAD** with inline const block. Const has
  2.0 in lane X. Most likely shape: `MAD R0.x = H0.x * 2.0 + H0.x`,
  producing `3 * vcol.x` in R0.x.

So sce-cgc is doing `0 + x + x + x = 3*x` algebraic fold AND lowering
`3*x` as a `MAD x*2.0, x` rather than as a `MUL x, 3.0`. Both transforms
are needed for byte-match.

Decode the bytes properly with the existing tooling before trusting
those guesses — `tools/rsx-cg-compiler/build/rsx-cg-compiler --dump-ir
--profile sce_fp_rsx --O2 --fastmath <shader>.cg` shows our IR; for
sce-cgc bytes, halfswap each dword and apply the shifts in
`tools/rsx-cg-compiler/src/nv40/nvfx_shader.h`. Update
`tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md` with whatever you
find — it's the in-tree living doc for this kind of work.

## What's blocking byte-match — two layers

### 1. NV40 FP emitter only handles `(varying, uniform)` pairs

`tools/rsx-cg-compiler/src/nv40/nv40_fp_emit.cpp`, around line 700:

```
"nv40-fp: arithmetic ops supported only for (varying, uniform) pairs "
"(uniform+uniform / literal arithmetic lands later)"
```

Anything in the IR that becomes `temp + uniform` / `temp + varying` /
`temp + temp` bails the emit path. Until the IR collapses chained adds
to a single MAD, the unrolled `acc += x; acc += x; acc += x` body
produces `Add(Add(Add(0, x), x), x)` — a chain the emitter can't lower.

### 2. No accumulator-style algebraic fold

`tools/rsx-cg-compiler/src/donor/ir/ir_passes.cpp` has
`AlgebraicSimplification` (handles `x + 0`, `x * 0`, `x * 1`, etc.) and
`InstructionCombining::tryCombineMulAdd` (`add(mul(a,b), c) → mad(a,b,c)`).
Neither matches the `x + x + x` chain shape. We'd need either:

- a new pass that recognises N copies of the same value being added
  and rewrites to `MUL(x, N.0f)` (which then participates in MAD-fusion
  if appropriate), OR
- the more general "arithmetic chain → MAD with literal coefficient"
  peephole sce-cgc clearly has.

The current `PassManager` is **dead code** (defined but never called
from `main.cpp`) — only a hand-wired `DeadCodeElimination` runs today.
You probably want to either wire `PassManager` properly or hand-wire
the new pass alongside DCE.

## Strategy

Two tracks. Don't merge them until both work in isolation; the
arithmetic-chain emitter work is independently useful even if
accumulator-fold byte-match doesn't land this session.

### Track A: NV40 emitter — N-ary arithmetic chain support

Extend `nv40_fp_emit.cpp`'s arithmetic case (the one that bails on
non-`(varying, uniform)` pairs) so it can emit:

- `temp + uniform`  → `ADDR Rn, prev_temp, c[inline]`
- `temp + varying`  → `ADDR Rn, prev_temp, f[INPUT]`
- `temp + temp`     → `ADDR Rn, prev_temp_a, prev_temp_b`

Each previous Add allocates a temp register; the emitter needs to track
which IR value lives in which Rn. Look at how the existing MAD-fusion
path handles temps (it allocates R1 for the MOVR preload and uses R0
as the MAD destination) — same allocation strategy applies, just
generalised.

Validation: a new test shader where the body is genuinely chained, e.g.

```cg
// chain_add_uniform_f.cg
void main(
    float4 vcol      : COLOR,
    uniform float4 a,
    uniform float4 b,
    uniform float4 c,
    out float4 color : COLOR)
{
    color = vcol + a + b + c;
}
```

sce-cgc should emit 3 sequential ADDR instructions (no folding because
the uniforms are runtime-patched). Get that to byte-match first; that
proves the chain support without entangling it with accumulator-fold.

### Track B: accumulator-fold pass

Once Track A is in, add an IR pass that recognises:

```
v0 = const 0.0
v1 = Add(v0, x)
v2 = Add(v1, x)
v3 = Add(v2, x)
... use v3 ...
```

and rewrites to `Mul(x, 3.0)` (then let MAD-fusion or peephole turn
that into the `MAD x*2.0 + x` form sce-cgc emits, if needed).

Implementation hint: walk the IR, for each Add chain of N copies of
the same SSA value, replace with `Mul(value, N.0f)`. Run after DCE +
CopyPropagation so the chain is already in canonical form.

If the resulting `MUL x, 3.0` doesn't byte-match sce-cgc's
`MAD x*2.0 + x`, decide whether to:

- add a peephole that lowers `MUL(x, K)` for small K to MAD-equivalent
  forms, OR
- accept `MUL` as functionally equivalent and skip the
  `loop_static3_f` test (only worth if it's not worth the engineering
  to match exactly — but byte-match is the project gate, so probably
  the peephole).

### Suite gate

48/48 stays. Add `loop_static3_f.cg` back (the previous session
removed it) once it byte-matches. Add `chain_add_uniform_f.cg` (or
similar) as the Track A gate. Target 50/50.

## House rules (still apply)

- Byte-exact gate via `tests/run_diff.sh` before commit
  (`feedback_byte_exact_shader_output`).
- No vendor name in tracked content (`feedback_no_vendor_name_tracked`);
  RE notes go to memory or gitignored paths
  (`feedback_re_docs_out_of_repo`).
- Reference SDK at `reference/sony-sdk/` is a coverage oracle, never
  copied into tracked content.

## What to report at the end

Whether the suite hit 50/50 (or more), what shape the chain emitter
took (which temp-allocation strategy you settled on), and whether
accumulator-fold ended up being a single pass or split across multiple.
If you discover the reference compiler's MAD form has a non-obvious
encoding twist (constant placement, swizzle quirk, …), capture it in
`tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md`.

If you hit a new blocker — e.g. the chain emitter requires a register
allocator pass we don't have, or accumulator-fold breaks an existing
shader you can't easily fix — pause and report rather than burn an
hour on the wrong fix.
