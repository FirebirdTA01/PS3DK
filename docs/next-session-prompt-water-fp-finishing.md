# Next session — close the last Water FP shader + 86/86 harness

This prompt picks up where `docs/next-session-prompt-water-fp.md`
left off after the 2026-04-30 session.  Five of the six Water-
sample shaders are byte-exact through our compiler; the harness
runs **84 / 86 byte-exact**.  The only remaining gaps:

- `FragmentProgram.cg` (the kitchen-sink Water FP shader) still
  falls back to wine `sce-cgc.exe`.
- Two harness fixtures are red:
  - `tests/shaders/alpha_mask_split_write_f.cg` (FP)
  - `tests/shaders/mul_chain4_v.cg` (VP)

Both gaps land in a single push: drive `FragmentProgram.cg` to
byte-exact and the harness reaches **86 / 86**.  The two pre-
existing fails are **not** unblocked by FragmentProgram work — they
need targeted fixes you can run in parallel.

## What HEAD already does (don't re-derive)

Read `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md` for binary-
format details.  The mechanisms in place after 2026-04-30:

1. **B1 — FP swizzled lane write-back** (`c.a = 1.0f;`):
   `IROp::VecInsert` lowered in `nv40_fp_emit.cpp`.  StoreOutput
   peels VecInsert chains and narrows the underlying producer's
   writemask by `~(1 << lane)`.  Tail MOV per override with inline
   `{lit, 0, 0, 0}` block.  Tests: `lane_w_literal_set_f.cg`.
2. **B2 — FP uniform-conditional `if/else`**: IR builder
   `buildIfStmt` snapshots `nameToValue_` and inserts
   `Select(cond, thenVal, elseVal)` at merge blocks (proper SSA).
   `nv40_if_convert.cpp` Shape 4 hoists pure-THEN bodies into
   entry, drops the brc + then block, inlines the merge into
   entry.  `CmpBinding::LhsKind::UniformScalar` resolves direct
   `LoadUniform` LHS.  Select-StoreOutput dispatch lowers via the
   reference compiler's CC-blend (`MOV?C` + cond MUL) shape — bare
   (5-insn) vs lane-override (6-insn) variants.  Tests:
   `uniform_branch_modify_f.cg`, `uniform_branch_lane_set_f.cg`.
3. **A — IR-level constant folding**:
   `IRBuilder::tryFoldBinaryOp` / `tryFoldUnaryOp` /
   `tryFoldVecConstruct` + single/multi-lane swizzle fold in
   `buildMemberAccessExpr`.  Float scalars + float vectors with
   scalar→vector broadcast.  `nv40_fp_emit` pre-populates
   `valueToLiteralVec4` from any vec-typed IRConstant in
   `entry.values` so the existing literal-vec4 emit paths keep
   firing.  Test: `const_fold_vec4_f.cg`.
4. **C — runnable PPU sample**:
   `samples/gcm/hello-ppu-cellgcm-global-uniforms/` clones the
   textured-cube renderer with file-scope-uniform shaders +
   struct-return VP.  Verified rendering correctly in RPCS3.
5. **B3 — FP intrinsics**:
   - `cos` / `sin` — single NV40 FP COS (0x22) / SIN (0x23) insn
     direct from a varying lane (no MOVH preload like RCP/RSQ).
     Tests: `cos_varying_f.cg`, `sin_varying_f.cg`.
   - `reflect(I, N)` — 5-instruction lowering: `MOVR R0=N, MOVR
     R1=I, DP3R(2X) R0.w=2·dot, FENCBR, MADR R0=-R0·R0.wwww+R1`.
     Folds the constant `2 *` from `I - 2*dot(N,I)*N` into the DP3
     via DST_SCALE_2X (NV40 FP hw[2] bits 28-29).
     `VecConstruct(reflect, 1.0)` Shape A2 wraps with `wrapW1`
     mechanism (same as Normalize).  Test: `reflect_f.cg`.
   - `normalize(varying ± uniform)` — `FpNormalizeBinding::
     hasArith` captures the arith op; emit fuses the SUB into the
     R0 the DP3+DIVRSQR run over.  Test: `normalize_subexpr_f.cg`.
   - **NV40 DST_SCALE plumbing** in the FP assembler — `insn.scale`
     writes hw[2] bits 28-29 (NVFX_FP_OP_DST_SCALE_SHIFT = 28).
     Reusable for any future op needing hardware scaling.

## NV40 byte-encoding — warm intuition

The reverse-engineering doc has the field-by-field breakdown.
This section is the "feel" version — the patterns that matter
when you're hex-decoding sce-cgc output to design a new emit.

### Halfword swap

`FpAssembler` stores ucode words in **logical** layout, then
`words()` runs a halfword swap before disk write:
`halfswap(w) = (w >> 16) | (w << 16)`.  So an on-disk uint32
read big-endian (BE) gets halfswapped to recover the logical
encoding.

When decoding by hand:
1. Read 4 bytes BE from the ucode region → on-disk u32.
2. `halfswap(on-disk u32)` → logical word.
3. Decode logical word as `byte 0 = bits 0-7` (LSB), ... `byte 3
   = bits 24-31` (MSB).  Shift fields are bit positions in the
   logical word.

Const blocks (16-byte inline literals) get the same halfword
swap at write time.  So `0000 3f80` on-disk = `0x3f80_0000` =
1.0f as a logical/runtime value.  The four floats live at
half-words (`x` at logical bits 0-31, etc.) — when sce-cgc puts
the literal `1.0` in a specific lane, the encoded swizzle
`.xxxy` (or similar) reads from the right slot.

### Source slot layout (hw[1] / hw[2] / hw[3])

`nvfx_src` packs into 32 bits at hw[N+1] for SRC[N]:

```
bits 0-1   TYPE      0=TEMP, 1=INPUT, 2=CONST
bits 2-7   SRC index (R0..R63 / c[0])
bit  8     HALF      1=fp16 (H-temp)
bits 9-10  SWZ_X     X=0, Y=1, Z=2, W=3   (LSB at bit 9, MSB at 10)
bits 11-12 SWZ_Y     ... (encode at shift 11)
bits 13-14 SWZ_Z     ... (shift 13)
bits 15-16 SWZ_W     ... (shift 15) — straddles byte 1 / byte 2!
bit  17    NEGATE    per-source full negation
bits 18-20 COND      condition code (only on hw[1] for the per-insn cond)
bits 21-28 CC_SWZ    (X@21, Y@23, Z@25, W@27 — straddles bytes too)
bit  29    SRC0_ABS  (hw[1] only — bit 18 in hw[2] / hw[3] for SRC1/2_ABS)
```

**SWZ_W straddling matters.**  swz value 3 (W) needs both bit 15
and bit 16 set.  Identity `.xyzw` encodes byte 1 = `0xC8`, with
bit 16 (= bit 0 of byte 2) = 1.  If byte 2's bit 0 is 0, SWZ_W
reads as 1 (Y), not 3 (W) — common mis-decode.

### dst layout (hw[0])

```
bit  0     PROGRAM_END     stamped by markEnd() on the LAST emitted insn
bits 1-6   OUT_REG         R0..R63 (or 0x3F sentinel when OUT_NONE)
bit  7     OUT_HALF        1=fp16 (H register)
bit  8     COND_WRITE_ENABLE   sets CC bits from this insn's result
bits 9-12  OUTMASK         X=bit 9, Y=bit 10, Z=bit 11, W=bit 12
bits 13-16 INPUT_SRC       per-insn varying selector (POSITION=0,
                           COL0=1, COL1=2, FOGC=3, TC0..7=4..11,
                           FACING=14)
bits 17-20 TEX_UNIT        TEX/TXP/TXB/TXD use this; arith leaves it 0
bits 22-23 PRECISION       0=FP32, 1=FP16, 2=FX12
bits 24-29 OPCODE          MOV=0x01, MUL=0x02, ADD=0x03, MAD=0x04,
                           DP3=0x05, DP4=0x06, MIN=0x08, MAX=0x09,
                           SLT=0x0A...SEQ=0x0F, FRC=0x10, FLR=0x11,
                           KIL=0x12, TEX=0x17, TXP=0x18, RCP=0x1A,
                           RSQ=0x1B, EX2=0x1C, LG2=0x1D, COS=0x22,
                           SIN=0x23, DP2=0x38, NRM=0x39, DIV=0x3A,
                           DIVRSQ_NV40RSX=0x3B, FENCBR=0x3E
bit  30    OUT_NONE        (NV40-only) discard write — sentinel for
                           condition-only ops; OUT_REG = 0x3F when set
bit  31    OUT_SAT         saturate result to [0, 1]
```

### DST_SCALE in hw[2]

NV40 extension: bits 28-30 of **hw[2]** carry the destination
scale factor.  `NVFX_FP_OP_DST_SCALE_SHIFT = 28`.  Values:

```
0  1X      no scale (default)
1  2X      multiply result by 2     ← reflect() folds *2 here
2  4X      multiply by 4
3  8X      multiply by 8
5  INV_2X  divide by 2
6  INV_4X
7  INV_8X
```

The assembler emits this from `insn.scale` when non-zero (added
during the B3 reflect work — see commit history).

### Condition codes

When an insn has `cc_test = 1` and `cc_cond != TR`, the op only
writes its destination if the condition matches the CC bits in
the lanes selected by `cc_swz`.  Conditions:

```
0 FL  (false / never)
1 LT  (negative)
2 EQ  (zero)
3 LE  (≤ 0)
4 GT  (> 0)
5 NE  (≠ 0)
6 GE  (≥ 0)
7 TR  (true / always — default)
```

Predication is per-output-lane.  `cc_swz = (X,X,X,X)` makes every
output lane test CC.X.  `(W,W,W,W)` tests CC.W — used in the B2
SimpleFragment shape because the lane-W literal override claims
W.

### Inline const-block ucode-byte offsets

When an FP insn reads `c[0]`, the assembler emits the insn, then
appends a 16-byte block via `appendConstBlock(values)`.  The
block's ucode-byte offset (relative to the start of the ucode
region) is recorded in `attrs.embeddedUniforms[idx].
ucodeByteOffsets`.  The container's `CgBinaryEmbeddedConstant`
record points at this offset so the runtime patches the uniform
value into place at draw time.

For folded literals (numbers known at compile time), the block
holds the actual float bits and gets no `embeddedUniforms`
record — it's static.

**Lane placement matters.**  When the consumer reads `c[0].xxxx`,
the block's lane 0 is what feeds.  When the consumer reads
`c[0].xxxy`, lane 0 feeds X/Y/Z and lane 1 feeds W — common
encoding for the cos+lane-override pattern (`{0, 1.0, 0, 0}`
read with `.xxxy` → outputs.W = 1.0).  Decode the consumer's
swizzle to know which slot the literal needs to occupy.

### `wrapW1` mechanism

When an intrinsic that returns vec3 (Normalize, Reflect, …) gets
wrapped in `float4(intrinsic_result, 1.0f)` at the source level,
the IR has:

```
%a = <intrinsic> vec3 ...
%b = vec vec4 %a, %lit_1.0
stout %b COLOR
```

The VecConstruct case in the FP emit detects `vec4(intrinsic,
1.0)` shape and aliases `%b` → same intrinsic binding with a
`wrapW1 = true` flag.  StoreOutput emits the intrinsic's normal
sequence, then appends:

```
MOVR R0.w, c[0].xxxx     <inline {1.0, 0, 0, 0}>
```

This is one of two ways to handle `(vec3, scalar)` constructors —
the other is the lane-override / VecInsert peel mechanism.  Pick
`wrapW1` when the intrinsic's emit is fixed-shape; pick
VecInsert peel when the consumer's downstream logic needs to see
the scalar override as a separate IR step.

### ucode sizing

VP ucode quartets are 16 bytes (4 × u32).  FP ucode is also
16-byte units when including inline const blocks: an arith insn
that reads `c[0]` consumes 32 bytes total (insn + block).  The
container header at `ucodeSize` (`fpAttrs` for FP, vp container
header for VP) is the **byte count** including const blocks.
Instruction count = (ucodeSize - sum of const block sizes) / 16.

### Halfword swap caveat with sce-cgc dumps

When you `xxd /tmp/foo.fpo`, you see on-disk bytes.  To decode an
instruction, group 4 bytes into a BE u32, then halfswap:

```
on-disk: 8e 00 17 00 c8 01 1c 9d c8 00 00 01 c8 00 3f e1
word 0 BE:  0x8e001700
halfswap:   0x17008e00      ← logical hw[0]
opcode:     (0x17008e00 >> 24) & 0x3F = 0x17 = TEX
out_reg:    bits 1-6 of 0x00 = 0 → R0
mask:       bits 9-12 of 0x8e (byte 1) = 0x7 (XYZ)
input_src:  bits 13-16 = 0b0100 = 4 = TC0
```

If you skip the halfswap, opcodes look wrong (you'd read 0x8E
which is bit-reversed 0x71 — no such opcode).

### FragmentProgram.cg-specific encoding cheatsheet

These are the patterns I saw in sce-cgc's reference output for
the various Water FP shapes; capture them before you re-decode:

**`dot(varying3, varying3)` — the swizzle-pack quirk:**

When sce-cgc emits dot of two distinct varyings, NV40 FP can
only route ONE INPUT_SRC per insn, so it pre-loads each varying
into an R-temp.  The clever bit: the second varying loads to
`R1.xyw` (mask = 0xB, NOT 0x7) using src swizzle `.xyzz` —
packing `b.z` into R1.W.  The DP3 then reads R1 with src
swizzle `.xyW?` to pick up b.x at lane 0, b.y at lane 1, b.z
at lane 2 (now in W).  This avoids initializing R1.z which
would otherwise need a clear write.

```
MOV R0.xyz, f[TC_a]                 ; load a normally
MOV R1.xyw, f[TC_b].xyzz            ; pack b.z → R1.W
DP3 R0.xyzw, R0, R1.swz(X,Y,W,?)    ; .xyW? compensates
```

If you don't reproduce this exactly, expect the swizzle byte 1
of hw[2] to be `0x68` (matches `.xyW?`) instead of `0xC8`
(identity).

**`reflect(I, N)` register layout:**

R0 holds N (xyz only, mask 0x7).  R1 holds I (xyz only, 0x7).
R0.W gets clobbered by the DP3 result (mask = 0x8 / W only).
Final MAD reads R0.wwww as the multiplier, R0.xyz (now N) and
R1.xyz (I).  The key insight: N goes to R0 first because R0.W
is the natural "scratch lane" the DP3 result lives in.  Don't
swap these without re-decoding — sce-cgc is consistent about
this.

**FENCBR** is NV40's fence-before-read.  Required between any
two dependent ops where the second reads a register the first
just wrote.  Insns with COS/SIN/RCP/RSQ/DIV scalar-functional-
unit results need a fence before any subsequent op reads them.
Our existing `asm_.emitFencbr()` handles the encoding (opcode
0x3E, OUT_NONE, OUT_REG = 0x3F, identity SRC defaults).

**FP matvecmul shape (4-DP4 expansion):**

NV40 FP has no native matrix opcode — matrix uniforms occupy 4
contiguous slots (one per row), and matvecmul lowers as 4 DP4s:

```
DP4 R_out.x, R_in, c[mat_row0]    + 16B zero block
DP4 R_out.y, R_in, c[mat_row1]    + 16B zero block
DP4 R_out.z, R_in, c[mat_row2]    + 16B zero block
DP4 R_out.w, R_in, c[mat_row3]    + 16B zero block
```

Mask narrows per-DP4 (X-only, Y-only, etc.).  The matrix
uniform's `embeddedConstants[N..N+3]` records cover all 4
slots, all with the same `entryParamIndex` pointing at the
matrix uniform — runtime patches all 4 from the same source.

For chained `mul(M2, mul(M1, v))` the second matvecmul reuses
R_out from the first, so the chain ping-pongs R0/R1 (4 DP4s
each).  Don't allocate fresh temps per chain step — sce-cgc
recycles through 2 R-temps total for any chain length.

**Register allocation hint (UNVERIFIED — confirm before relying):**

In the existing tests we got byte-exact, sce-cgc consistently
allocates the H-temp for fp16 promotion at H4 (rsqrt/cos preload),
R0 for the output, R1 for sample/secondary input.  R2 only
appears in the SimpleFragment lane-override shape.  Whether this
generalizes to longer FP programs is unverified — re-decode
before committing to a register-allocation rule.

**NRM opcode (0x39) — UNVERIFIED:**

NV40 has a native NRM opcode (probably "DP3R + DIVRSQR + MUL"
fused).  Our existing `normalize(varying)` test emits the 4-insn
MOVH + DP3R + DIVRSQR shape and byte-matches the reference, so
it's clear sce-cgc doesn't use NRM for that case.  Whether NRM
appears in any other shape (longer programs, different precision
modes) is unverified — if you hit a normalize divergence, decode
the reference output's opcode field and check for 0x39.

## Gotchas pinned down (don't re-discover)

- **`set -uo pipefail` in `tests/run_diff.sh`** (intentional —
  was changed from `-euo` because `pipefail` + the rsx-cg-compiler
  exit-1-on-emit-failure makes `set -e` exit early on the first
  ucode-fallback failure).  Don't revert it back to `-euo`.
- **`buildReturnStmt`** only treats return-of-identifier as a
  struct return when the var's type is actually `BaseType::
  Struct`.  Without this, `float4 c; return c;` mis-routes away
  from the function-semantic StoreOutput emit.
- **`wrapW1` flag** on the bind type is the canonical way to fold
  `float4(intrinsic_result_vec3, 1.0)` into the intrinsic's emit.
  Already wired for Normalize and Reflect.  Apply the same pattern
  for any new intrinsic that returns vec3 and gets wrapped.
- **`emitLaneOverrides()` lambda** in StoreOutput appends per-lane
  MOV + inline literal blocks for any peeled VecInsert chain.  Call
  it after the producer emits to dst — it figures out which lanes
  were narrowed.
- **`valueToVaryingTexMul`** — `Mul(varying, tex_sample_result)`
  records this binding instead of erroring out.  Consumed by the
  uniform-conditional Select dispatch (B2).  Other arith mixes of
  varying + non-uniform operands need similar bindings.
- **`nv40_if_convert.cpp` Shape 4** requires the THEN block's
  instructions to all be `isPureOp` (anything except control flow,
  stores, Call, Phi).  Sample, ldunif, mul, add, sub, mad — all
  pure.  When a new op type lands inside conditionally-executed
  blocks, check `isPureOp` covers it.

## FragmentProgram.cg — the remaining IR shapes

Get this shader's IR with:

```bash
cd tools/rsx-cg-compiler
./build/rsx-cg-compiler --profile sce_fp_rsx --O2 --fastmath --dump-ir \
    ~/sony-build/cell-sdk-shim/samples/tutorial/CgTutorial/GCM/Water/shaders/FragmentProgram.cg
```

Truncated highlights of the IR after const-fold + reflect /
normalize / cos already lower:

```
%25 = shuffle float %1            ; worldPos.x
%26 = shuffle float %1 [z]        ; worldPos.z
%27 = vec vec2 %25, %26           ; planePos = float2(worldPos.x, worldPos.z)
%42 = dot float %31, %27          ; dot(waveDir1_lit, planePos)  ← BLOCKER #1
%43 = dot float %35, %27          ; dot(waveDir2_lit, planePos)
%44 = dot float %40, %27          ; dot(waveDir3_lit, planePos)
%45 = vec vec3 %42, %43, %44      ; Length = float3(dot, dot, dot)
%58 = mul vec3 %21, %45           ; freq * Length (folded literal * vec3 of dots)
%60 = mul vec3 %23, %59           ; phase * gTime (folded literal * uniform float)
%61 = sub vec3 %58, %60           ; freq*Length - phase*gTime
%62 = cos vec3 %61                ; cos(vec3) ← BLOCKER #2 (current cos only takes scalar)
%63 = mul vec3 %57, %62           ; (freq*amplitude) * cos(...)
%65 = dot float %50, %63          ; dot(waveDirX_lit, waveCos)
%67 = dot float %55, %63          ; dot(waveDirY_lit, waveCos)
%76 = vec vec3 %71, %72, %75      ; (-1*dX, 1.0, -1*dY)
%77 = normalize vec3 %76          ; ← normalize(arith_vec3)  BLOCKER #3
%82 = normalize vec3 %81          ; normalize(worldPos.xyz - gEye)  ✓ (existing)
%85 = reflect vec3 %82, %77       ; reflect(varying-derived, varying-derived)  ← BLOCKER #4
%100 = matvecmul vec4 %99, %97    ; FP mul(mat4, vec4)  ← BLOCKER #5
%121 = refract vec3 %82, %77, %120 ; ← BLOCKER #6 (deferred but unavoidable here)
%153 = mul vec4 %152, %115        ; gColorBlending.x * reflection (broadcast scalar * vec4)
%157 = add vec4 %153, %156        ; final color blend chain
```

## Workstream — order of attack

### 1. `dot(literal_vec2, varying_shuffle_vec2)` — BLOCKER #1

The reference compiler uses a non-obvious 3-instruction shape
when one operand is a folded literal vec and the other is a
shuffled varying.  Probe `tests/shaders/dot_lit_var_2_f.cg` (write
it: `dot(float2(0.97, 0.242), float2(worldPos.x, worldPos.z))` to
isolate just this).  Watch for:

- DP2 opcode (0x38) might be used directly, or expanded into
  MUL+ADD.
- Inline literal block holds the literal vec2 (padded with 0s).
- Source swizzles for the varying might use the `R1.xyw` pack
  pattern documented in the `dot(varying, varying)` decode (see
  `next-session-prompt-water-fp.md` Note 2 — three years of `.z`
  → `.w` swizzle-pack quirks).

Land a bind: `FpDotLiteralBinding { IRValueID inputId, lane[2],
literal[2] }`.  Emit at the point where the dot result feeds a
`VecConstruct vec3` (the `Length` shape) so all three dots
co-emit and share register state.

### 2. `cos(vec3)` — BLOCKER #2

Current cos handler only binds via `valueToScalarExtract`
(`cos(vcol.x)`).  The Water shape is `cos(vec3_arith)` which
maps to per-lane COS (the NV40 COS opcode is scalar; for a vec3
result the front-end emits one COS per lane, or sce-cgc may emit
a single COS broadcast if the input is identifiable as a single
scalar broadcast).

Probe sce-cgc with `cos(varying.xyz - uniform)` to see whether
it emits 3 COS instructions or a single COS + broadcast.

### 3. `normalize(arith_vec3)` where the arith isn't `varying ±
uniform` — BLOCKER #3

`%76 = vec vec3 %71, %72, %75` (where %71 / %75 are MUL chains
and %72 is a literal 1.0 — the `(-1.0*dX, 1.0, -1.0*dY)` shape).
Then `normalize(%76)`.

Extend `FpNormalizeBinding::hasArith` to capture more arith shapes
than just `Add`.  Or fall back to a generic "compute arith into
R0, then DP3+DIVRSQR" path that any well-formed vec3 producer
satisfies.  See item #6 below for the broader generalisation.

### 4. `reflect(varying-derived, varying-derived)` — BLOCKER #4

Current reflect requires both operands to resolve via
`valueToInputSrc` (direct varyings).  In FragmentProgram both
operands are normalize() results.  Extend reflect to accept any
"vec3 in an R-temp" — the pre-MAD setup is just `MOVR R0=N,
MOVR R1=I` and these can come from prior emits if the bindings
chain.

This and #3 share the underlying need: a generic "produce any
vec3 into a designated R-temp" mechanism that intrinsics can
invoke.  See #6.

### 5. FP `matvecmul(mat4, vec4)` — BLOCKER #5

Currently no FP lowering.  Reference shape is 4 DP4s into a
single R-temp's lanes, with the matrix uniform spanning 4
constant slots:

```
DP4 R_out.x, R_in, c[mat[0]]      <16B zero block>
DP4 R_out.y, R_in, c[mat[1]]      <16B zero block>
DP4 R_out.z, R_in, c[mat[2]]      <16B zero block>
DP4 R_out.w, R_in, c[mat[3]]      <16B zero block>
```

Track FP matrix uniforms separately from vec4 uniforms — the
container emits them with `embeddedConstants[N..N+3]` covering
4 slots, all backed by the same matrix at runtime.

### 6. Generic "compute any vec3 into R-temp" plumbing

The right fix for #3 + #4 + the chained-arith case (`gColorBlending.
x * reflection + gColorBlending.y * refraction + ...`) is a
"multi-stage emit" mechanism: each binding declares whether it
can emit standalone (writes to a designated dst reg) or only as
part of a fused larger pattern.  Today every binding type expects
to be the StoreOutput's terminal producer.

Concretely: factor each existing intrinsic emit into a function
`bool emitInto(nvfx_reg dst, uint8_t mask, bool markEnd)`.
Compose via the consuming op (Reflect's MOVR R0=N first, MOVR
R1=I second — both produced by their underlying bindings).  This
is a meaningful refactor (a few hundred lines) but unblocks every
chained-intrinsic shape FragmentProgram exercises.

### 7. `refract(I, N, eta)` — BLOCKER #6

Cg stdlib expansion (~19 ucode units, including a conditional
branch on `cost2 > 0`).  After #6 lands, refract follows the
same pattern as reflect (just a longer expansion).  See
`docs/next-session-prompt-water-fp.md` for the full Cg standard
library formula.

## Pre-existing 2 harness fails

These don't block FragmentProgram.cg — handle separately.

### `tests/shaders/alpha_mask_split_write_f.cg`

```cg
void main(float4 color : COLOR0, float2 texcoord : TEXCOORD0,
          uniform sampler2D texture, out float4 oColor : COLOR0)
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

Errors with `nv40-fp: unsupported IR op #78` (`CondBranch`).  The
B1 lane-write change should let the ELSE branch's `oColor.rgb =
color.rgb; oColor.a = color.a;` lower via VecInsert chains.  But
the IF-ELSE diamond isn't matched by any existing
`nv40_if_convert.cpp` shape — the comparison's LHS is a
**tex-result scalar** (`tex2D(...).w`), not a varying or uniform.
Plus the THEN branch is a literal-vec4(0) write.

The existing `alpha_mask_tex_f.cg` test handles `(tex.a CMP k) ?
0 : varying` (Select tex-LHS).  This split-write variant adds
per-lane writes in the ELSE branch.  Strategy:

- Run if-conversion's Shape 1 (full diamond) with the new SSA
  Phi-merge.  THEN's `oColor = float4(0)` and ELSE's
  `oColor.rgb = ...; oColor.a = ...;` both terminate with a
  StoreOutput once VecInsert chains converge.  After the IR
  builder phi-merge inserts a Select for `oColor`, the diamond
  matches Shape 1 with the cmp's tex-result LHS dispatch.

### `tests/shaders/mul_chain4_v.cg`

VP — chained `mul(matrix, vector)` against four file-scope
matrix uniforms, **plus** per-component MAD chain on the color
output (`c.r = c.r * tweak.x + tweak.y + offset.x;` × 4 lanes).

**Status changed in 2026-04-30 session:** error shifted from
"diverges by 2 ucode words + 1 register" to `nv40-vp: unsupported
IR op (handles MOV from attr/uniform only)`.  Cause: B1's
IR-builder VecInsert lowering for `c.<lane> = expr` now produces
VecInsert chains the VP back-end doesn't handle at all (the FP
back-end was extended in B1; the VP back-end was not).

Real fix path:
1. Add `case IROp::VecInsert` and `case IROp::VecExtract` to
   `nv40_vp_emit.cpp`.  The existing per-lane MOV-with-mask emit
   that `immediateStores` uses is already most of what's needed
   — VecInsert composes naturally.
2. Re-decode reference output for the four-matrix chain; the
   prior session's "fresh temp on last chain step" rule is still
   valid — verify the recycling tweak.
3. The per-component MAD chain (`c.r = ...`, etc.) likely needs
   `valueToArith`-style binding that lifts each lane's MAD into
   a single output-lane MOV at StoreOutput time.

If the VecInsert lowering proves complex, an interim path is to
have the IR builder emit the OLD per-name composite store for
VP only (gated by `currentFunction_->isFragment` or similar) so
mul_chain4_v passes without the VP back-end refactor.  Not
elegant but unblocks 86/86.

## Repro

```bash
cd /home/firebirdta01/source/repos/PS3_Custom_Toolchain
source scripts/env.sh
cd tools/rsx-cg-compiler
cmake --build build
bash tests/run_diff.sh                 # current: 84 / 86

# Verify Water's 5 currently-byte-exact shaders still pass:
SCE=../../reference/sony-sdk/host-win32/Cg/bin/sce-cgc.exe
for s in VertexProgram SimpleVertex RenderSkyBoxVertex \
         RenderSkyBoxFragment SimpleFragment; do
    cg=~/sony-build/cell-sdk-shim/samples/tutorial/CgTutorial/GCM/Water/shaders/$s.cg
    ext=$([[ $s == *Fragment* ]] && echo fpo || echo vpo)
    prof=$([[ $s == *Fragment* ]] && echo sce_fp_rsx || echo sce_vp_rsx)
    WINEDEBUG=-all wine "$SCE" --quiet --profile $prof --O2 --fastmath \
        -o /tmp/${s}_sce.$ext "$cg" 2>/dev/null
    ./build/rsx-cg-compiler --profile $prof --O2 --fastmath \
        --emit-container /tmp/our_${s}.$ext "$cg" 2>/dev/null
    cmp -s /tmp/our_${s}.$ext /tmp/${s}_sce.$ext && echo "[OK] $s" || echo "[FAIL] $s"
done

# Try the kitchen sink:
./build/rsx-cg-compiler --profile sce_fp_rsx --O2 --fastmath \
    --dump-ir ~/sony-build/cell-sdk-shim/samples/tutorial/CgTutorial/GCM/Water/shaders/FragmentProgram.cg
```

## House rules

- `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md` — append
  any new binary-format discovery here, terse.
- No "sce-cgc" by name in tracked source/comments — say "the
  reference compiler" / "the original Cg compiler".
- `tests/shaders/<name>.cg` — every new test gets a clean-room
  source comment explaining what shape it locks in.
- Pre-existing 2 fails stay red until you fix them — don't `rm`
  the test fixtures.

## Memory pointers

The session memory is at
`~/.claude/projects/-home-firebirdta01-source-repos-PS3-Custom-Toolchain/memory/`.
Relevant entries:

- `project_rsx_cg_compiler_fp_water_fp.md` — B1 (lane writes)
- `project_rsx_cg_compiler_fp_uniform_cond.md` — B2 (uniform-cond)
- `project_rsx_cg_compiler_const_fold.md` — A (const-fold) + C (sample)
- `project_rsx_cg_compiler_intrinsics.md` — B3 (cos/sin/reflect/normalize-of-arith)
- `project_rsx_cg_compiler_vp_features.md` — VP back-end status
- `feedback_byte_exact_shader_output.md` — every change goes
  through `tests/run_diff.sh`, no exceptions.

## Definition of done

- [ ] `bash tests/run_diff.sh` reports `86 / 86`.
- [ ] All 6 Water-sample shaders byte-exact via our compiler
      (verify with the loop above).
- [ ] FragmentProgram.cg's `cellGcmCgGetNamedParameter` lookups
      still resolve correctly when running the Water sample in
      RPCS3 — same names: `gReflectionMap`, `gRefractionMap`,
      `gEye`, `gTime`, `gWaterColor`, `gLightDirection`,
      `gColorBlending`, `gFragmentModelViewProjectionMtx`.
- [ ] No regressions in the existing 5 Water shaders or any
      other harness fixture.
