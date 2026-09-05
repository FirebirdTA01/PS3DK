# Roadmap

This project is already usable as a Linux-hosted PS3 toolchain and SDK. The remaining work is about breadth, validation, and release packaging.

## SDK surface

The SDK is moving subsystem by subsystem from compatibility shims into native headers and archives under `sdk/`. The NID/FNID database drives stub archive generation, and `docs/coverage.md` tracks export coverage against the known reference SDK library surface.

Near-term SDK work focuses on:

- finishing high-value cell headers that still block external sample imports
- continuing the network and NP sample coverage sweep
- tightening ABI checks for caller-allocated structs and cross-SPRX pointer fields
- replacing fallback PSL1GHT paths with native archives where the ABI is understood

## Toolchain

PPU uses GCC 12.4.0 today. Future PPU bumps are routine patch-set rebases across newer GCC target-hook and middle-end API changes.

SPU uses GCC 9.5.0 today because it is the last upstream GCC release with the Cell SPE backend. The forward-port track resurrects that backend for a newer GCC line, modernizes it against current backend APIs, and eventually gives PPU and SPU a unified compiler version.

## Runtime and samples

The native LV2 runtime, compact OPD path, CMake SELF helper flow, and install manifest are the default path for new samples. The sample tree continues to grow as each SDK family lands. Samples are intended to demonstrate practical build and link shapes, not to serve as a release-quality test matrix by themselves.

## Releases

Linux remains the primary development and source-build host. Windows-hosted release artifacts are produced by cross-building from Linux with Mingw-w64 and packaging the `.exe` toolchain, host tools, CMake helpers, portlibs, headers, archives, and samples into a self-contained zip.

Release mechanics and version-string rules live in `docs/VERSIONING.md`.

## Shader compiler

`tools/rsx-cg-compiler` compiles Cg to NV40 microcode for the `sce_fp_rsx` and
`sce_vp_rsx` profiles, and is the largest active workstream in the project. It
replaced its own predecessor in September 2026: the general lowering path is now
the compiler, and the retired shape matcher survives only behind
`--legacy-lowering` for differential testing.

Progress is measured on three independent axes, because a compiler can pass any
one of them while failing the other two.

- **Acceptance** — does it compile the shader at all? Measured over a corpus of
  community and in-tree Cg shaders (`docs/design/shader-compiler-testing.md`
  defines the corpus policy and its license classes). The remaining refusals are
  concentrated in control flow, matrix-valued expressions, and a handful of
  operand-resolution holes; each refusal is loud and named, never a silent
  wrong answer.
- **Correctness** — does what it compiles paint the right picture? Judged by a
  differential pixel harness that renders our output and a baseline through
  RPCS3 and compares readbacks, plus a byte-identity set that must not shrink.
- **Quality** — is the microcode worth shipping? An NV40 fragment program
  declares its temp-register count, and that count governs how many fragment
  threads the hardware keeps in flight, so a longer program that uses fewer
  registers can be the faster one. We do not yet model that trade at all: our
  optimization levels are currently a declaration rather than a behaviour, and
  our programs are both longer and register-hungrier than they need to be. This
  is the axis no gate currently enforces, and closing it is what turns the
  compiler from correct into useful.

Near-term shader work, in order:

- general-path control flow — merging multiple return blocks is the single
  largest remaining acceptance unlock (`docs/design/shader-compiler-control-flow.md`)
- matrix-valued expression plumbing, which blocks several shipped homebrew
  vertex programs
- register-pressure modelling and instruction-count reduction
  (`docs/design/shader-compiler-register-pressure.md`), which also closes the
  last refusals that are budget consequences rather than missing features
- screen-space derivatives (`docs/design/shader-compiler-derivatives.md`)
- an NV40 microcode disassembler, so that microcode-level verification is
  reproducible from this repository alone rather than depending on private
  comparison material

## Relationship to upstream PSL1GHT (deliberate divergences)

Upstream's "v3" RFC (ps3dev/PSL1GHT issue #67, 2017, still open) has four items. Three are already this project's design: the NID/FNID database with generated stub archives (`tools/nidgen`), `cell*`/`Cell*`/`CELL_*` naming for SDK-owned APIs, and `sys_*` snake_case syscalls. The remaining item is a deliberate divergence, recorded here so it is not revisited by accident:

- **Duplicated C headers.** The RFC proposes deleting PSL1GHT's copies of standard headers and pushing the implementations into newlib. This project does the opposite: `sdk/include/` overrides the vendored headers and the newlib patch set carries what the PS3 needs, because SDK-API parity (the official `cell/*` surface) will never live in upstream newlib and must be owned here.
- **Legacy names.** The RFC's position is that renamed symbols need no aliases (old code stays on the old PSL1GHT). This project keeps the legacy PSL1GHT names compiling through deprecated aliases behind a negative gate (`__PS3DK_NO_PSL1GHT_COMPAT__`), so existing homebrew builds and warns instead of breaking.
- **Vendored PSL1GHT** is pinned at `eca3f99`; upstream's in-flight NID rename (PR #169) is tracked but not chased while it is a draft.

The remaining v3 must-have, SPRX/PRX module generation, is implemented: `tools/prx-gen` and `tools/sprx-linker` produce the modules, `docs/design/sprx-generation.md` records the format work behind them, and the regression battery boots generated `.sprx` rows.
