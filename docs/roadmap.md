# Roadmap

This project is already usable as a PS3 toolchain and SDK on Linux and, through the Windows SDK zip, on Windows. The remaining work is about breadth, validation, the compiler transition, and a Linux SDK tarball to match the Windows zip.

## SDK surface

The SDK is moving subsystem by subsystem from compatibility shims into native headers and archives under `sdk/`. The NID/FNID database drives stub archive generation, and `docs/coverage.md` tracks export coverage against the known platform ABI library surface.

Near-term SDK work focuses on:

- finishing high-value cell headers that still block external sample imports
- continuing the network and NP sample coverage sweep
- tightening ABI checks for caller-allocated structs and cross-SPRX pointer fields
- replacing fallback PSL1GHT paths with native archives where the ABI is understood

## Toolchain

PPU uses GCC 16.1.0 (`patches/ppu/gcc-16.x`); earlier releases shipped GCC 12.4.0. The transition plan is in [design/gcc-transition.md](design/gcc-transition.md).

SPU uses GCC 9.5.0 today because it is the last upstream GCC release with the Cell SPE backend. The forward-port track, a later stage of the same plan, resurrects that backend for GCC 16, modernizes it against current backend APIs, and gives PPU and SPU a unified compiler version.

## Runtime and samples

The native LV2 runtime, compact OPD path, CMake SELF helper flow, and install manifest are the default path for new samples. The sample tree continues to grow as each SDK family lands. Samples are intended to demonstrate practical build and link shapes, not to serve as a release-quality test matrix by themselves.

## Releases

Linux remains the primary development and source-build host. Windows-hosted release artifacts are produced by cross-building from Linux with Mingw-w64 and packaging the `.exe` toolchain, host tools, CMake helpers, portlibs, headers, archives, and samples into a self-contained zip.

Release mechanics and version-string rules live in `docs/VERSIONING.md`.

## Shader compiler

`tools/rsx-cg-compiler` compiles Cg to NV40 microcode for the `sce_fp_rsx` and
`sce_vp_rsx` profiles, and is the largest active workstream in the project. It
replaced its own predecessor in September 2026: the general lowering path is now
the compiler, and the retired shape matcher was removed on 2026-10-01;
`--legacy-lowering` now fails with a message saying so.

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
  is the axis a gate holds at no-regression but no gate yet drives toward the
  established compiler's counts, and closing it is what turns the compiler from correct
  into useful.

The items that headed this list have moved. Control flow merges multiple return
blocks; matrix-valued expressions compile; derivatives landed as a partial, with
one precision question still open. On register pressure, demand scheduling keeps
register counts bounded on the measured fragment accumulation families and
removed the two measured register-budget refusals — it triggers only on
fragment accumulator chains of at least three members whose intermediates have a
single use, and it can lengthen the lifetime of a value shared with an unrelated
consumer, so broader register-pressure modelling remains open.

### The bar for a release

The goal is that this compiler can replace the established compiler for every
shader this project ships, even where it produces less well optimized microcode.
Acceptance is the near-term bar and it is largely met: every shipped sample and
SDK shader compiles and renders identically on colour 0. The remaining refusals
— including four of this project's own regression fixtures — are named shapes
the system accepts, tracked individually rather than as a percentage.

The harder half is verification, so the gate is:

- every shader the system compiles, compiles here
- every shader this project ships renders identically, across **all declared
  outputs** — colour targets beyond the first, and depth, not only colour 0
- remaining single-LSB differences either eliminated or accepted individually
- a spot check on real hardware for a named set, since emulator-based judging
  cannot decide fixed-point precision, and multi-output comparison is a new
  instrument that deserves one hardware confirmation before its verdicts are
  trusted

Byte-for-byte equality with the system is explicitly **not** part of that
bar. It is tracked separately as a quality signal, because a shader that matches
exactly needs no pixel judgement at all.

Near-term shader work, in order:

- judging **every declared output**, not only colour 0. This is the instrument
  that stands between "compiles" and "replaces the system": a wrong depth
  export survived for as long as the compiler had existed because nothing
  rendered depth to compare it
- multiple render targets, which cannot be called complete before that
  instrument exists
- the remaining shapes the system compiles and this compiler refuses
- the last single-LSB pixel differences, each attributed to a named mechanism
  rather than to precision in general
- an NV40 microcode disassembler, so that microcode-level verification is
  reproducible from this repository alone rather than depending on private
  comparison material
- instruction-count and register-count reduction, the quality axis above. A gate
  holds these at no-regression against a recorded baseline; no gate yet drives
  them toward the system's counts

## Relationship to upstream PSL1GHT (deliberate divergences)

### Why this was never an upstream branch

When this project started, PSL1GHT upstream looked dormant to us: we saw no maintainer activity, its "v3" RFC (ps3dev/PSL1GHT issue #67, opened in 2017) was still unaddressed, and the ps3dev toolchain was still on GCC 7.2.0, binutils 2.22 and newlib 1.20.0. We judged that a patch series of this size (a modern compiler, a new runtime, a different pointer model) would have no realistic path to review there, so the SDK was built as its own project, with PSL1GHT vendored as a source of homebrew-compatible APIs rather than as a base to patch.

Upstream has since become active again: pull requests and discussions are moving (the NID rename in PR #169, the C port of the `sfo`/`pkg` tools), and ps3toolchain has moved its PPU compiler to GCC 13.2. By then the two code bases had diverged in the ways below, and this project chose to keep its own foundations rather than rebase onto upstream's.

### Divergences

Upstream's v3 RFC has four items. Three are already this project's design: the NID/FNID database with generated stub archives (`tools/nidgen`), `cell*`/`Cell*`/`CELL_*` naming for SDK-owned APIs, and `sys_*` snake_case syscalls. These choices separate the projects:

- **Canonical API.** The SDK's API and ABI follow the official PS3 SDK (`cell/*`), and PSL1GHT names are kept as thin compatibility aliases over this project's implementation, behind the negative gate `__PS3DK_NO_PSL1GHT_COMPAT__`, so existing homebrew builds and warns instead of breaking. The 2017 RFC proposed no aliases (old code stays on old PSL1GHT); current upstream work plans compatibility aliases behind a negative gate too (PR #169 and its review), so the gate itself is shared ground. The difference is the direction: PR #169 renames PSL1GHT's own types and functions in place, while this project wraps the PSL1GHT names in its own `cell` layer, and the two collide.
- **Replace rather than patch.** Where PSL1GHT code is wrong for the official ABI, the intended direction is to replace it with code this project owns (the native LV2 runtime in `runtime/lv2`, nidgen-generated stubs, `sdk/` libraries) instead of patching it. `patches/psl1ght/` still receives targeted fixes where replacement is not yet practical.
- **Pointer model and object format.** PPU code is ELF64 with 32-bit pointers by default (ILP32, compact 8-byte `.opd` descriptors), with LP64 available through `-mlp64` (`docs/abi/cellos-lv2-abi-spec.md`). PSL1GHT and the ps3dev toolchain produce LP64 objects with classic 24-byte descriptors. Objects and archives are not interchangeable between the two.
- **Compiler, binutils and C library.** PPU GCC 16.1, binutils 2.42 for both PPU and SPU, newlib 4.4 with its own PS3 glue (locks, start-up order, LV2 errno values), and PPU GDB 14. Each carries a patch series written for this project's ABI and runtime, against a different base from upstream's GCC 13.2 / binutils 2.22 / newlib 1.20.
- **Runtime.** The CRT and `librt` (heap, POSIX layer, pthreads over the LV2 primitives, start-up ladder) are this project's own, replacing PSL1GHT's.
- **Header ownership.** The RFC proposes deleting PSL1GHT's copies of standard headers and leaving the standard C/POSIX surface to newlib. For standard declarations this project is moving the same way: the declarations the runtime implements (pthreads, `clock_gettime`, `nanosleep`, scheduling) are moving into the toolchain's newlib, with the SDK's wrappers kept only as fallbacks for older toolchains. The `cell/*` headers are a different matter: SDK-API parity will never live in upstream newlib, so those stay owned by `sdk/include/`.
- **Shaders.** `tools/rsx-cg-compiler` is an independent Cg compiler targeting the official output format; PSL1GHT's `cgcomp` is bundled only for old Makefiles.
- **Vendored PSL1GHT** is pinned at `f649a08` (`scripts/bootstrap.sh`), upstream master of 2026-06-29. Upstream changes are reviewed and adopted selectively; PR #169 is tracked but not chased while it is a draft.

The remaining v3 must-have, SPRX/PRX module generation, is implemented: `tools/prx-gen` and `tools/sprx-linker` produce the modules, `docs/design/sprx-generation.md` records the format work behind them, and the regression battery boots generated `.sprx` rows.

### What could still go upstream

This project does not plan to upstream its architecture: the pointer model, the runtime, the toolchain patch series and the canonical API direction are each its own foundation, and offering them would mean a different project rather than a patch. Bugs found in code both projects still share are candidates to report or fix upstream individually, each needing upstream-specific adaptation and review rather than a copy of this project's ABI-specific fix: the `rsxContextCallback` inline assembly, which lacks memory and call clobbers (a FIFO-wrap hang under GCC 11 and later); `__heap_block_prev` in `librt/heap.inl`, which adds `prev_size` instead of subtracting it; and `sbrk` failure paths that leave `errno` stale. Offering each is a separate decision.
