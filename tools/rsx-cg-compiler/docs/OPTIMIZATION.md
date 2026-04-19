# Optimization Architecture

How `rsx-cg-compiler` models `sce-cgc`'s compile-time knobs, and where
future optimization passes plug in.

## Today's reality (probed 2026-04-18, SDK 475.001)

Of the ~60 flags `sce-cgc --help` documents, exactly **one** actually
changes output bytes on the shaders we currently lower:

| flag                  | effect                                    |
|:----------------------|:------------------------------------------|
| `--fastmath` (default)| MAD fusion; register reuse; fp16 promotion of COLOR varyings |
| `--nofastmath`        | Literal expression tree; no fusion; more temps |

Everything else — `--O0`/`--O1`/`--O2`/`--O3`, `--partial-reg-alloc`,
`--reduce-dp-size`, `--noreduce-dp-size` — produced byte-identical
output on both simple and complex FP probes and on VP across the
board.  That's NOT because they're broken; it's because our test
shaders don't exercise the optimisation paths those flags gate.

The shape will change as we add **real-world** shader lowering:

- Dead code elimination needs a shader with unused IR to prune (O1).
- Register reuse needs shaders with disjoint temp lifetimes (O2).
- Cross-basic-block propagation needs branching / conditional code (O3).
- Loop unrolling / function inlining need, well, loops + functions.

Keep the `OptLevel` axis intact; the gates just haven't fired yet.

## The CompileOptions surface

Defined in [`src/compile_options.h`](../src/compile_options.h) under
`namespace rsx_cg`.  Defaults match sce-cgc's defaults so the
byte-diff harness compares like-for-like without either side passing
extra flags.

```cpp
struct CompileOptions {
    OptLevel opt      = OptLevel::O2;   // sce-cgc default
    bool     fastmath = true;           // default ON
    bool     backFaceColorBits = false; // attributeOutputMask: front-face only
    bool     keepUnusedParams = false;  // sce-cgc strips unused by default
    bool     useNrmh           = true;
    bool     earlyKills        = true;
    bool     maxPsizeWorkaround= true;
    // ... future: texsign, texformat, inline, unroll, ifcvt
};
```

All four emit entry points take a `const rsx_cg::CompileOptions&`:

```cpp
nv40::emitFragmentProgram  (module, entry, opts);
nv40::emitFragmentProgramEx(module, entry, opts);  // + FpAttributes
nv40::emitVertexProgram    (module, entry, opts);
nv40::emitVertexProgramEx  (module, entry, opts);  // + VpAttributes
```

Pass a default-constructed `CompileOptions{}` if you don't care —
it's equivalent to running sce-cgc with no flags.

## CLI surface

Flags are parsed in `src/main.cpp` and stored on `CompilerContext`:

```
-O0 / -O1 / -O2 / -O3       (default: -O2)
--fastmath                  (default)
--nofastmath
```

sce-cgc's code-gen flags (`--bcolor`, `--fkeep-unused`, `--fuse-nrmh`,
`--fearly-kills`, `--fmax-psize`, `--disablepc`, etc.) aren't wired to
the CLI yet — add them as the corresponding behaviour lands in the
back-end.  The `CompileOptions` struct already has slots for them.

## Adding a new optimization pass

1. Add the gate to `CompileOptions` if a finer toggle is needed; for
   most passes just reading `opts.opt` + `opts.fastmath` is enough.
2. Write the pass in the appropriate back-end file (`nv40_vp_emit.cpp`
   for VP, `nv40_fp_emit.cpp` for FP, or the container layer for
   param-table shaping).
3. Gate it:
   ```cpp
   if (rsx_cg::atLeast(opts.opt, rsx_cg::OptLevel::O2) && opts.fastmath) {
       fuseMulAddIntoMad(...);
   }
   ```
4. Add a test shader that exercises the difference.  Give it a
   telling name (`mad_fused_f.cg`) and make sure the diff harness
   invokes sce-cgc with matching flags.

**Hard rule:** every new optimization must still produce byte-exact
output against sce-cgc at the same `(OptLevel, fastmath)` tuple.
Our diff harness is the gate; optimizations that pass functional
tests but diverge from sce-cgc are bugs.

## The test contract

`tests/run_diff.sh` pins against `--O2 --fastmath` on **both** sides
(our compiler and sce-cgc).  This is documented at the top of the
script via the `OPT_FLAGS` array.

When we add real-shader tests that need to exercise a different level
(e.g. a `mad_fused_f.cg` written to work at both `-O0` and `-O2` and
validate that the lowering differs), the harness should grow a
per-shader override — probably a sidecar `.flags` file or a naming
convention.  That mechanism doesn't exist yet; the current `OPT_FLAGS`
is a repo-wide knob.

## Why no `--fastmath` behaviour implemented yet

Every shader in `tests/shaders/` today is simple enough that sce-cgc
produces identical output at `--fastmath` and `--nofastmath`.  We
match sce-cgc at `--fastmath` by construction because that's what
sce-cgc emits as the baseline.  When we land the first shader that
DOES differ (e.g. a 3-term `MUL+ADD` expression sce-cgc fuses into a
MAD), implementing the `fastmath`-gated MAD fusion is a correctness
requirement, not just an optimization.
