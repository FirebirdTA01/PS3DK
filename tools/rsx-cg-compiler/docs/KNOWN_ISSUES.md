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
