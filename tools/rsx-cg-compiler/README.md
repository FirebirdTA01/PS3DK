# rsx-cg-compiler — Cg → RSX (NV40) compiler

Replacement for PSL1GHT's `cgcomp` that drops the NVIDIA Cg toolkit
dependency and adds full Cg shader-language support, including the
`#pragma alphakill` / `#pragma texformat` extensions that NVIDIA's
stock compiler silently ignores.

The binary is named `rsx-cg-compiler` to mirror the reference
compiler's profile names (`sce_vp_rsx`, `sce_fp_rsx`) and the sibling
front-end donor project `vita-cg-compiler`.

## Architecture

The compiler is composed from two donor projects, plus PS3-specific
back-end code authored here:

```
.cg source
    │
    ▼
┌───────────────────────────┐
│ Front-end (vita-cg donor) │   src/vita-cg-compiler/src/{frontend,ir,common}/
│   preprocessor → lexer →  │   Adopts the donor's GXM Cg compiler lexer, parser,
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

The donor front-end is **copied into `src/donor/`** in this tree.
Both projects (vita-cg-compiler and this PS3 toolchain) share the
same author + license (MIT), so adopting the donor source directly
beats a sibling-clone reference: the build is self-contained, no
bootstrap dependency, and we're free to evolve the front-end for
RSX without touching the upstream Vita repo.

## Why a separate tree from `tools/cgcomp/`

PSL1GHT's existing `tools/cgcomp/` keeps working unchanged.  We can
A/B test our output against it during development.  Once
rsx-cg-compiler reaches feature parity + container byte-compat, the
old cgcomp is retired.

## License

MIT.  Donor front-end (vita-cg-compiler) is also MIT (matching
copyright 2026 FirebirdTA01).
