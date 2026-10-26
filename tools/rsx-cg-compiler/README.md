# rsx-cg-compiler — Cg → RSX (NV40) compiler

Replacement for PSL1GHT's `cgcomp` that drops the NVIDIA Cg toolkit
dependency and adds full Cg shader-language support, including the
`#pragma alphakill` / `#pragma texformat` extensions that NVIDIA's
stock compiler silently ignores.

The binary is named `rsx-cg-compiler` to mirror the reference
compiler's profile names (`sce_vp_rsx`, `sce_fp_rsx`) and its sibling
project, FirebirdTA01's open-source Vita Cg compiler
(`Vita-Cg-Compiler`), whose front end it uses.

## PSGL fixed-function shader library

    rsx-cg-compiler --psgl-ffp-library state.txt -o shaders.bin

builds the library `psglLoadShaderLibrary` loads. `state.txt` holds one
fixed-function state mask per line (any `strtoul` base-0 number; `#`
starts a comment, blank lines are skipped). For each mask the vertex and
fragment Cg come from libPSGL's own generator (`sdk/libPSGL/src/ffp_emit.c`,
compiled into this binary), and both programs are compiled in process, so
each container is the one `-p sce_vp_rsx` / `-p sce_fp_rsx` with
`--emit-container` would write. The file is `PSGLSHDR`, a big-endian
version and count, then per mask its value, the two sizes and the two
containers. A malformed line, an unsupported mask or a failed compile exits
1 and writes nothing; a bad command line exits 2. The option must come
first.

`psgl-shader-builder`, which did the same by running this compiler twice
per mask, is now a deprecated wrapper around this mode and will be removed
after one release.

## Architecture

The compiler is FirebirdTA01's Vita Cg compiler front end plus a
PS3-specific back end written here:

```
.cg source
    │
    ▼
┌───────────────────────────┐
│ Front-end (Vita Cg comp.) │   tools/rsx-cg-compiler/src/cg_frontend/{frontend,ir,common}/
│   preprocessor → lexer →  │   The Vita Cg compiler's lexer, parser,
│   parser → AST →          │   AST, semantic analysis, IR builder, and IR
│   semantic → IR builder → │   passes.  Hardware-independent IRModule output.
│   IR optimisation passes  │
└─────────────┬─────────────┘
              │  IRModule
              ▼
┌───────────────────────────┐
│ NV40 back-end (NEW, here) │   tools/rsx-cg-compiler/src/nv40/
│   IR → NV40 lowering →    │   Reads IRModule, produces nvfx_insn[] arrays
│   NV40 register alloc →   │   for fragment + vertex programs.  Implements
│   ucode emit              │   Cg pragma semantics (alphakill flag bit on
│                           │   TEX, texformat metadata, etc.).
└─────────────┬─────────────┘
              │  nvfx_insn[]
              ▼
┌───────────────────────────┐
│ Container emit (NEW, here)│   tools/rsx-cg-compiler/src/cg_container_*.{h,cpp}
│   nvfx_insn[] →           │   Emits CellCgbFragmentProgramConfiguration /
│   .fpo / .vpo binary      │   CellCgbVertexProgramConfiguration container —
│                           │   primary target.  Optional rsxFragmentProgram /
│                           │   rsxVertexProgram emit for PSL1GHT-runtime compat.
└───────────────────────────┘
              │
              ▼
        .fpo / .vpo
```

The front end is from **FirebirdTA01's Vita Cg compiler**; both projects
are MIT.  It contains no third-party or proprietary compiler code.  It lives
in this tree (`src/cg_frontend/`) rather than as a separate clone, so the
build is self-contained, needs no bootstrap step, and the front end can change
for RSX without touching the Vita repo.

## Why a separate tree from `tools/cgcomp/`

PSL1GHT's existing `tools/cgcomp/` keeps working unchanged.  We can
A/B test our output against it during development.  Once
rsx-cg-compiler reaches feature parity + container byte-compat, the
old cgcomp is retired.

## License

MIT.  The front end from the Vita Cg compiler is also MIT (copyright 2026
FirebirdTA01).
