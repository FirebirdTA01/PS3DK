# cgcomp-v2 — Phase 8 Cg → NV40 compiler

Replacement for PSL1GHT's `cgcomp` that drops the NVIDIA Cg toolkit
dependency and adds full Sony Cg shader-language support, including
the proprietary `#pragma alphakill` / `#pragma texformat` extensions
that NVIDIA's stock compiler silently ignores.

## Architecture

The compiler is composed from two donor projects, plus PS3-specific
back-end code authored here:

```
.cg source
    │
    ▼
┌───────────────────────────┐
│ Front-end (vita-cg donor) │   src/vita-cg-compiler/src/{frontend,ir,common}/
│   preprocessor → lexer →  │   Adopts Sony GXM Cg compiler's lexer, parser,
│   parser → AST →          │   AST, semantic analysis, IR builder, and IR
│   semantic → IR builder → │   passes.  Hardware-independent IRModule output.
│   IR optimisation passes  │
└─────────────┬─────────────┘
              │  IRModule
              ▼
┌───────────────────────────┐
│ NV40 back-end (NEW, here) │   tools/cgcomp-v2/src/nv40_*.{h,cpp}
│   IR → NV40 lowering →    │   Reads IRModule, produces nvfx_insn[] arrays
│   NV40 register alloc →   │   for fragment + vertex programs.  Implements
│   ucode emit              │   Sony pragma semantics (alphakill flag bit on
│                           │   TEX, texformat metadata, etc.).
└─────────────┬─────────────┘
              │  nvfx_insn[]
              ▼
┌───────────────────────────┐
│ Container emit (NEW, here)│   tools/cgcomp-v2/src/sony_container_*.{h,cpp}
│   nvfx_insn[] →           │   Emits Sony's CellCgbFragmentProgramConfiguration /
│   .fpo / .vpo binary      │   CellCgbVertexProgramConfiguration container —
│                           │   primary target.  Optional rsxFragmentProgram /
│                           │   rsxVertexProgram emit for PSL1GHT-runtime compat.
└───────────────────────────┘
              │
              ▼
        .fpo / .vpo
```

The donor front-end lives at `../../src/vita-cg-compiler/` (cloned by
`scripts/bootstrap.sh`).  We do NOT vendor it here — `src/` is
gitignored, regenerable from a single git clone.  This tree
(`tools/cgcomp-v2/`) holds only PS3-specific back-end code +
build glue.

## Why a separate tree from `tools/cgcomp/`

PSL1GHT's existing `tools/cgcomp/` keeps working unchanged.  We can
A/B test our output against it during Phase 8 development.  Once
cgcomp-v2 reaches feature parity + Sony-byte-compat, the old
cgcomp is retired.

## Status

- Stage 0 (this commit): repo + tree skeleton, no code yet.
- Stage 1 (next): IR builder reaches a working dump of trivial shaders
  via the donor front-end, no NV40 emit.
- Stage 2: NV40 back-end emits ucode bit-identical to Sony sce-cgc for
  simple shaders.
- Stage 3: Sony pragma semantics (alphakill + texformat first).
- Stage 4: Sony binary container.
- Stage 5: PSL1GHT runtime container update (parallel-implementation).
- Stage 6: Sony GCM sample tree compile sweep.

See `MEMORY/project_phase8_cg_compiler_direction.md` for the rolling
status notes.

## License

MIT.  Donor front-end (vita-cg-compiler) is also MIT (matching
copyright 2026 FirebirdTA01).
