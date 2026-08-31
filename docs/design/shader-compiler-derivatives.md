# Screen-space derivatives (`ddx` / `ddy` / `fwidth`) — scoping note

Status: SCOPED, not started.  Owner lane: shader compiler feature work.
Closes the last community-corpus compile failure (134/135 → 135/135 on
the general path; the sweep's one remaining row fails with an unresolved
`ddy(float)`).

## 1. What the corpus actually needs

Exactly one corpus shader uses derivatives (the GPL fetch-run-only
suite's test 56).  Its whole derivative surface is:

```c
float dx = abs(ddx(pattern));   // pattern is a scalar float
float dy = abs(ddy(pattern));
```

Scalar `float` only.  No `float3`/`float4` derivatives, no `fwidth()`
call (it hand-expands `abs(ddx)+abs(ddy)` itself).  Everything else in
that shader (`sin`, `length`, `abs`, vector construct) already compiles
— the sweep diagnostic names only the derivative call.

Implementation note: the frontend has NO `ddx`/`ddy` intrinsic today —
the only place those names appear is as *parameter names* of the
gradient-sampling `tex2D` overload (`symbol_table.cpp:583`).  The sweep
reporting only `ddy(float)` (not `ddx`) is a diagnostic-capture
artifact to verify during implementation; both are equally missing.

## 2. Hardware facts (NV40 fragment processor)

From `src/nv40/nvfx_shader.h`:

```
NVFX_FP_OP_OPCODE_DDX 0x15   /* can only write XY */
NVFX_FP_OP_OPCODE_DDY 0x16   /* can only write XY */
```

- Native instructions, fragment programs ONLY.  There is no vertex
  counterpart; `ddx` in a VP is a profile error, full stop.
- Both are component-wise per lane (lane i of the result is the
  derivative of lane i of the source), but the destination write mask
  is hardware-restricted to X and/or Y.  A `float`/`float2` derivative
  is one instruction; `float3`/`float4` needs two (second op reads the
  source through a `.zwzw`-style swizzle, writes the lanes into a
  second temp's XY, then the consumer recombines) — this is the
  standard nvfx driver scheme.
- Semantics: forward difference across the 2x2 pixel quad.  For a
  linearly interpolated varying the result is a screen constant.

## 3. Slices

**Slice 1 — scalar/2-lane (closes the corpus):**
- Frontend: register `ddx`, `ddy` stdlib intrinsics for `float` and
  `float2` (fragment profile only).
- IR: unary `Ddx`/`Ddy` ops.
- Lowering, BOTH paths (the corpus sweep runs `--general-lowering`, so
  the general path is the one that must land; the default path takes it
  only if a known-good fixture is seeded for the byte gate):
  emit the single DDX/DDY instruction; destination mask ⊆ {X, Y}.
- Widths 3–4 REJECT LOUDLY in slice 1 ("derivative width not yet
  supported") — never silent truncation.  Rejection is tier-a conform:
  a diagnostic, not resource exhaustion, not wrong output.
- `ddx`/`ddy` in a vertex profile: clean tier-a diagnostic.

**Slice 2 — full widths + `fwidth`:**
- `float3`/`float4` via the two-instruction lane-routing scheme.
- `fwidth(x)` = `abs(ddx(x)) + abs(ddy(x))` frontend expansion, all
  widths.

**Slice 3 — scheduling parity:** belongs to the optimization-study
lane, not here.

## 4. Interaction with the write-mask invariant (t_daf2da77)

The materialization fix carries the consumer's destination mask on the
assumption lanes-read == lanes-written, true for every current
`GenericFpOp` member (Dot3/Dot4 explicitly widened).  Derivatives ARE
component-wise, so a `Ddx`/`Ddy` op added to that enum keeps the
invariant — but the XY-only hardware write restriction means the
*emission* side cannot honor an arbitrary requiredMask directly and
must lane-route instead.  Whoever implements slice 2 must revisit the
`requiredMask` computation site in `nv40_fp_emit.cpp` (the Dot3/Dot4
special-case branch) and either handle derivatives there or keep them
out of the generic materialization path entirely.

## 5. Testing

- **Tier a:** fixture per width; VP-profile rejection fixture; slice-1
  width-3/4 rejection fixture (retired by slice 2 into compile
  fixtures).
- **Tier c (readback):** derivatives of linear varyings are screen
  constants, so they are exactly judgeable: with `u` spanning 0..1
  across the 64x64 RT, `ddx(u)` is 1/64 at every pixel.  A
  `rb_deriv.fcg` row writing `float4(abs(ddx(u))*32.0, abs(ddy(v))*32.0,
  0, 1)` expects flat (0.5, 0.5, 0, 1) — PPU mirror is two constants.
  Add as test 5 of the existing shader-readback battery (manifest count
  flips 4/4 → 5/5 in the same commit, per that row's convention).
- **Corpus:** the general-path sweep's last row flips; acceptance for
  slice 1 is 135/135 with `bad_alloc=0 unknown=0` and no other row
  moving.

## 6. Cost estimate

Slice 1 is small: one intrinsic registration site, one IR op pair, one
emission case per path, three fixtures, one readback row.  The lane
routing in slice 2 is the only genuinely fiddly part and nothing in the
corpus needs it today.
