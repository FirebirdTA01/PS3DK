# Optimization Architecture

How `rsx-cg-compiler` organises its optimisation levels and fast-math
behaviour, and where future optimisation passes plug in.

## Flag matrix

`rsx-cg-compiler` exposes two orthogonal compile-time knobs:

| flag                  | effect                                                |
|:----------------------|:------------------------------------------------------|
| `--fastmath` (default)| MAD fusion; register reuse; fp16 promotion of COLOR varyings |
| `--nofastmath`        | Literal expression tree; no fusion; more temps |
| `-O0 … -O3`           | Progressive optimisation tiers; `-O2` is the default |

`-O1` gates dead-code elimination and constant folding; `-O2` adds MAD
fusion, disjoint-lifetime register reuse, and common-subexpression
elimination; `-O3` also does cross-basic-block propagation and more
speculative register reuse.  Not every gate has fired yet — the test
shaders used to regression-check output are simple enough that many of
these transformations have no observable effect on them.  The gates
are in place so real-world shaders start exercising them cleanly.

The defaults were chosen to match the reference compiler's behaviour
so the byte-diff harness can compare like-for-like without either side
passing extra flags.

## The CompileOptions surface

Defined in [`src/compile_options.h`](../src/compile_options.h) under
`namespace rsx_cg`:

```cpp
struct CompileOptions {
    OptLevel opt      = OptLevel::O2;   // default optimisation level
    bool     fastmath = true;           // fast-math default ON
    bool     backFaceColorBits = false; // attributeOutputMask: front-face only
    bool     keepUnusedParams = false;  // strip unused params by default
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

Pass a default-constructed `CompileOptions{}` if you don't care.

## CLI surface

Flags are parsed in `src/main.cpp` and stored on `CompilerContext`:

```
-O0 / -O1 / -O2 / -O3       (default: -O2)
--fastmath                  (default)
--nofastmath
```

Code-gen flags like `--bcolor`, `--fkeep-unused`, `--fuse-nrmh`,
`--fearly-kills`, `--fmax-psize`, `--disablepc`, etc. aren't wired to
the CLI yet — add them as the corresponding behaviour lands in the
back-end.  The `CompileOptions` struct already has slots for them.

## Adding a new optimisation pass

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
4. Add a test shader that exercises the difference.  Give it a telling
   name (`mad_fused_f.cg`).

**Hard rule:** every new optimisation must still produce byte-exact
output against the reference compiler at the same
`(OptLevel, fastmath)` tuple.  Our diff harness is the gate;
optimisations that pass functional tests but diverge from the
reference are bugs.
