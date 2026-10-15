# Roadmap

This project is already usable as a PS3 toolchain and SDK on Linux and, through the Windows SDK zip, on Windows. The remaining work is about breadth, validation, the compiler transition, and a Linux SDK tarball to match the Windows zip.

## SDK surface

The SDK is moving subsystem by subsystem from compatibility shims into native headers and archives under `sdk/`. The NID/FNID database drives stub archive generation, and `docs/coverage.md` tracks export coverage against the known reference SDK library surface.

Near-term SDK work focuses on:

- finishing high-value cell headers that still block external sample imports
- continuing the network and NP sample coverage sweep
- tightening ABI checks for caller-allocated structs and cross-SPRX pointer fields
- replacing fallback PSL1GHT paths with native archives where the ABI is understood

## Toolchain

PPU uses GCC 12.4.0 today. The approved next step is GCC 16 ([design/gcc-transition.md](design/gcc-transition.md)): the patch series is ported (`patches/ppu/gcc-16.x`) and the work continues on the `feat/gcc16` branch until a switchover release.

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
  reference's counts, and closing it is what turns the compiler from correct
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

The goal is that this compiler can replace the reference compiler for every
shader this project ships, even where it produces less well optimized microcode.
Acceptance is the near-term bar and it is largely met: every shipped sample and
SDK shader compiles and renders identically on colour 0. The remaining refusals
— including four of this project's own regression fixtures — are named shapes
the reference accepts, tracked individually rather than as a percentage.

The harder half is verification, so the gate is:

- every shader the reference compiles, compiles here
- every shader this project ships renders identically, across **all declared
  outputs** — colour targets beyond the first, and depth, not only colour 0
- remaining single-LSB differences either eliminated or accepted individually
- a spot check on real hardware for a named set, since emulator-based judging
  cannot decide fixed-point precision, and multi-output comparison is a new
  instrument that deserves one hardware confirmation before its verdicts are
  trusted

Byte-for-byte equality with the reference is explicitly **not** part of that
bar. It is tracked separately as a quality signal, because a shader that matches
exactly needs no pixel judgement at all.

Near-term shader work, in order:

- judging **every declared output**, not only colour 0. This is the instrument
  that stands between "compiles" and "replaces the reference": a wrong depth
  export survived for as long as the compiler had existed because nothing
  rendered depth to compare it
- multiple render targets, which cannot be called complete before that
  instrument exists
- the remaining shapes the reference compiles and this compiler refuses
- the last single-LSB pixel differences, each attributed to a named mechanism
  rather than to precision in general
- an NV40 microcode disassembler, so that microcode-level verification is
  reproducible from this repository alone rather than depending on private
  comparison material
- instruction-count and register-count reduction, the quality axis above. A gate
  holds these at no-regression against a recorded baseline; no gate yet drives
  them toward the reference's counts

## Relationship to upstream PSL1GHT (deliberate divergences)

### Why this was never an upstream branch

When this project started, PSL1GHT upstream was effectively dead: no maintainer activity, its "v3" RFC (ps3dev/PSL1GHT issue #67, opened in 2017) still unaddressed, and the ps3dev toolchain still on GCC 7.2.0, binutils 2.22 and newlib 1.20.0. There was nobody to review a patch series, and the work that was needed (a modern compiler, a new runtime, a different pointer model) was not something a dormant project could take incrementally. So the SDK was built as its own project, with PSL1GHT vendored as a source of homebrew-compatible APIs rather than as a base to patch.

Upstream has since come back to life: pull requests and discussions are moving again (the NID rename in PR #169, the C port of the `sfo`/`pkg` tools), and ps3toolchain moved its PPU compiler to GCC 13.2 in 2026. By then the two code bases had diverged too far for this project to become an upstream branch, for the reasons below.

### Divergences

Upstream's v3 RFC has four items. Three are already this project's design: the NID/FNID database with generated stub archives (`tools/nidgen`), `cell*`/`Cell*`/`CELL_*` naming for SDK-owned APIs, and `sys_*` snake_case syscalls. Beyond the RFC, these choices separate the projects, each on purpose:

- **Canonical API.** The SDK's API and ABI follow the official PS3 SDK (`cell/*`); PSL1GHT names are kept as thin compatibility aliases over our implementation, behind the negative gate `__PS3DK_NO_PSL1GHT_COMPAT__`, so existing homebrew builds and warns instead of breaking. Upstream's RFC position is the opposite (renamed symbols get no aliases), and PR #169 renames PSL1GHT's own surface in place, which collides with our `cell` layer.
- **Replace, don't patch.** Where PSL1GHT code is wrong for the official ABI, it is replaced by code this project owns (the native LV2 runtime in `runtime/lv2`, nidgen-generated stubs, `sdk/` libraries) rather than patched. `patches/psl1ght/` only shrinks from here.
- **Pointer model and object format.** PPU code is ELF64 with 32-bit pointers by default (ILP32, compact 8-byte `.opd` descriptors), with LP64 available through `-mlp64` (`docs/abi/cellos-lv2-abi-spec.md`). PSL1GHT and the ps3dev toolchain produce LP64 objects with classic 24-byte descriptors. Objects and archives are not interchangeable between the two.
- **Compiler, binutils and C library.** PPU GCC 12.4 with GCC 16 as its approved successor, binutils 2.42 for both PPU and SPU, newlib 4.4 with its own PS3 glue (locks, start-up order, LV2 errno values), and PPU GDB 14. Each carries a patch series written for this project's ABI and runtime; none applies to upstream's GCC 13.2 / binutils 2.22 / newlib 1.20 stack.
- **Runtime.** The C runtime, `librt` (heap, POSIX layer, pthreads over the LV2 primitives, start-up ladder) and the CRT are this project's own, replacing PSL1GHT's.
- **Duplicated C headers.** The RFC proposes deleting PSL1GHT's copies of standard headers and pushing the implementations into newlib. This project does the opposite: `sdk/include/` overrides the vendored headers and the newlib patch set carries what the PS3 needs, because SDK-API parity (the `cell/*` surface) will never live in upstream newlib and must be owned here.
- **Shaders.** `tools/rsx-cg-compiler` is an independent Cg compiler targeting the official output format; PSL1GHT's `cgcomp` is bundled only for old Makefiles.
- **Vendored PSL1GHT** is pinned at `f649a08` (`scripts/bootstrap.sh`), upstream master of 2026-06-29. Upstream changes are reviewed and adopted selectively; PR #169 is tracked but not chased while it is a draft.

The remaining v3 must-have, SPRX/PRX module generation, is implemented: `tools/prx-gen` and `tools/sprx-linker` produce the modules, `docs/design/sprx-generation.md` records the format work behind them, and the regression battery boots generated `.sprx` rows.

### What can still go upstream

The architecture cannot: the pointer model, the runtime, the toolchain patch series and the API direction are each a different foundation, not a change upstream could merge. Self-contained bug fixes in code both projects still share can be offered back individually, for example the `rsxContextCallback` inline assembly that lacks memory clobbers (a FIFO-wrap hang under GCC 11 and later), `__heap_block_prev` in `librt/heap.inl`, which steps forward to find the previous block, and `sbrk` failures that leave a stale `errno`. Offering them is a separate decision for each fix.
