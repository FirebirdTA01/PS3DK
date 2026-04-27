# Next-session prompt — dbgfont_fpshader byte-match (tex-result-as-compare-LHS + multi-insn if-else)

Paste this as the opening user message of a fresh Claude Code session in
the PS3 Custom Toolchain repo. Self-contained — no prior context needed
beyond what's checked into the repo and `~/.claude/projects/.../memory/`.

---

The previous session landed the implicit-varying-binding fix
(`Semantic.inferred` flag, fragment-stage TEXCOORD<N> default with
skip-already-used). `bash tools/rsx-cg-compiler/tests/run_diff.sh`
should report **66/66 byte-match** and `clear_fpshader.cg` (from the
reference SDK's `fw` framework) compiles cleanly via our compiler.

The other shader the EMP engine sample needs is `dbgfont_fpshader.cg`
— alpha-masked text rendering. It still fails:

```
nv40-fp: unsupported IR op #78
```

Op 78 is `CondBranch` — the `brc` IR that the if-convert pass should
have folded away. Source:

```cg
void main(
    float2 texcoord : TEXCOORD0,
    float4 color    : COLOR,
    uniform sampler2D texture,
    out float4 oColor : COLOR)
{
    float4 tex = tex2D(texture, texcoord);
    if (tex.a <= 0.5) { oColor = float4(0,0,0,0); }
    else              { oColor = color;           }
}
```

`KNOWN_ISSUES.md → "FP texture-result feeding compare / arithmetic"`
has the three coupled gaps; this prompt expands them with
implementation hints.

## Reference target — what we have to match

3-instruction ucode, 64 bytes, captured at
`/tmp/dbgfont_ref.fpo` (from the previous session's probe — re-probe
to confirm):

```
0x140: 9002 1700 c801 1c9d c800 0001 c800 3fe1   ← inst 1 (16B): TEX
0x150: 037e 4c00 fe04 1c9d 0002 0000 c800 0001   ← inst 2 (32B): SLE-CC
0x160: 0000 3f00 0000 0000 0000 0000 0000 0000   ←   inline {0.5,0,0,0}
0x170: 3e01 0100 c801 0009 c800 0001 c800 3fe1   ← inst 3 (16B): MOV (END)
```

Half-swap each dword (`(w >> 16) | (w << 16)`) to get the logical
NV40-FP word layout used by `tools/rsx-cg-compiler/src/nv40/nvfx_shader.h`.

- **inst 1**: `TEX R0, f[TEX0], TEX0`. opcode = 0x17 (TEX).
- **inst 2**: opcode 0x0c (SLE) with bit 30 set (`NV40_FP_OP_OUT_NONE`)
  and bit 8 (`NVFX_FP_OP_COND_WRITE_ENABLE`). LHS comes from a temp
  reg (R0 from inst 1's TEX output, `.w` swizzled). RHS is the inline
  literal `{0.5, 0, 0, 0}.x`.
- **inst 3**: opcode 0x01 (MOV) writing R0, conditional via the CC
  set by inst 2. Bit 0 (END) is set. Decode the source word
  (`0x0009c801` upper half / `0xc801` lower half) to confirm it reads
  `f[COL0]` and figure out which CC predicate (`GT.x`?) the conditional
  fires on. Update `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md`
  with the decoded fields.

Conceptually: `R0 = tex2D(texture, texcoord); CC = (R0.w <= 0.5);
R0(GT.x) = f[COL0]`. The output buffer at the end is whatever came out
of the conditional MOV — when `tex.a <= 0.5` the conditional doesn't
fire and R0 stays = tex2D result, which is *wrong* relative to the
source's "= float4(0)" semantics.

So either:
1. The reference compiler relies on an invariant about tex2D + alpha
   in this dbgfont context (R0.w >= 0 always, so the literal-vec4
   path is selected differently), OR
2. inst 3 is reading something other than COL0 (e.g. a constant slot
   that holds 0,0,0,0 and the predicate is inverted), OR
3. There's encoding nuance the half-swap interpretation has missed.

**This is the first thing to nail down.** Don't proceed to coding
until the inst 3 source + predicate is decoded and you can describe
what the 3-instruction sequence actually computes.

## Implementation tracks

Once the ucode is decoded, three tracks need to land before
`tests/run_diff.sh` can include `dbgfont_fpshader.cg`:

### Track A — IR shape: hoist if-convert, fold VecConstruct-of-consts

`tools/rsx-cg-compiler/src/nv40/nv40_if_convert.cpp` rejects THEN/ELSE
arms with > 2 instructions (`matchSingleStoutThenBranch` requires
exactly `[stout; br]`). dbgfont's THEN arm is `[vec %14 ...; stout
%14; br]` because `float4(0,0,0,0)` builds a `VecConstruct` from four
const operands.

Two ways to fix:

1. Run a const-fold pass *before* if-convert that collapses
   `VecConstruct(const, const, const, const)` into a single
   constant value. Cleanest — also useful for other shapes.
2. Extend `matchSingleStoutThenBranch` to allow N "hoistable"
   instructions before the stout, hoisted into entry by the
   rewrite. A hoistable op is purely computational (no side effects,
   no IO); restrict to `VecConstruct`, `VecExtract`, `VecShuffle`
   for now.

Pick the one that's least invasive. (1) is probably better long-
term but (2) is narrower. The harness gate stays 66/66 either way
until track B + C land — adding `dbgfont_fpshader.cg` as a test
shader should be the last step.

### Track B — `CmpBinding`: tex-sample results as compare LHS

`nv40_fp_emit.cpp` lines 566-626 record a `CmpBinding` only when
`valueToScalarExtract` resolves to a `valueToInputSrc` (a varying
read). Tex-sample results live in `valueToTex` — they get silently
dropped at line 579.

Extend the comparison handler to also accept LHS = scalar-extract
of a `valueToTex` entry. Add a new `CmpBinding::LhsKind` enum
(`Varying` vs `TexResult`) and route the LHS source accordingly.

### Track C — Select emit: tex-LHS + literal-vec4 + varying

`nv40_fp_emit.cpp`'s Select StoreOutput path handles literal-vec4 +
varying branches today (line 1742-1796), but the cmp-side LHS path
is varying-only. Add a third schedule (alongside the existing
"varying-RHS" and "uniform-RHS" paths):

- LHS = tex-temp register (e.g. R0 from a preceding TEX emit)
- RHS = literal scalar
- trueBr = literal vec4 OR varying
- falseBr = the other

The emit shape needs to match the 3-instruction form decoded above.
Likely: TEX (already emitted) + SLE-CC + conditional MOV.

`texCoords2D` + `texCoordsInputMask` bookkeeping:
- TEXCOORD0 IS used as a 2D tex2D coord here, so `texCoords2D` bit 0
  stays SET (unlike the implicit-varying case).
- `texCoordsInputMask` bit 0 set as usual.

Drop in a focused test shader once each track lands so you can
run `run_diff.sh` between tracks.

## Suite gate

Stay at 66/66 until track A + B + C are all green and
`dbgfont_fpshader.cg` byte-matches. Target 67/67.

If track A surfaces unrelated regressions in existing if-else
shapes, **stop and report** rather than chasing them — the existing
`if_else_f.cg` / `if_only_f.cg` tests should never regress.

## House rules (still apply)

- Byte-exact gate via `tests/run_diff.sh` before commit
  (`feedback_byte_exact_shader_output`).
- No vendor name in tracked content (`feedback_no_vendor_name_tracked`);
  RE notes go to memory or gitignored paths
  (`feedback_re_docs_out_of_repo`).
- No external-project references (`feedback_no_external_project_refs`)
  — describe the alpha-mask pattern, not which downstream consumer
  needs it.
- Reference SDK at `reference/sony-sdk/` is a coverage oracle, never
  copied into tracked content.
- Don't commit without asking (`feedback_dont_commit_without_asking`).

## What to report at the end

Whether 67/67 byte-match held, the decoded shape of inst 3
(source + predicate), and which track-A approach (const-fold pass
vs hoist-extension) you took.

If you hit a new blocker — e.g. the conditional-MOV's source can't be
decoded, or it turns out the reference compiler emits a different
shape at `--O2 --fastmath` than at `--O3` — pause and report rather
than burn an hour on the wrong fix.
