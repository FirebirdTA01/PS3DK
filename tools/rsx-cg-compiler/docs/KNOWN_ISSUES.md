# rsx-cg-compiler — Known Issues

Tracked deferrals where we understand the encoding but haven't yet
shipped emitter coverage.  Each entry names a specific reference
shape, what data we have on it, and what's needed to close the gap.

The byte-diff harness at `tests/run_diff.sh` is the authoritative
gate; everything below is something we've verified the reference
compiler does that we currently bail on with a diagnostic, not a
silent miscompilation.

---

## Scalar-lane repeated-add fold (`v.x + v.x + v.x + v.x`)

The full-vec4 `valueToScale` path now covers N ∈ [2, 64] (short
MOVH+MAD shape for N ≤ 3, long MOVH+MUL+chain+[FENCBR]+ADD/MAD for
N ≥ 4 — see `triple_vcol_f`, `quad_vcol_f`, `septuple_vcol_f` in the
test suite).  **The scalar-lane variant is still capped at 3.**

At scalar-lane N=4 sce-cgc switches to a 2-instruction DP2R shape
that doesn't share any structure with the full-vec4 chain:

```
MOVH H0.x, f[COL0]
DP2R R0.x [END], H0.x, {2,0,0,0}.x      # 2-component dot: 2*x*x = 4x
```

Probe at `/tmp/rsx-cg-chain-probe/scalar_x4_f.fpo`.  Higher scalar
counts (5, 6 …) almost certainly use a related DP-then-ADD shape
that hasn't been probed yet.

The IR detector caps scalar-lane at `kMaxScalarLaneScale = 3` (see
the `scaleCap` helper in `nv40_fp_emit.cpp`); past that, the chain
falls through to other handlers or produces a clear diagnostic, never
a miscompile.

---

## Lane-elision: scaledLane=W with < 3 unique values

The lane-elision handler in `nv40_fp_emit.cpp` (search
`FpScaledLanesBinding`) covers all four scaledLane positions
(X / Y / Z / W) for 2 and 3 unique const values across the non-scaled
lanes.  One outstanding case: **scaledLane=W with 0, 1, or 2 unique
values**.  The 3-unique row uses the natural inline-slot layout
(slots 0, 1, 2 hold the values, slot 3 is filler); the < 3 cases use
an asymmetric "shift-by-1" allocation where slot 0 is *also* filler
and the real values land in slots 1..2.

Probed examples (consts at output lanes X, Y, Z; scaledLane = W):

| consts (X,Y,Z) | inline       | swz   | unique |
|----------------|--------------|-------|--------|
| 0, 0, 0        | (0, 0, 0, 0) | .yyyy | 0      |
| 7, 7, 7        | (0, 7, 0, 0) | .yyyy | 1      |
| 0, 7, 7        | (0, 0, 7, 0) | .yzzw | 2      |
| 7, 0, 0        | (0, 7, 0, 0) | .yzzw | 2      |
| 0, 1, 0        | (0, 0, 1, 0) | .yzyw | 2      |
| 2, 3, 4        | (2, 3, 4, 0) | .xyzw | 3 ← natural |

The swz patterns themselves are predictable (`.yyyy` for all-same,
`.yzzw` / `.yzyw` for 2-unique depending on which lane has the odd
value out), but the *reason* sce-cgc shifts only here is opaque from
byte probes alone.  Best guess: a register-allocation interaction
with the trailing W slot, or a scheduler invariant that wants
`swz_for_dst[W] = W` (the identity-W swz) only achievable by leaving
slot 3 = filler — which it does in all cases — but for some reason
also forces slot 0 = filler when there's room.

Probe data: `/tmp/rsx-cg-lane-probe/sfw_*.fpo` (off-tree).  Today
the emitter bails with a `scaled-lanes scaledLane=W with < 3 unique
values not yet supported` diagnostic; the natural extension is a
small dispatch in the const-packing routine that allocates from
slot 1 instead of slot 0 when scaledLane=W and slotCount < 3.

---

## Multi-instruction if-else (predicated execution)

The simplest if-only-with-default + multi-insn-THEN shape is now
handled end-to-end via `IROp::PredCarry` (`nv40_if_convert.cpp` Shape
3 + the StoreOutput PredCarry handler in `nv40_fp_emit.cpp`).
Coverage today (all in the harness, byte-exact vs sce-cgc):

- `if_then_2add_f.cg` — 2-insn THEN, R1/R0 alternation, FENCBR.
- `if_then_3insn_f.cg` — 3-insn THEN, H2 LHS promote (R0/R1/R0).
- `if_then_4add_f.cg` — 4-insn THEN.
- `if_then_5add_f.cg` — 5-insn THEN.

The implementation enforces three restrictions; cases that hit any
of them currently fall through with a clear diagnostic, not a
miscompile:

1. **Default must equal the LHS varying of the comparison.**  Today
   we only handle `color = vcol; if (vcol.x > k) { color = ...; }`
   shapes — the default is `vcol` and the cmp LHS is `vcol.x`,
   which lets us reuse the MOVH-promoted H-temp as both the cmp
   source and the first carry source.  Generalising to an
   arbitrary default needs a separate carry-source register and
   the matching probe data.

2. **At most one non-LHS varying read across the chain.**  The
   chain links may all read the same preloaded varying (e.g. `vt`
   loaded once into R2), but two distinct non-LHS varyings would
   need an R-temp allocator that picks fresh slots per varying.
   Probes `fenc_skip2_f.fpo` show sce-cgc inserting interstitial
   preloads which also disable the FENCBR-before-last rule — so
   it's a coupled change, not just an allocator extension.

3. **Always emits FENCBR before the last predicated OP for chains
   of length >= 2.**  Matches sce-cgc when the chain uses R0/R1
   alternation (the most common shape).  Probes
   `if_then_multi_f.fpo` and `fenc_skip2_f.fpo` show no FENCBR
   when the alternation displaces to R2/R0 because of preload
   pressure — the rule appears tied to NV40's R-temp hazard window
   but the exact trigger is still unclear.

**Full if-else with multi-insn both branches** is the next tier and
remains deferred.  Probe `if_multi2_f.fpo` shows sce-cgc
*interleaving* THEN and ELSE instructions to share register
pressure: the THEN result is computed unconditionally into R1 via
two ALU instructions, then the ELSE result conditionally
overwrites R1 with `(EQ.x)`, then R0 carries forward, and the
final ELSE step also fires under `(EQ.x)`.  Implementing that
needs a second `PredCarry`-style chain in the rewrite for the ELSE
arm, an R-temp allocator that interleaves both chains over the
same registers, and the same hazard-aware FENCBR rule as above.

---

## FP entry-point implicit varying inference — DONE (2026-04-27)

Unbound FP entry-point inputs (no `: TEXCOORD<N>` / `: COLOR`
semantic) now infer to sequential `TEXCOORD<N>` slots in declaration
order, skipping any `TEXCOORD<N>` already explicitly used by a
sibling parameter.  The resource code (e.g. `0x0c94` = TEXCOORD0)
is written into the `.fpo` parameter table; the semantic *string*
is suppressed via the new `Semantic.inferred` flag, matching the
reference compiler's behaviour on probed `clear_fpshader.cg`,
`mixed_unbound_f.cg`, `two_unbound_f.cg`.  Test:
`tests/shaders/implicit_varying_f.cg`.

The TEXCOORD<N> 2D bit also gets cleared on a 4-component varying
read (was previously only cleared in projective / cube tex paths).

---

## FP basic tex2D / cube / proj — DONE

The `texture_sample_f.cg`, `texture_cube_f.cg`, and `texture_proj_f.cg`
shapes (a single `tex2D` / `texCUBE` / `tex2Dproj` whose result flows
*directly* into a `StoreOutput`) are byte-exact against the reference
compiler.  The remaining tex2D gaps live further along in the
dataflow — see "FP texture-result feeding compare / arithmetic"
below.

---

## FP texture-result feeding compare / arithmetic — PENDING

```cg
float4 tex = tex2D(texture, texcoord);
if (tex.a <= 0.5) { oColor = float4(0); }
else              { oColor = color;     }
```

Today the `TexSample` IR result feeds straight into a `StoreOutput`
in every test we've probed.  Real shaders (e.g. the alpha-masked
text shader from the reference SDK's `fw` framework) feed the result
into a `VecShuffle` (`tex.a`), then a `CmpLe` against a literal,
then drive an if-else.  Three coupled gaps:

1. **`CmpBinding` LHS is varying-only.**  `nv40_fp_emit.cpp`'s
   comparison handler only records a `CmpBinding` when the LHS is a
   scalar-extract of a value resolvable through `valueToInputSrc` —
   tex-sample results live in `valueToTex`, so the binding is
   silently dropped and the downstream `Select` bails with
   "Select needs a CmpGt/CmpGe/... constant-threshold binding".

2. **`if-convert` rejects multi-instruction THEN/ELSE arms.**  The
   pass requires each arm to be exactly `[stout; br]`.  The dbgfont
   shape's THEN arm is `[vec %14 ...; stout %14; br]` (a
   `VecConstruct` of four float consts before the stout), so the
   diamond never collapses to a `Select` and `CondBranch` reaches
   the emitter unlowered (`unsupported IR op #78`).  Fix shape:
   hoist side-effect-free instructions from each arm into entry
   before synthesising the Select.

3. **Reference encoding for tex-result-as-cmp-LHS isn't probed.**
   `dbgfont_fpshader.cg` ucode (3 instructions, 64 bytes) at
   `--O2 --fastmath`:
   ```
   inst 1 (16B): TEX R0, f[TEX0], TEX0
   inst 2 (32B): SLE [OUT_NONE | COND_WRITE_ENABLE], R0.w, {0.5}.x
   inst 3 (16B): MOV R0[CC.?], <something>            # END
   ```
   inst 1 is the same shape as our existing `texture_sample_f.cg`
   path.  inst 2 is an SLE writing only the condition code (no
   register destination — the OUT_NONE bit at 30 is set; same
   pattern as our existing SGTRC-based varying-cmp path but with
   the LHS sourced from a tempreg, not a varying input).  inst 3
   is a conditional MOV; it needs decoding to confirm whether it
   reads the COL0 varying directly or routes through a temp.
   Probe: invoke the reference Cg compiler (the binary under
   `reference/sony-sdk/host-win32/Cg/bin/`) under `wine` with
   `--profile sce_fp_rsx --O2 --fastmath` against the alpha-mask
   shader to capture a fresh `.fpo`.

Captured separately as session prompt
`docs/next-session-prompt-dbgfont.md`.

---

## FP optimisation gap: arithmetic-lowering for non-passthrough writes

```cg
out_color = pow(texColor.rgb * lightIntensity, 2.2.xxx);
```

Today `StoreOutput` is byte-exact for three input shapes: direct
varying read, `tex2D` result (once tex2D ships), or literal `vec4`.
Anything in between — vector arithmetic, swizzle compositions,
multi-step expressions — currently bails with the
"arithmetic lowering lands later" diagnostic.

The fix is incremental rather than a single shape: each new arith
op (`mul`, `add`, `dot`, `lerp`, etc.) lands in `nv40_fp_emit.cpp`
with a focused test under `tests/shaders/` and a byte probe from
the reference compiler to pin the encoding.  See the FENCBR /
accumulator-fold sections above for examples of how that gets
staged.

---

## How to close one of these

1. Pick the case, gather a fresh byte probe from sce-cgc to confirm
   the encoding hasn't shifted (`wine sce-cgc.exe …` then `xxd`).
2. Add a focused test shader under `tests/shaders/` that exercises
   the case end-to-end — the test contract is byte-equality at
   `--O2 --fastmath`, asserted by `tests/run_diff.sh`.
3. Implement the new branch in `nv40_fp_emit.cpp`, keeping the
   existing scaledLane=0 / Y / W / Z dispatch shapes untouched.
4. Re-run the harness to confirm 59/59 → 60/60 (or higher) without
   regressing any prior shader.
