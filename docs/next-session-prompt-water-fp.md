# Next session — close the Water-sample shader gaps

We made the rsx-cg-compiler match the reference compiler byte-for-byte
on three patterns the older SDK tutorial Water shaders rely on:
file-scope `float4x4 gMtx;` (VP), file-scope vec4 / sampler2D /
samplerCUBE uniforms (FP), and the `float4 main(...) : COLOR { return
... }` value-return-with-semantic shape (FP).  Three of the six Water
shaders (`VertexProgram.cg`, `SimpleVertex.cg`, `RenderSkyBoxVertex.cg`)
now build through our compiler instead of falling back to wine.

The remaining three are FP shaders — `RenderSkyBoxFragment.cg`,
`SimpleFragment.cg`, and `FragmentProgram.cg`.  They still fall back
to wine because of three patterns we don't lower yet.  This session
attacks all three, plus authors a runnable PPU sample built around
the patterns we already cover.

## What ships in HEAD already (reference for context)

- `tools/rsx-cg-compiler/tests/run_diff.sh` runs **76 / 78** byte-exact
  vs the reference compiler.  The two pre-existing fails
  (`alpha_mask_split_write_f`, `mul_chain4_v`) are out of scope; do
  not pull on those threads first.
- 4 new byte-exact test shaders that are also clean-room samples:
  - `global_mvp_v.cg` — VP file-scope `float4x4` + struct return
  - `global_vec4_mul_f.cg` — FP file-scope `float4` uniform + value
    return with `: COLOR` semantic
  - `global_sampler_f.cg` — FP file-scope `sampler2D` + `tex2D`
  - `global_samplercube_f.cg` — FP file-scope `samplerCUBE` + `texCUBE`
- Compiler errors now exit 1 (was silently exit 0 with a "skeleton
  stage" message — the shim could not detect the miss).
- `nv40_fp_emit.cpp` handles `IROp::LoadUniform` (= IR op #84) by
  routing samplers to `valueToTexUnit` and non-samplers to
  `valueToFpUniform` with slot index ≥ `entry.parameters.size()`.
  `cg_container_fp.cpp` walks `module.globals` after the entry-param
  loop and emits matching param entries (one per global, plus the
  synthetic `<entry>` output param for value-return shaders).
- IR builder promotes file-scope `StorageQualifier::None` → `Uniform`
  in `buildGlobals`, so the container + emitter see top-level
  globals as uniforms (Cg's implicit rule).

## Gotchas / behaviour we already pinned down

These bit us during this session — keep them in mind so the next
session does not re-discover them the hard way:

1. **`set -euo pipefail` in `tests/run_diff.sh` exits early on the
   first FAIL**, which masked the 4 new passing tests behind two
   pre-existing fails.  When debugging a regression: temporarily
   patch to `set -uo pipefail` to see the full board, then revert.
2. **The reference compiler orders multi-output passthrough VP
   stores by *source attribute index*, not output register index
   and not source-statement order.**  See the `attrIndexFromValue`
   helper in `nv40_vp_emit.cpp`.  For VecConstruct / shuffle sources
   we walk through to find the underlying input ATTR.  Stores with
   no traceable input-attr source fall back to output index +1024.
3. **Struct returns: emit StoreOutputs at `return` time in
   struct-field declaration order, NOT inline at member-assignment
   time.**  We special-case "local var returned by `return X;`" in
   `buildAssignment` (skip inline emit) and re-emit in
   `buildReturnStmt` walking the struct fields.  For `out`-keyword
   parameter struct fields the inline emit still fires (no return
   statement to anchor).
4. **Synthetic struct outputs need their source-level field type on
   the StoreOutput's `resultType`** so the container emits a
   correctly-sized param entry (`float2 texCoord` → kCgFloat2, not
   kCgFloat4).  The IR builder stashes this in
   `buildReturnStmt`'s synthesised StoreOutput.
5. **`isShared = (g.explicitRegisterBank == 'C') ? 1 : 0`.**  File-
   scope uniforms with no `: register(CN)` annotation get
   isShared=0; ones with the annotation get isShared=1.  Do not
   blindly hard-code `isShared = 1` for globals like the original
   struct-flat path did — that was a bug we fixed.
6. **`isStructFlattened` triggers only when an entry param is `void`-
   typed.**  Water VP shaders with a non-struct input + struct
   return go through the regular path, not the struct-flat path —
   that's why the regular path now also emits file-scope uniform
   entries and synthetic struct outputs.
7. **The reference compiler's `.fpo` runtime patches an embedded-
   constant block per uniform use.**  For each `valueToFpUniform`
   slot, the back-end appends a 16-byte zero-initialised block
   directly after the consuming instruction and records the
   ucode-byte offset in `attrs.embeddedUniforms[slot].ucodeByteOffsets`.
   This already works for entry-param uniforms; it works for
   top-level uniforms too because we allocate `slot ≥
   entry.parameters.size()` and the existing block-append code
   reads from `valueToFpUniform[srcId]`.
8. **`tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md` is the
   living doc** for binary-format discoveries.  Update it (and the
   `feedback_byte_exact_shader_output` memory) when you pin down
   new layout details — do not write tracked RE narrative outside
   of `docs/REVERSE_ENGINEERING.md`.
9. **No "sce-cgc" by name in tracked source/comments.**  Use "the
   reference compiler" / "the original Cg compiler" — see memory
   `feedback_no_sce_cgc_by_name`.
10. **Repro the Water sample** at
    `~/sony-build/cell-sdk-shim/samples/tutorial/CgTutorial/GCM/Water/`.
    The shim's `sce-cgc` shell wrapper falls back to wine
    `sce-cgc.exe` only when our compiler emits an empty file —
    since the exit-1 fix in this session, the shim's `[[ ! -s
    "$out" ]]` check fires correctly on emit failures.

## Targeted plan — all three workstreams in parallel

Each subsection is independent.  Open them as three feature
branches; merge as each goes byte-exact.

### A. FP pure literal/uniform constant folding

**Goal:** `const float3 frequency = 2.0 * 3.14159265358979323846 /
waveLength;` (with `waveLength` itself a `const float3` literal init)
folds at compile time so the back-end never sees the divide /
multiply on it.  Same for `const float3 phase = speed * frequency;`
and the `cos(frequency * Length - phase * gTime)` chain inside
`FragmentProgram.cg`.

**Approach:**

1. Add a const-eval pass that runs *after* the IR builder and
   *before* `nv40_fp_emit` lowers anything.  Live in a new
   `tools/rsx-cg-compiler/src/donor/ir/ir_const_fold.cpp`.  Walk
   each function once and rewrite instructions whose every operand
   resolves to an `IRConstant` into a single `IRConstant` of the
   folded value.  Fold the obvious arithmetic (Add / Sub / Mul /
   Div / Neg / Mad), `VecConstruct` of literals, `VecShuffle` /
   `VecExtract` of constant vectors, and the unary math
   (Abs / Sqrt / RSqrt — already half-implemented in
   `nv40_fp_emit`).
2. **Follow the front-end's existing `const`-tracking.** Locals
   declared `const float3 X = float3(...);` already get marked
   `StorageQualifier::Const` — see `parser.cpp:996`.  In
   `buildDeclStmt`, when the storage is `Const` and the initializer
   evaluates to an `IRConstant`, route through a new
   `nameToConstant_` map (parallel to `nameToValue_`) so that
   subsequent `buildIdentifierExpr` returns an `IRConstant` SSA
   value rather than a `LoadUniform`.  This is the cheapest path to
   get the canonical pattern working.
3. Fold cross-uniform expressions: when an `Add` / `Mul` / `Div`
   has one operand that's an `IRConstant` and the other a
   `LoadUniform` of a *file-scope `const`-initialised* global, treat
   the global itself as a literal (its init expression must be
   constant-foldable).  Do this only when the global is `Const`-
   storage; we must keep `Uniform`-storage globals as runtime
   uniforms.
4. Add a test shader `tests/shaders/literal_arith_const_f.cg`
   exercising:
   ```cg
   float4 main(float4 vcol : COLOR) : COLOR {
       const float3 wave = float3(2.0, 1.2, 0.8);
       const float3 freq = 2.0 * 3.14159 / wave;
       return float4(vcol.rgb * freq, 1.0);
   }
   ```
   The reference compiler folds `freq` into a literal-pool slot;
   our emit should match byte-for-byte.

**Watch out for:**
- Fast-math affects fold semantics — we always run with
  `--O2 --fastmath` (see `OPT_FLAGS` in `run_diff.sh`).  Match the
  reference compiler's NaN / inf handling under `--fastmath` (it's
  permissive — feel free to fold IEEE special cases).
- `const` storage at *file scope* gets the implicit-uniform promotion
  in `buildGlobals`.  Suppress the promotion for `Const` storage:
  ```
  if (varDecl->storage == StorageQualifier::None)
      varDecl->storage = StorageQualifier::Uniform;
  ```
  should become a check that doesn't clobber existing `Const`.
- Folded literals need to go through the same `LiteralPool` path the
  back-end already uses for `mad_literal_f` / `branch_literal_f` —
  do not invent a new slot mechanism.

### B. Water FP shaders — three remaining patterns

`RenderSkyBoxFragment.cg` is the smallest Water FP shader and the
right starting point.  After it lands, `SimpleFragment.cg` adds the
`if`/`else` shape, and `FragmentProgram.cg` is the kitchen sink.

#### B1. Swizzled-lane write-back: `oColor.a = 1.0f;`

`RenderSkyBoxFragment.cg`:
```cg
float4 color = texCUBE(gSkyBoxCubeMap, worldPos.xyz);
color.a = 1.0f;
return color;
```

The pattern is "compute a vec4 into a local, override one lane,
return".  Today our `buildAssignment` only handles member assign
to a struct (looks up `getStructFields`).  For built-in vectors the
member is a swizzle (`.a` / `.w`), not a struct field, so the assign
falls through unhandled.

**Approach:**
1. In `buildAssignment`, when the LHS is `MemberAccessExpr` whose
   object's type is a vector and the member is a single-lane swizzle
   (`x` / `y` / `z` / `w` / `r` / `g` / `b` / `a`), emit a
   `VecInsert` op against the previous SSA value of that variable
   and update `nameToValue_[name]` to the result.  The IR already
   has `IROp::VecInsert`; nothing in `nv40_fp_emit.cpp` consumes
   it yet, so add a lowering: `VecInsert` of a literal lane into
   a known vec4 source emits a one-lane MOV against the dst temp
   with the writemask narrowed to that lane.
2. The reference compiler folds `oColor = float4(rgb, 1.0)` into
   the prior op's writemask + a tail MOV with literal 1.0 in the W
   lane.  Match that — when the StoreOutput consumer of a
   VecInsert sees `lane == w` and `value` is the literal `1.0`,
   emit a single MOV writing the .w lane only with literal-pool 1.0,
   leaving the prior op's RGB write intact.

**Test shader to add:** `tests/shaders/lane_w_literal_set_f.cg`:
```cg
sampler2D gTex;
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float4 c = tex2D(gTex, uv);
    c.a = 1.0f;
    return c;
}
```

#### B2. `if (gFunctionSwitch != 0.0) { ... }` — runtime-uniform branch

`SimpleFragment.cg`:
```cg
float gFunctionSwitch;
float4 main(...) : COLOR0 {
    float4 fragColor = color;
    if (gFunctionSwitch != 0.0) {
        fragColor = fragColor * tex2D(gBaseTextureMap, texCoord);
    }
    fragColor.a = 1.0;
    return fragColor;
}
```

The current FP emit has a Select-based path for if/else over
varying scalars (see `feedback`-history note `alpha_mask_tex_lhs` and
the `lane_w_select_f` family of tests).  The Water case is a
**uniform-scalar** comparison driving a multi-instruction THEN
block whose body modifies a local that's also read in the
ELSE-or-fallthrough.

**Approach:**
1. The reference compiler lowers `if (uniformBool) ...` differently
   from a varying conditional — the uniform value patches at runtime
   so it's effectively a branch the GPU resolves once per draw.
   Investigate whether `--fastmath` lets us collapse this to a
   `MULR R0, predicate, branchValue + ADDR R0, R0,
   inverse_predicate * fallthroughValue` pattern (a select-style
   blend).  Pull the `--debug` ucode listing from the reference
   compiler and compare against our Select lowering.
2. If the reference compiler emits real BRA/CAL flow (NV40 FP has
   IF/ELSE/ENDIF opcodes — see `nvfx_shader.h`), wire that through
   `nv40_fp_emit`.  Existing `nv40_if_convert.cpp` does CPU-side
   if-conversion for the varying case; the uniform case might
   straight-emit IF/ELSE/ENDIF.
3. Track `nameToValue_` updates inside the THEN block; on merge,
   either Phi (if real branches) or Select-blend (if collapsed).
   This is exactly what `tests/shaders/if_else_f.cg` exercises for
   varyings — extend the same shape to uniforms.

**Test shader to add:**
`tests/shaders/uniform_branch_modify_f.cg` — minimal repro of the
SimpleFragment pattern with one `if (uniform != 0)` block.

#### B3. Vector-math intrinsics: `length` / `normalize` / `reflect` / `refract` / `cos`

`FragmentProgram.cg` is the kitchen sink.  Current state:

- `IROp::Length` and `IROp::Normalize` are partly lowered already
  (see `valueToLength`, `valueToNormalize` in `nv40_fp_emit.cpp`)
  but only for the `length(varying)` shape.  Extend to vec3 ops
  on results of arithmetic chains.
- `IROp::Reflect` / `IROp::Refract` have no FP lowering.  Both
  unfold into 4-5 NV40 FP instructions:
  - `reflect(I, N) = I - 2.0 * dot(N, I) * N` — DP3 + MUL + MAD.
  - `refract(I, N, eta)` is longer — see the Cg standard library
    expansion; the reference compiler likely matches that exactly.
- `IROp::Cos` (and Sin) have NV40 FP opcodes (COS/SIN); wire them
  up the same way as RSQ in the existing path.

**Approach:**
- Walk through `FragmentProgram.cg` op-by-op against the reference
  compiler's `--listing` output (`sce-cgc --listing FragmentProgram.cg`
  if the wine wrapper supports it; otherwise dump the .fpo and
  disassemble with `sce-cgcdisasm`).  Build up tests in
  `tests/shaders/` one intrinsic at a time:
  - `tests/shaders/cos_varying_f.cg`, `sin_varying_f.cg`
  - `tests/shaders/reflect_f.cg` — varying-only operands
  - `tests/shaders/refract_f.cg`
  - `tests/shaders/normalize_subexpr_f.cg` — `normalize(a - b)`
- Get each one byte-exact.  After all four land,
  `FragmentProgram.cg` should be in reach — if not, the remaining
  delta will be obvious from the diff.

**Watch out for:**
- The Water FP shader uses `mul(matrix, vec4)` *inside* the fragment
  shader (not just VP).  The FP MatVecMul lowering does not exist
  yet — it's a 4-DP4 sequence into a temp.  Treat as a separate
  test (`mat_vec_mul_f.cg`) before integrating.
- Operand width: many of these intrinsics take vec3 in the source
  but `mul`/`tex2D` produce vec4.  Watch for narrow-write masks.

### C. Runnable PPU sample exercising the new patterns

The 4 new test shaders are standalone clean-room templates but
they don't yet ride in a runnable PPU sample.  The user explicitly
asked for one — let's land it.

**Approach:**

1. Clone `samples/gcm/hello-ppu-cellgcm-textured-cube` to
   `samples/gcm/hello-ppu-cellgcm-global-uniforms`.  Keep the C
   source structure (flip protocol, vertex array setup, etc.) —
   only the shaders change.
2. Replace the shaders with file-scope-global versions:
   ```cg
   // vpshader.vcg
   float4x4 modelViewProj;
   struct VOUT {
       float4 hpos     : POSITION;
       float2 texcoord : TEXCOORD0;
   };
   VOUT main(float3 in_position : POSITION, float2 in_texcoord : TEXCOORD0) {
       VOUT o;
       o.texcoord = in_texcoord;
       o.hpos     = mul(modelViewProj, float4(in_position, 1.0f));
       return o;
   }
   ```
   ```cg
   // fpshader.fcg
   sampler2D tex;
   float4 main(float2 in_texcoord : TEXCOORD0) : COLOR {
       return tex2D(tex, in_texcoord);
   }
   ```
3. PPU side: the existing `cellGcmCgGetNamedParameter(vpo,
   "modelViewProj")` and `cellGcmCgGetNamedParameter(fpo, "tex")`
   already resolve by name — file-scope vs entry-param scope is
   transparent at the runtime API.  No `main.c` changes needed.
4. Build green via `cmake --build` from
   `samples/gcm/hello-ppu-cellgcm-global-uniforms/cmake-build/`.
5. Boot in RPCS3, confirm the cube renders the same as
   `hello-ppu-cellgcm-textured-cube`.  Both samples staying in tree
   gives us a side-by-side "with-keyword vs without-keyword"
   comparison.
6. Update `samples/README.md` (if it exists) to list the new
   sample and what it demonstrates.

**Watch out for:**
- Do not touch the existing `hello-ppu-cellgcm-textured-cube` —
  it's the canonical "uniform-keyword + out-keyword" reference.
  The new sample is the "implicit-uniform + struct-return"
  reference.  Both should ship.
- `ps3_add_cg_shader_rsxcgc` is the right CMake function (uses our
  rsx-cg-compiler path).  Do not switch to `ps3_add_cg_shader`
  (legacy cgcomp) — see existing samples' commented sibling lines.
- Test that `make_fself` + RPCS3 render with the same texture and
  matrix as `textured-cube`.  If it crashes, the most likely cause
  is the runtime patching the embedded-uniform block at the wrong
  offset — `cellGcmSetVertexProgramConstants` for matrices,
  `cellGcmSetFragmentProgramConstant` for non-matrix uniforms,
  and `cellGcmSetTextureControl`/`cellGcmSetTexture` for samplers.

## Pre-existing 2 failing tests — opportunistic fix

These two have been red since before this session.  They're worth
including because **(B1) and (B3) above are likely to unblock both
of them as a side-effect** — leaving the harness 78/78 with no
extra targeted work.

### `alpha_mask_split_write_f` (FP)

Source (`tests/shaders/alpha_mask_split_write_f.cg`):

```cg
void main(float4 color : COLOR0, float2 texcoord : TEXCOORD0,
          uniform sampler2D texture,
          out float4 oColor : COLOR0)
{
    float alpha = tex2D(texture, texcoord).w;
    if (alpha <= 0.5f) {
        oColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        oColor.rgb = color.rgb;
        oColor.a = color.a;
    }
}
```

Failure shape: `nv40_fp_emit` returns empty ucode (our emitted
output is the empty string in the harness diff).  The diagnostic
trail through the harness logs is `nv40-fp: unsupported IR op #78`
(= `IROp::CondBranch`) — our FP emit has no `case IROp::CondBranch`,
so the if/else lowering aborts before producing any ucode.

The blocker is **the same lane-write pattern** as workstream **B1**
(`oColor.rgb = ...; oColor.a = ...;`).  Once `VecInsert` lowering
is wired through, the front-end stops needing the multi-step path
and the if-conversion pass (`nv40_if_convert.cpp`) can fold the
whole thing.  After (B1) lands, re-run the harness — this test
will likely flip to green without targeted effort.

If it doesn't auto-resolve, the remaining work is to teach the FP
emit's existing varying-scalar Select / PredCarry path to handle
the **vec4-literal-zero THEN branch + split-lane ELSE branch**
shape (companion to `alpha_mask_tex_f.cg` which is green and uses
the simpler "single-value ELSE" shape).

### `mul_chain4_v` (VP)

Source (`tests/shaders/mul_chain4_v.cg`): four chained
`mul(matrix, vector)` against four distinct file-scope-uniform
matrices, plus per-component colour MADs.

Failure shape: container diverges at the CgBinaryVertexProgram
subtype block (offset `0x620`).  Decoded:

| Field            | ours | sce | Notes |
|------------------|-----:|----:|-------|
| numInstructions  | 0x11 = 17 | 0x13 = 19 | sce emits 2 more ucode words |
| registerCount    | 2 | 3 | sce allocates one more temp |

The 2 extra ucode words and 1 extra temp register suggest sce-cgc
**does not collapse the `R0 → R1 → R0` ping-pong all the way down
the chain**.  Our chain logic (see `nv40_vp_emit.cpp` ~line 1232,
"DP4 path #3: vector operand is a previous MatVecMul's result")
recycles the source temp aggressively (`availableTemps.push_back`)
while the reference compiler keeps a fresh temp for one extra step
in the chain.

Targeted fix:
1. Disassemble both `.vpo`s side-by-side
   (`sce-cgcdisasm` on the wine-built reference, our `--listing`
   if it dumps the per-instruction breakdown).
2. Find the chain step where the temp counts diverge — likely the
   step that produces the final vec4 fed into the last mul, where
   sce keeps the value in a fresh temp instead of recycling.
3. Tighten the recycling heuristic in `nv40_vp_emit.cpp` to skip
   recycling on the *last* chain step (or whatever the
   reference-compiler-matching rule turns out to be).

This is an isolated fix; do not let it block A/B/C.  If the diff
is too obscure to land in this session, leave it red and update
the memory `project_rsx_cg_compiler_vp_features` with the finding.

## Workstream ordering

Recommended order of attack:

1. **(B1) lane-write `c.a = 1.0f`** — small, unblocks
   `RenderSkyBoxFragment.cg` (one of the three remaining Water FP
   shaders).  Land its byte-exact test first, then verify
   RenderSkyBoxFragment lands.
2. **(C) sample wrap-up** — ride on the patterns we already cover.
   Cheap to land in parallel.
3. **(B2) uniform-conditional if/else** — medium difficulty;
   unblocks `SimpleFragment.cg`.
4. **(A) const-fold pass** — medium-large; partially unblocks
   `FragmentProgram.cg` (kills its uniform-arithmetic init lines).
5. **(B3) intrinsics (cos/reflect/refract/etc.)** — biggest;
   needed for `FragmentProgram.cg` to fully land.

After (1) and (2): two of three Water FPs compile, plus a runnable
demo.  After (3) and (4): the remaining FragmentProgram.cg either
compiles or gives a focused next-session backlog.

**Stretch (no extra work):** (B1) is likely to flip
`alpha_mask_split_write_f` green for free (same lane-write
pattern), bringing the harness from 76/78 to 77/78.  Confirm with
a harness run after (B1) ships; if it doesn't auto-resolve, see the
"Pre-existing 2 failing tests" section above for the targeted
follow-up.

## Repro for the Water sample (unchanged from prior session)

```bash
source ~/source/repos/PS3_Custom_Toolchain/scripts/env.sh
cd ~/sony-build/cell-sdk-shim/samples/tutorial/CgTutorial/GCM/Water
CELL_SDK=$HOME/sony-build/cell-sdk-shim make             # builds Water.ppu.elf + .self
$PS3DEV/bin/sprxlinker Water.ppu.elf
$HOME/sony-build/cell-sdk-shim/host-linux/bin/make_fself \
    Water.ppu.elf Water.ppu.self
rpcs3 --no-gui Water.ppu.self
```

`docs/next-session-prompt-water-sample.md` (the prior session's
prompt) still has the open-gaps list for the Water *runtime* boot
issue (`_sys_lwmutex_lock` CELL_ESRCH).  That's a separate
workstream from the shader work — keep it on the backlog but do
not let it block any of A/B/C above.
