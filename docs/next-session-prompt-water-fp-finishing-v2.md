# Next session — close Water FP + 86/86 (v2)

Supersedes `docs/next-session-prompt-water-fp-finishing.md`.  Same end
state target: `bash tests/run_diff.sh` reports `86 / 86` and
FragmentProgram.cg is byte-exact.  This v2 prompt adds the field-
level decoding done at the start of the previous attempt so you do
not have to re-derive it.

## Status snapshot (verified)

- `bash tests/run_diff.sh` → **84 / 86**.
- Failing fixtures (unchanged from v1):
  - `tests/shaders/alpha_mask_split_write_f.cg` (FP IF-ELSE diamond
    with tex-LHS comparison + per-lane writes in ELSE)
  - `tests/shaders/mul_chain4_v.cg` (VP per-component MAD chain on
    color output via VecInsert chains the VP back-end can't lower)
- Water/FragmentProgram.cg still falls back to the reference Cg
  compiler because it triggers `nv40-fp: Dot supported only for
  (varying, uniform) pairs today` on the first planar dot.
- No new code changes vs. v1 baseline.  The previous tmp scratch
  file `nv40_fp_emit.cpp.tmp.4877.1777560442389` was deleted —
  no other working-tree changes happened.

## What you have to start with

Read v1 (`next-session-prompt-water-fp-finishing.md`) end-to-end.
This v2 only adds:

1. The decoded reference encoding for `dot(literal_vec2, varying_
   shuffle_vec2)` — Blocker #1.
2. A clean fixture text ready to drop into
   `tests/shaders/dot_lit_var_2_f.cg`.
3. Concrete pointers into `nv40_fp_emit.cpp` for where to plug new
   bindings in.

Everything else (warm intuition on hw[N] field layout, register-
allocation heuristics, wrapW1, isPureOp gotcha, etc.) lives in v1.

## Blocker #1 — `dot(literal_vec2, varying_shuffle_vec2)` (decoded)

### Probe shader (drop into `tests/shaders/dot_lit_var_2_f.cg`)

```cg
// Locks in dot(literal_vec2, varying-derived_vec2) — folded literal
// vec crossed with a vec2 built from two lanes of one varying.  The
// reference compiler packs the varying lanes via MOV with .xzzw
// swizzle, sets the unrelated W lane via inline c[0]={1,0,0,0}, then
// emits one DP2 with the literal in c[0]={0.97, 0.242, 0, 0}.
void main(float4 worldPos : TEXCOORD0,
          out float4 oColor : COLOR)
{
    float2 dir      = float2(0.97f, 0.242f);
    float2 planePos = float2(worldPos.x, worldPos.z);
    float  d        = dot(dir, planePos);
    oColor          = float4(d, d, d, 1.0f);
}
```

After IR builder + const-fold, `@main` has this shape (verified):

```
%6  = const vec2 {0.97, 0.242}
%8  = shuffle float %1            ; worldPos.x
%9  = shuffle float %1 [z]        ; worldPos.z
%10 = vec vec2 %8, %9             ; planePos
%12 = dot float %6, %10
%14 = vec vec4 %12, %12, %12, %13 ; %13 = const 1.0
stout %14 COLOR
```

### Reference ucode (3 insns + 2 const blocks, 80 bytes)

`xxd /tmp/dot_lit_var_2_sce.fpo` from offset 0xc0:

```
0xc0:  86 00 01 00 d0 01 1c 9d c8 00 00 01 c8 00 3f e1   ; insn 1: MOV
0xd0:  10 00 01 00 00 02 1c 9c c8 00 00 01 c8 00 00 01   ; insn 2: MOV
0xe0:  00 00 3f 80 00 00 00 00 00 00 00 00 00 00 00 00   ; const block: {1.0, 0, 0, 0}
0xf0:  0e 01 38 00 c8 00 1c 9d 08 02 00 00 c8 00 00 01   ; insn 3: DP2 (END)
0x100: 51 ec 3f 78 ce d9 3e 77 00 00 00 00 00 00 00 00   ; const block: {0.97, 0.242, 0, 0}
```

Decoded (logical = halfswap of on-disk u32-BE):

```
insn 1:  hw[0] = 0x01008600   MOV  R0.xy   mask=XY  input_src=TC0 (4)
         hw[1] = 0x1c9dd001   src0=f[TC0].xzzw                       ; bit-decoded:
                                                                     ;   TYPE=INPUT(1) IDX=0
                                                                     ;   SWZ_X=0(X) SWZ_Y=2(Z)
                                                                     ;   SWZ_Z=2(Z) SWZ_W=3(W)
         hw[2] = 0x0001c800   src1 default (R0.xyzw, unused for MOV)
         hw[3] = 0x3fe1c800   src2 default + bits at MSB
         ; effect: R0.x ← worldPos.x  ;  R0.y ← worldPos.z

insn 2:  hw[0] = 0x01001000   MOV  R0.w    mask=W   input_src=POSITION(0) (unused)
         hw[1] = 0x1c9c0002   src0=c[0].xxxx                         ; TYPE=CONST(2) IDX=0
                                                                     ;   SWZ_X=0(X) SWZ_Y=0(X)
                                                                     ;   SWZ_Z=0(X) SWZ_W=0(X)
         hw[2] = 0x0001c800   src1 default
         hw[3] = 0x0001c800   src2 default
         <16-byte inline const block: {1.0, 0, 0, 0}>
         ; effect: R0.w ← 1.0  (the literal alpha lane of the float4(d,d,d,1))

insn 3:  hw[0] = 0x38000e01   DP2  R0.xyz  mask=XYZ  PROGRAM_END
                                                                     ; opcode 0x38 = DP2
         hw[1] = 0x1c9dc800   src0=R0.xyzw                           ; TYPE=TEMP(0) IDX=0, identity
         hw[2] = 0x00000802   src1=c[0].xyxx                         ; TYPE=CONST(2) IDX=0
                                                                     ;   SWZ_X=0(X) SWZ_Y=1(Y)
                                                                     ;   SWZ_Z=0(X) SWZ_W=0(X)
         hw[3] = 0x0001c800   src2 default
         <16-byte inline const block: {0.97, 0.242, 0, 0}>
         ; effect: R0.xyz ← R0.x*0.97 + R0.y*0.242 (broadcast)
```

### What to encode

The minimum delta is one new binding type plus a recognizer in
`IROp::Dot` and an emitter that StoreOutput's existing
`emitLaneOverrides()` can layer the W=1.0 over.  The MAD-style
"emit at the consumer" pattern is the easiest fit:

```
struct FpDotLitPackBinding
{
    IRValueID          varyingId;        // base varying (resolves via valueToInputSrc)
    int                lanes[2];         // pack source lanes (e.g. {0, 2} for x/z)
    float              literal[4];       // {dir.x, dir.y, 0, 0} (vec2 padded)
    int                lanesUsed = 2;    // 2 for DP2; supports DP3 if you generalize
};
std::unordered_map<IRValueID, FpDotLitPackBinding> valueToDotLitPack;
```

Recognizer (inside `case IROp::Dot:` ahead of the existing
`(input, uniform)` paths):

1. `valueToLiteralVec4.find(opA)` finds a folded vec literal AND
   `valueToVecConstruct`/`valueToShuffle` chain on opB resolves to
   a single varying with two distinct shuffle lanes — record the
   binding.  Symmetric on (opB literal, opA varying-pack).
2. The number of pack lanes determines DP2 vs DP3 vs DP4.  Water's
   FragmentProgram does dot(vec2, vec2) → DP2, but generalize for
   the inner `dot(varying3, varying3)` case that already byte-
   matches in `dot3_f.cg` (different binding, same idea).

Emit (inside StoreOutput's main switch, ahead of the existing
broadcast-VecConstruct path):

1. MOV pack: write the source varying's lanes into a temp R-reg
   with a partial mask + tail-replicated swizzle.  Same byte
   pattern the existing `dot(varying, varying)` already produces
   for its second-operand pack — just one operand here.
2. (only when consumer is `vec4(d,d,d,1.0)` shaped) tail MOV for
   the W=1.0 literal — already done by `emitLaneOverrides()`
   when StoreOutput sees a VecInsert chain or a
   wrapW1-style alias.
3. DP2 (or DP3/DP4) with PROGRAM_END.  Source 1 reads the pack
   temp, source 2 reads `c[N].<lit-swizzle>` with the inline
   literal block.  Byte 1 of the src1 swizzle for DP2 .xyxx is
   `0x08` — match the decoded reference exactly.

The 3-instruction order in the reference is **pack, W-set, DP2**
— the W=1.0 MOV emits BEFORE the DP2 because DP2 owns
PROGRAM_END.  StoreOutput's existing lane-override lambda has
to be told to flush its overrides before the producer's last
op rather than after; this is the same "scheduler-aware
ordering" already implemented for normalize+wrapW1 (see
`emitLaneOverrides` callers in `nv40_fp_emit.cpp`).

### Where to plug in (concrete)

- `case IROp::Dot:` lives at `nv40_fp_emit.cpp:900`.  Add the
  recognizer right after the size check, before the existing
  `(input, uniform)` path so it's tried first.
- Emit dispatch at StoreOutput is in the long switch starting
  at `nv40_fp_emit.cpp:1533`.  The natural insertion point is
  immediately after the existing literal-vec4 / scaled-lanes
  paths — find the StoreOutput broadcast-handling block and
  add a "binding came from valueToDotLitPack" branch.
- `emitLaneOverrides()` is the lambda inside the StoreOutput
  case.  Audit its W-flush ordering against the reflect+wrapW1
  emit (already correct) before plugging the dot-pack in.

## Blocker #1.5 — three planar dots in a row

After Blocker #1 byte-matches on `dot_lit_var_2_f.cg`, the actual
Water FragmentProgram does THREE planar dots in a row, all sharing
the SAME varying-pack temp:

```
%42 = dot %lit1, %planePos  ;
%43 = dot %lit2, %planePos  ;
%44 = dot %lit3, %planePos  ;
%45 = vec vec3 %42, %43, %44 ; Length
```

The reference emits a single MOV pack for `%planePos` and reuses
that temp across all three DP2s.  Watch for double-emit bugs in
the binding's flush logic — the temp build should fire once on
first consumption, then the binding flag flips so subsequent
dots skip re-emitting it.

## Remaining blockers — see v1

- #2 cos(vec3) — per-lane vs broadcast.  v1 has the workplan.
- #3 normalize(arith_vec3) where arith isn't varying±uniform.
- #4 reflect(varying-derived, varying-derived).
- #5 generic "compute vec3 into R-temp" — refactor of intrinsic
  emit signatures.  The unblock for #3 / #4 / #7.
- #6 FP matvecmul(mat4, vec4) — 4-DP4 lowering.
- #7 refract(I, N, eta) — depends on #5.

## Pre-existing 2 fails — see v1

- `alpha_mask_split_write_f.cg` — IF-ELSE diamond w/ tex-LHS
  cmp + per-lane writes in ELSE.  v1 strategy: Shape 1
  full-diamond if-conv with the new SSA Phi-merge.
- `mul_chain4_v.cg` — VP back-end has no VecInsert / VecExtract
  handler today.  v1 lists the real fix and the interim "gate
  IR-builder VecInsert lowering on stage" path.

## Repro

```bash
cd /home/firebirdta01/source/repos/PS3_Custom_Toolchain
source scripts/env.sh
cd tools/rsx-cg-compiler
cmake --build build
bash tests/run_diff.sh                 # current: 84 / 86

# Cleanest probe of the decoded dot-lit-pack shape:
SCE=/home/firebirdta01/cell-sdk/475.001/cell/host-win32/Cg/bin/sce-cgc.exe
WINEDEBUG=-all wine "$SCE" --quiet --profile sce_fp_rsx --O2 --fastmath \
    -o /tmp/dot_lit_var_2_sce.fpo  tests/shaders/dot_lit_var_2_f.cg 2>/dev/null
xxd /tmp/dot_lit_var_2_sce.fpo
```

## House rules (carry from v1)

- Append any new binary-format discovery to
  `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md`, terse.
- No "sce-cgc" by name in tracked source/comments — say "the
  reference compiler" / "the original Cg compiler".
- Every new test gets a clean-room source comment explaining
  what shape it locks in.
- Pre-existing 2 fails stay red until you fix them — don't `rm`
  the test fixtures.

## Definition of done

- [ ] `bash tests/run_diff.sh` reports `86 / 86`.
- [ ] All 6 Water-sample shaders byte-exact via our compiler.
- [ ] FragmentProgram.cg's `cellGcmCgGetNamedParameter` lookups
      still resolve correctly when running the Water sample in
      RPCS3.
- [ ] No regressions in the existing 5 Water shaders or any
      other harness fixture.
