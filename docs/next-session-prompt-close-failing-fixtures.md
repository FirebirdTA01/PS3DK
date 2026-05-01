# Next session — close the 2 failing harness fixtures

This prompt is the actionable plan to take rsx-cg-compiler from
**89 / 91** to **91 / 91** by landing the two pre-existing failures:

- `tests/shaders/alpha_mask_split_write_f.cg` (FP)
- `tests/shaders/mul_chain4_v.cg` (VP)

Both have substantial reverse-engineering already done (see this
prompt + the memory entries it points to).  Total estimated work:
**~800-1500 lines** across the FP and VP back-ends, plus
reverse-engineering ~20-30 additional reference bytes for the dual-
issue VP encoding.

## Status snapshot (verified)

```
$ source scripts/env.sh && cd tools/rsx-cg-compiler && bash tests/run_diff.sh | tail -3
[OK  ] uniform_mov_f (240 bytes container match)

89 / 91 shaders match sce-cgc byte-for-byte
```

The 2 reds:
- `[FAIL] alpha_mask_split_write_f: ucode diverges from sce-cgc`
- `[FAIL] mul_chain4_v: ucode diverges from sce-cgc`

All 6 Water-sample FP/VP shaders byte-exact.  All 5 byte-exact FP
shapes from the dot-pack family pass cleanly (covered in
`memory/project_rsx_cg_compiler_dot_lit_pack.md`).

## What's already in tree (don't redo)

Pipeline plumbing committed in `b1452ec`:

1. **IR builder StoreOutput dedup** — when a swizzle-write to an
   out-param emits a StoreOutput, any prior StoreOutput in the same
   block targeting the same semantic gets dropped.  Needed because
   `oColor.rgb = ...; oColor.a = ...;` would otherwise leave two
   stores and the if-converter couldn't collapse the diamond.

2. **`nv40_if_convert::isHoistableOp` includes `VecInsert`** —
   VecInsert is pure (no side effects) so hoisting it from a
   conditional arm into entry is safe.  This lets Shape 1 (full
   diamond) collapse when the ELSE arm is a per-lane VecInsert chain.

3. **`nv40_fp_emit` handles `IROp::VecExtract`** — aliases via
   pure SrcMod chains (identity-prefix VecShuffle records) to a
   ScalarExtract binding.  Needed once the if-converter starts
   hoisting `extract` ops out of conditional arms.

After these, `alpha_mask_split_write_f.cg` advances from
"unsupported IR op #78 (CondBranch)" to a clean `Select(cmp_tex_lhs,
vec4(0), VecInsert_chain_rebuilding_color)` IR — but the next
blocker is the FP back-end's tex-LHS Select dispatch (more on that
below).

## Plan A — alpha_mask_split_write_f.cg

### Current IR (after pipeline plumbing)

```
entry:
  %6  = sample vec4 %3, %2
  %7  = shuffle float %6 [w]
  %9  = cmple bool %7, 0.5
  %15 = shuffle vec3 %1 [xyz]      ; color.rgb
  %16 = extract float %15 [0]
  %17 = insert vec4 %4, %16 [0]    ; oColor.r = color.r
  %18 = extract float %15 [1]
  %19 = insert vec4 %17, %18 [1]   ; .g
  %20 = extract float %15 [2]
  %21 = insert vec4 %19, %20 [2]   ; .b
  %22 = shuffle float %1 [w]
  %23 = insert vec4 %21, %22 [3]   ; .a
  %25 = select vec4 %9, vec4(0), %23
  stout %25 COLOR
  ret
```

The if-conversion ran: Shape 1 collapsed the diamond into a Select
with `falseValue = %23` (the VecInsert chain rebuilding `color`).

### Current blocker

```
nv40-fp: Select tex-LHS: FALSE branch must be a varying input today
```

The existing tex-LHS Select dispatch (used by `alpha_mask_tex_f.cg`)
expects `falseValue` to resolve to a direct `valueToInputSrc` entry.
Our `%23` is a VecInsert chain instead.

### Minimum-effort path (Approach A1)

Recognize that the VecInsert chain `%17 → %19 → %21 → %23` rebuilds
all 4 lanes of `%4` (oColor) from corresponding lanes of `%1`
(color), and fold it into a direct alias `%23 ≡ %1`.  Then the Select
falseValue resolves to a varying and the existing tex-LHS path fires.
The output would byte-match `alpha_mask_tex_f.cg`'s 4-instruction
shape — but **the reference compiler emits a different 6-instruction
shape for THIS source** (see decode below), so this approach won't
actually byte-match.  Reproduce the reference output instead.

### Approach A2 — reproduce the reference output

The reference output is **6 instructions** (432 bytes):

```
; Decoded from reference output (per_lane reverse-engineering).  See
; tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md for the FP
; encoding cheat-sheet.

Insn 1 (0x140): TEX R1.w, f[TC0], TEX0
                — sample texture's W lane only.

Insn 2 (0x150): MOVH H?, ???       (half-precision MOV; bit 7 of
                                    byte 0 in hw[0] is OUT_REG_HALF)
                — preloads color into a half-temp pair.  Decode the
                exact source: byte 1 of hw[0] = 0x3e (mask .xyzw,
                input_src bits suggest COL0 but "FOGC" may be
                misencoded — verify against the reference bytes).

Insn 3 (0x160): SGE [OUT_NONE | mask=W | COND_WRITE_ENABLE]
                R1.w, c[0].x  + inline {0.5, 0, 0, 0}
                — sets CC bits where R1.w >= 0.5 (the inverse of
                "alpha <= 0.5"; the conditional MOVs below use NE/EQ
                accordingly).
                opcode 0x0C (SGE), byte 3 bit 6 = 1 (OUT_NONE).

Insn 4 (0x180): MOV R0.w, ??? (predicated)
Insn 5 (0x190): MOV R0.w?, ???
Insn 6 (0x1a0): MOV[END] R0, ???
                — the predicated MOV chain producing the conditional
                output.  Two of these write R0.w; one writes R0.xyzw.
                Decode the exact swizzles + cc_test/cc_cond fields
                from the reference bytes.
```

To match this, the FP back-end's existing tex-LHS Select dispatch
needs:

1. Recognise the VecInsert-chain falseValue as equivalent to its
   underlying source vec (`%23 ≡ %1`).  Add a helper
   `resolveVecInsertChainToBaseVarying(IRValueID id)` that walks the
   chain and returns the base varying iff every lane is overridden
   with a scalar that matches the corresponding lane of the same
   underlying vec.

2. Extend the tex-LHS Select emit path to handle this specific shape
   (half-precision color preload → SGE cc-write → predicated MOV
   chain).  Look at the existing `alpha_mask_tex_f.cg` emit (which
   uses 4 instructions, not 6) and figure out what extra
   instructions sce-cgc inserts when the source has explicit per-lane
   writes.  Hypothesis: sce-cgc preserves a hint about the
   "split-write source shape" and emits more conservative ucode (an
   extra MOVH preload + a SCA-slot MOV) even when the IR is logically
   equivalent.

The half-precision MOVH preload is the new emit primitive.  Our
existing FP emitter does emit MOVH for normalize/reflect (FpScalarUnaryBinding
Cos/Sin/Rcp/Rsq), but always to H0 with mask `.xyzw` from a single
varying lane.  The split-write shape's MOVH preloads the FULL color
varying into a half-pair temp, masked `.xyzw`.  Same encoding shape,
just different src/dst semantics.

## Plan B — mul_chain4_v.cg (the bigger lift)

### Current IR (after pipeline plumbing)

```
entry:
  %13 = vec vec4 in_position, 1.0
  %14 = matvecmul vec4 modelViewProj, %13     ; chained matvecmuls
  %15 = matvecmul vec4 bias, %14
  %16 = matvecmul vec4 scale, %15
  %17 = matvecmul vec4 rot, %16

  ; Per-lane MAD chain on color (R)
  %19 = shuffle float in_color  [x]
  %20 = shuffle float tweak     [x]
  %21 = mul float %19, %20
  %22 = shuffle float tweak     [y]
  %23 = add float %21, %22
  %24 = shuffle float offset    [x]
  %25 = add float %23, %24
  %26 = insert vec4 in_color, %25, 0    ; oColor.r = ...

  ; ... lanes G and B follow same shape with tweak.[z|w] / offset.[y|z] ...

  ; Lane A is just an ADD (no Mul)
  %43 = shuffle float %42 [w]
  %44 = shuffle float offset [w]
  %45 = add float %43, %44
  %46 = insert vec4 %42, %45 [3]

  stout %17 POSITION
  stout %46 COLOR
```

### Current blocker

```
nv40-vp: unsupported IR op (handles MOV from attr/uniform only)
```

Triggers on `IROp::VecInsert` (which the VP back-end has no case for
at all).

### Reference compiler emit — full mul_chain4_v output

19 instructions, **interleaved**:

```
Pos  Insns                                    Conceptual op
0x640-0x67f    4× DP4                          modelViewProj × p   → R0
0x680          1× ???                          color computation #1
0x690-0x6c0    4× DP4                          bias × prev          → R1
0x6d0          1× ???                          color computation #2
0x6e0-0x710    4× DP4                          scale × prev         → R0
0x720          1× ???                          color computation #3
0x730-0x76f    4× DP4                          rot × prev → out_pos (END on last DP4)
```

The 3 color insns interleaved between matvecmul groups suggest
sce-cgc fills DP4-result stalls with independent color ops.

### Reference emit — isolated per-lane MAD chain

Probed by writing a simpler version (`per_lane_mad_chain.cg`):
position = passthrough, color = the same per-lane chain.  Reference
output: **5 instructions**, opcodes MOV-MAD-MOV-MOV-ADD.

```
Insn 1 (MOV, dst=POS):         out_position = float4(in_position, 1.0)
Insn 2 (MAD, dst=TEMP[58]?):   probable VEC slot of a dual-issue pair
Insn 3 (MOV, dst=POS):         probable SCA slot of the dual-issue pair
                               (writes POS in parallel with insn 2's MAD)
Insn 4 (MOV, dst=TEMP):        VEC slot — preload c[offset]
Insn 5 (ADD, dst=COL0, END):   alpha lane write
```

**Hypothesis**: insns 2/3 and 4/5 are dual-issued pairs.  NV40 VP
hardware can run a VEC op (slot 0) and an SCA op (slot 1)
simultaneously in one 16-byte instruction; each has its own dst,
swizzle, and opcode at different bit positions in hw[3].

This is the structural unblock for Plan B.  Without dual-issue
support, our emitter can't reproduce the reference shape.

### Required NV40 VP back-end work (in dependency order)

1. **VecInsert / VecExtract recognizers in `nv40_vp_emit.cpp`.**
   - `IROp::VecInsert`: record `VecInsertBinding { vectorId,
     scalarId, lane }` into a `valueToVecInsert` map (mirrors the FP
     back-end's existing structure).
   - `IROp::VecExtract`: record into the existing
     `valueToShuffle` with `width = 1`, `lanes[0] = componentIndex`.
   - At `emitStoreOutput` time, when `srcId` resolves to a
     VecInsert, peel the chain inward and emit per-lane writes —
     either as separate partial-mask MOV/MAD/ADD instructions, or
     (for byte-match) folded into the dual-issue shape below.

2. **Scalar arith chain resolution.**  Today `regFromValue` resolves
   only direct `valueToSource` entries.  When a Mul/Add operand is
   itself a Mul/Add result (e.g. `%23 = add %21, %22` where
   `%21 = mul %19, %20`), bail.  Need to either materialise the
   inner arith into a temp lane and read back, or recognise specific
   chain shapes (MAD fusion, etc.) at IR-walk time.

3. **NV40 VP dual-issue VEC + SCA slot encoding.**  The current
   `VpAssembler::emit` takes ONE opcode and writes ONE slot.  The
   reference output uses two-op-per-instruction encoding:
   - VEC slot:  `hw[1]` bits 22-26 = vec opcode, dest temp at hw[0]
     bits 15-20, dest output / mask at `hw[3]` bits 2-6 / 13-16,
     VEC_RESULT bit 30 of hw[0].
   - SCA slot:  `hw[1]` bits 27-31 = sca opcode (`NVFX_VP_INST_SCA_OPCODE_SHIFT
     = 27`), dest temp at hw[3] bits 7-12 (`SCA_DEST_TEMP_SHIFT`),
     dest output / mask at hw[3] bits 2-6 / 17-20 (`SCA_WRITEMASK_SHIFT`),
     SCA_RESULT bit ? of hw[3].
   - Need to figure out which bits in `hw[3]` carry the SCA-side
     fields when both slots are populated.  Decode the
     per_lane_mad_chain reference output bytes byte-by-byte to find
     them.
   - Refactor `VpAssembler::emit` to accept a `MergedInsn { vec_op,
     vec_dst, vec_srcs[3], vec_mask, sca_op, sca_dst, sca_srcs[3],
     sca_mask }` that encodes a VEC+SCA pair into one 16-byte word.

4. **High-numbered temp allocator alignment.**  The reference output
   uses TEMP[58] for SCA-side results.  Either:
   - Reserve temps 32..63 for SCA-side results in our allocator and
     drive `numTempRegs` up to match the reference's container
     header field.
   - Or document why we don't and accept that even functionally
     correct ucode may diverge in the temp-count metadata.

5. **MAD-fusion / vector-fold pattern recognizer.**  When a VecInsert
   chain has the structure
   `lane[k] = in.lane[k] * uniform.x + (uniform.[k+1] + uniform2.[k])`
   for k ∈ {0, 1, 2} and `lane[3] = in.w + uniform2.w`, fold to:
   - 1 MOV preload R_temp = c[offset]
   - 1 ADD R_temp.xyz = c[tweak].yzw + R_temp.xyz (combined addend)
   - 1 MAD out_color.xyz = in_color.xyz * c[tweak].xxx + R_temp.xyz
   - 1 ADD out_color.w = in_color.w + R_temp.w
   Total 4 ops, plus 1 MOV for out_position = 5 (matches the
   simpler probe).  After dual-issue merging, paired down to 5
   instructions in the encoding.

6. **Interleaved scheduling.**  After (5), the 5 color ops need to
   slot into the 4-deep matvecmul chain at the positions the
   reference picks.  Hypothesis: sce-cgc inserts a color insn
   between each matvecmul group of 4 DP4s to hide DP4 result
   latency.  Implement either as a real scheduler or a shape
   recognizer for the 4-deep chain + per-lane MAD pattern.

### Estimated work

- Steps 1-2: ~150 lines, ~half a day.
- Step 3 (dual-issue): ~200 lines + RE work to map SCA-side bit
  positions; probably the hardest part.  Needs decoding of the 5
  instructions of `per_lane_mad_chain_sce.vpo` byte-by-byte.
- Steps 4-5: ~200 lines.
- Step 6: ~100-200 lines + testing.

Total: ~700-900 lines, plus the RE work upstream of step 3.

## NV40 VP encoding cheat-sheet (verified)

| Field | Word | Bit range | Notes |
|---|---|---|---|
| VEC_DEST_TEMP | `hw[0]` | 15-20 | 6-bit temp index, 0x3F sentinel = no temp |
| SRC0_ABS / SRC1_ABS / SRC2_ABS | `hw[0]` | 21 / 22 / 23 | per-source abs |
| SATURATE | `hw[0]` | 26 | saturate result |
| VEC_RESULT | `hw[0]` | 30 | set when writing to OUTPUT (also forces VEC_DEST_TEMP_MASK = 0x3F) |
| VEC_OPCODE | `hw[1]` | 22-26 | MOV=1, MUL=2, ADD=3, MAD=4, DP3=5, DPH=6, DP4=7 |
| SCA_OPCODE | `hw[1]` | 27-31 | RCP=1, RSQ=2, EXP=3, LOG=4, ... |
| INPUT_SRC | `hw[1]` | depends | varying input index for SRC slots reading INPUT type |
| CONST_SRC | `hw[1]` | 12-20 | 9-bit absolute c[] index, 0..511 |
| VEC_WRITEMASK | `hw[3]` | 13-16 | .xyzw mask for VEC slot |
| SCA_WRITEMASK | `hw[3]` | 17-20 | .xyzw mask for SCA slot |
| DEST (output) | `hw[3]` | 2-6 | POS=0, COL0=1, ..., TC0=7+n |
| DEST_MASK | `hw[3]` | 2-6 | 0x1F sentinel = no output (writes only TEMP) |
| LAST | last byte of last u32 | bit 0 | PROGRAM_END flag |

VP instructions are written **as-is to disk** — no halfswap (FP-only).
Each insn = 4 u32 BE = 16 bytes.

## Repro

```bash
cd /home/firebirdta01/source/repos/PS3_Custom_Toolchain
source scripts/env.sh
cd tools/rsx-cg-compiler
cmake --build build
bash tests/run_diff.sh                 # 89/91

# Get the reference outputs to diff against:
SCE=/home/firebirdta01/cell-sdk/475.001/cell/host-win32/Cg/bin/sce-cgc.exe
for s in alpha_mask_split_write_f mul_chain4_v; do
    cg=tests/shaders/$s.cg
    prof=$([[ $s == *_v ]] && echo sce_vp_rsx || echo sce_fp_rsx)
    ext=$([[ $s == *_v ]] && echo vpo || echo fpo)
    WINEDEBUG=-all wine "$SCE" --quiet --profile $prof --O2 --fastmath \
        -o /tmp/${s}_sce.$ext "$cg" 2>/dev/null
    ./build/rsx-cg-compiler --profile $prof --O2 --fastmath \
        --emit-container /tmp/our_${s}.$ext "$cg" 2>/dev/null || true
    cmp -s /tmp/our_${s}.$ext /tmp/${s}_sce.$ext \
        && echo "[OK] $s" || echo "[DIFF] $s"
done

# Probe the simpler per_lane_mad_chain shape (smaller test surface
# for VP dual-issue work):
cat > /tmp/per_lane_mad_chain.cg <<EOF
void main(
    float3 in_position : POSITION,
    float4 in_color : COLOR,
    uniform float4 tweak,
    uniform float4 offset,
    out float4 out_position : POSITION,
    out float4 out_color : COLOR)
{
    out_position = float4(in_position, 1.0);
    float4 c = in_color;
    c.r = c.r * tweak.x + tweak.y + offset.x;
    c.g = c.g * tweak.x + tweak.z + offset.y;
    c.b = c.b * tweak.x + tweak.w + offset.z;
    c.a = c.a + offset.w;
    out_color = c;
}
EOF
WINEDEBUG=-all wine "$SCE" --quiet --profile sce_vp_rsx --O2 --fastmath \
    -o /tmp/per_lane_mad_chain_sce.vpo /tmp/per_lane_mad_chain.cg 2>/dev/null
xxd /tmp/per_lane_mad_chain_sce.vpo | tail -8
```

## Caveats / gotchas

1. **Tests dir is gitignored** (`.gitignore` line 130:
   `/tools/rsx-cg-compiler/tests/`).  Test fixtures are local-only.
   The harness count is purely local progress; closing both fails
   takes us to 91/91 in the local harness without changing any
   tracked content other than the source code.

2. **FP and VP encoding differ.**  FP halfword-swaps each u32 at
   write time (see `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md`).
   VP writes u32s as-is.  Don't apply halfswap when decoding VP
   reference output.

3. **Sce-cgc temp index ≠ our temp index.**  The reference compiler
   uses TEMP[58] for SCA-side results.  Our allocator picks
   sequential 0..N.  Even if the byte pattern is "correct" in every
   other field, mismatched temp indices will diverge.  Either align
   the convention or audit it explicitly.

4. **Don't conflate FpScalarUnaryBinding's MOVH with the VP MOVH.**
   The FP MOVH (Cos / Sin / Rcp / Rsq) preloads a single varying
   lane into H0 with mask `.xyzw`.  The split-write shape's MOVH (in
   alpha_mask_split_write reference) preloads the full color varying
   into a half-pair, masked .xyzw — same encoding shape, different
   semantics.  Don't generalise the existing binding without
   isolating the cases.

5. **The 14 tests in the dot-pack family must keep passing.**  Run
   the full harness after each change.  See
   `memory/project_rsx_cg_compiler_dot_lit_pack.md` for the family.

6. **House rules** (carry from prior prompts):
   - `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md` — append any
     new binary-format discovery here, terse.
   - No "sce-cgc" by name in tracked source / commit messages — say
     "the reference compiler" / "the original Cg compiler".
   - Every new test fixture gets a clean-room source comment
     explaining what shape it locks in.

## Memory pointers

Relevant entries (auto-loaded into context via `MEMORY.md`):

- `project_rsx_cg_compiler_split_write_chain4.md` — full reverse-
  engineering log on both fixtures, NV40 VP encoding cheat-sheet
  reproduced, recommended phased work order.
- `project_rsx_cg_compiler_dot_lit_pack.md` — the 5 byte-exact FP
  shapes + the W=1.0 lane allocator rule (validated on 14 probes).
- `feedback_byte_exact_shader_output.md` — every change goes through
  `tests/run_diff.sh`, no exceptions.
- `feedback_no_sce_cgc_by_name.md` — extends the no-vendor-name
  family to the Cg compiler.
- `feedback_dont_commit_without_asking.md` — pause for explicit user
  confirmation before `git commit`; stage + ask.

## Definition of done

- [ ] `bash tests/run_diff.sh` reports `91 / 91`.
- [ ] All 6 Water-sample shaders byte-exact via our compiler.
- [ ] All 5 byte-exact FP shapes from the dot-pack family stay
      byte-exact.
- [ ] Probe `per_lane_mad_chain.cg` byte-matches as a stepping stone
      before tackling the full mul_chain4_v shape (gitignored fixture
      that lives in the harness).

## Suggested session order

If time permits one big push:

1. **Decode the dual-issue VP encoding.**  Take
   `per_lane_mad_chain_sce.vpo` and decode every bit of every
   instruction byte-by-byte until the SCA-slot fields are mapped.
   Append the findings to
   `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md`.

2. **Refactor `VpAssembler::emit` to support dual-issue.**  Either
   add a second `emitMerged(vec_insn, sca_insn)` entry point or take
   a unified `MergedInsn` struct.  Existing single-slot callers
   should keep working by passing a "no-op" SCA-slot.

3. **Wire `IROp::VecInsert` and `IROp::VecExtract` in
   `nv40_vp_emit.cpp`.**  Add the basic recognizers that record into
   binding maps.  Don't yet emit anything special at StoreOutput
   time — leave that for the pattern recognizer.

4. **Land the per-lane MAD chain pattern recognizer.**  Walk the
   VecInsert chain at StoreOutput time, detect the
   `(in.lane * uniform.x + sum_of_two_uniforms)` shape per lane,
   emit the 4-op functional sequence (MOV preload + ADD addend +
   MAD rgb + ADD alpha).

5. **Probe `per_lane_mad_chain.cg` until byte-match.**  Iterate on
   the dual-issue merging (which two ops pair up?) and the temp
   index allocation.

6. **Tackle `mul_chain4_v.cg`'s 4-deep matvecmul + interleaved
   color schedule.**  Thread the per-lane MAD chain emit between
   the existing matvecmul chain emits.  Verify the 19-instruction
   reference output.

7. **Loop back to alpha_mask_split_write_f.cg.**  Now that the FP
   back-end has a clear pattern for VecInsert-chain falseValue, add
   the half-precision color preload + SGE + predicated MOV chain
   emit specific to this shader.

8. Verify `bash tests/run_diff.sh` reports 91 / 91 and no
   regressions in the dot-pack family or any other fixture.

If time only permits Phase 1 of the VP work (steps 1-3 above),
**that alone is a meaningful structural unblock** for future
sessions and worth committing on its own.
