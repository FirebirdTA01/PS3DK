# rsx-cg-compiler — Known Issues

Tracked deferrals where we understand the encoding but haven't yet
shipped emitter coverage.  Each entry names a specific reference
shape, what data we have on it, and what's needed to close the gap.

The byte-diff harness at `tests/run_diff.sh` is the authoritative
gate; everything below is something we've verified the reference
compiler does that we currently bail on with a diagnostic, not a
silent miscompilation.

---

## Repeated-add scale fold capped at scale = 3

The `valueToScale` deferred binding (in `nv40_fp_emit.cpp`) collapses
chains of `Add(x, x …)` over a single varying into a single MOVH +
MAD pair (`MAD H0 * (N-1).xxxx + H0`).  Capped at `kMaxScale = 3`.

**What happens at N ≥ 4**: the reference compiler switches to a
4-instruction shape using a different scheduler path:

```
MOVH H0.xyzw, f[COL0]
MUL  R0.xyzw, H0, c[2.0].xxxx       # MUL not MAD on this link
FENCBR                              # opcode 0x3E + OUT_NONE
MAD  R0.xyzw [END], H0, c[(N-1).0].xxxx, R0
```

Verified against `vcol+vcol+vcol+vcol` (4-copy probe at
`/tmp/rsx-cg-chain-probe/quad_vcol_f.fpo`).  Adding a fourth-link
extension to `valueToScale` would need a new emit branch that picks
between the current MOVH+MAD shape (N ≤ 3) and the 4-insn
MUL+FENCBR+MAD shape (N ≥ 4).

Today the seed/extend logic in the Add walker just refuses to grow
past `kMaxScale = 3`, so `vcol + vcol + vcol + vcol` falls through
the existing arithmetic-chain check and ends up in the chain-add path
(which then fails because the chain wants uniforms not a repeated
varying).  Result: a clear diagnostic, no miscompilation.

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
