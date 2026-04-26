# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(pre-1.0: minor bumps may include breaking changes; patch bumps are
backward-compatible fixes within the same minor line).

The version stamped into builds is generated from the most recent
`vMAJOR.MINOR.PATCH` git tag plus commits-since-tag — see
[docs/VERSIONING.md](docs/VERSIONING.md) and `scripts/version.sh`.

## [Unreleased]

<!-- New entries go here while work is in progress; promote them to a
     dated, version-tagged section at release time. -->

## [v0.3.0] — 2026-04-26

### Sample build system — Makefile → CMake

Every sample under `samples/` now builds via standalone CMake projects;
the per-sample PSL1GHT-style Makefiles + `ppu_rules` / `spu_rules`
include path are gone.  Highlights:

- New `cmake/ps3-ppu-toolchain.cmake` and `cmake/ps3-spu-toolchain.cmake`
  cross-compilation toolchain files.  Each reads `$PS3DEV` / `$PS3DK`
  from the environment, points CMake at the cross-compiler, sets
  defaults that match the legacy `MACHDEP` flag set, and adds the
  SDK include / library search paths.
- New `cmake/ps3-self.cmake` helper module exposing four primitives:
  - `ps3_add_self(<target>)` — strip → sprxlinker → make_self / fself
    post-build chain.  Final `.elf` / `.self` / `.fake.self` artefacts
    land at the sample source directory next to `CMakeLists.txt`,
    matching the legacy Makefile placement.  Build intermediates stay
    in `cmake-build/`.
  - `ps3_bin2s(<target> <file>)` — embed an arbitrary binary blob into
    the target via the `bin2s` host tool plus a generated header that
    declares the three externs (`<id>` / `<id>_end` / `<id>_size`).
  - `ps3_add_spu_image(<target> NAME <name> SOURCES <files…>
    [LIBS <libs…>])` — compiles SPU sources via `spu-elf-gcc`, embeds
    the resulting LS image into the PPU executable, and surfaces it
    under the `<name>_bin*` symbol set.  Replaces the legacy `spu/`
    sub-Makefile pattern.
  - `ps3_add_cg_shader(<target> <file>)` — drives `cgcomp` for `.vcg`
    / `.fcg` shaders, then embeds the compiled blob.
- 39 samples ported, including the GCM Cg-shader and Spurs-task
  samples that previously depended on bespoke Makefile-only data and
  SPU sub-trees.
- README, samples README, and the direct-toolchain (no-CMake)
  fallback path all refreshed for the new build flow.

### Windows-host toolchain release pipeline

End-to-end cross-build of the PPU + SPU toolchains from Linux to
`x86_64-w64-mingw32` is now wired into the release workflow:

- New `--host=x86_64-w64-mingw32` mode for
  `scripts/build-ppu-toolchain.sh` and `scripts/build-spu-toolchain.sh`.
  The first pass builds the native Linux compiler so target libraries
  (libgcc, newlib, libstdc++) are available; the second pass reuses
  those target libraries while re-bootstrapping a Windows-hosted
  binutils + GCC + GDB.
- New `scripts/build-host-tools-windows.sh` cross-builds the host
  utilities (`make_self`, `make_self_npdrm`, `package_finalize`,
  `fself`, `rsx-cg-compiler`, plus their static `zlib` / `gmp` /
  `openssl` deps) for Windows.
- New `scripts/package-windows-release.sh` collects the
  Linux-cross-built compilers, the Windows-cross-built host tools,
  and the SDK runtime into a self-contained
  `ps3-sdk-vX.Y.Z-windows-x86_64.zip` plus a `setup.cmd` that exports
  `%PS3DEV%` / `%PS3DK%` / `%PSL1GHT%` and prepends `bin\`,
  `ppu\bin\`, and `spu\bin\` to `%PATH%`.
- `release.yml` grew a `build-toolchain-windows` job (with optional
  opt-in via the `run_toolchain_windows` boolean for non-tag
  `workflow_dispatch` runs) that drives the full cross-build +
  packaging flow on tag pushes.  Native Linux PPU/SPU toolchain
  builds split into two parallel CI jobs sharing a `ccache` cache;
  binutils-mingw and gcc-mingw run as discrete jobs whose outputs
  the `build-toolchain-windows` job stitches together.

### `rsx-cg-compiler` — shader byte-match coverage

The byte-diff suite against `sce-cgc` at `--O2 --fastmath` grows from
59/59 to **65/65** byte-identical:

- **Repeated-add scale-fold (full vec4)** generalises the pattern
  matcher to N=2..64.  The N≤3 path keeps the short MOVH+MAD shape;
  longer chains lower to `MOVH H0; MULR R1 = H0*2; (MADR R1 += H0*2)
  × (K-1); [FENCBR if K odd]; (ADD/MAD) R0[END]`.  Final write picks
  ADD vs MAD by carry parity.  Scalar-lane N≥4 stays capped at 3 —
  `sce-cgc` switches to a DP2R shape there which is documented in
  `KNOWN_ISSUES.md`.
- **Multi-instruction if-only-with-default + N-insn THEN diamonds**
  predicate end-to-end via a new `IROp::PredCarry`.
  `nv40_if_convert` Shape 3 detects the `entry{stout default; brc}
  → then{(compute, stout) × N} → ret` shape and synthesises a chain
  of `PredCarry` ops; the FP emit handler walks the chain, emits
  the SGTRC compare + per-link `MOVR carry; <OP>(NE.x)` pair,
  alternates dst R-temps to land at R0, picks the H-promote register
  to dodge the R0 alias, and inserts FENCBR before the last
  predicated OP for chain length ≥ 2.

### Build correctness — runtime fixes

- `cmake/ps3-ppu-toolchain.cmake` pre-links `librt` so libc's
  `init_metalock` ctor runs before librt's `__glob_file_init` ctor.
  Both share `__attribute__((constructor(105)))` and the legacy
  Makefiles got the right order from explicit `-lrt` in `$(LIBS)`;
  the CMake path inherited only the GCC spec's `--start-group`
  injection, which pulled libc first and made the init path call
  `strdup` → `malloc` → `__libc_auto_lock_allocate` on a still-zero
  metaLock → `abort()`.  Fix lands `init_metalock` at the correct
  `.ctors` slot.
- `cmake/ps3-ppu-toolchain.cmake` drops `-Wl,--gc-sections`.  The
  flag stripped BSS arrays the GCM/sysutil callback path reaches
  via 32-bit EAs (`cellGcmSetFlipHandler` etc.) — the linker has no
  static reference to trace, so the sections were silently
  collected.  PSL1GHT's `ppu_rules` never passed `--gc-sections` for
  the same reason; the SPU side keeps it (LS-image case is bounded).
  Visible symptom on `hello-ppu-cellgcm-cube`: `.bss` collapsed from
  0x40638 to 0x538, and the SELF black-screened in RPCS3.
- `samples/spu/hello-spu` SPU side now reads
  `sysSpuThreadArgument.arg0` (passed via `$3`) instead of `arg1`
  (`$4`) for the done-flag EA, and exits via libsputhread's
  `sys_spu_thread_exit()` — which traps with the LV2-recognised
  `stop 0x102` — instead of the spu-elf default crt0's `stop
  0x2000`, which RPCS3 rejects as a fatal STOP code.
- `samples/sysutil/hello-ppu-png` `configure_file()`s
  `sdk/assets/ICON0.PNG` into `pkg_files/USRDIR/ps3dk.png` at
  CMake-generation time, so direct-boot of the `.self` resolves the
  asset (which the legacy Makefile pkg-build chain copied as a side
  effect, lost in the CMake migration).
- `sdk/libgcm_sys_legacy` callback trampoline saves the caller's
  TOC via the PPC64 ELFv1 stack save area instead of clobbering
  `r31`.  The old form lost the TOC when the wrapped GCM callback
  itself made a function call.
- `scripts/build-ppu-toolchain.sh` patches the bundled GDB
  py-readline build for Python 3.13+ hosts (where the previous
  `_PyOS_ReadlineTState` symbol moved).

### SDK runtime install layout

- `$PS3DK` is now the unified runtime install root for everything
  PSL1GHT and our own SDK ship: `ppu/`, `spu/`, `bin/`, `lib/`,
  `include/`, `samples/`, plus a top-level `setup.cmd`.  The legacy
  `$PSL1GHT` directory remains as a back-compat alias for downstream
  homebrew that still reads it.
- New `scripts/build-runtime-lv2.sh` step inside
  `scripts/build-psl1ght.sh` builds the LV2 CRT objects (`lv2-crt0.o`
  / `lv2-crti.o` / `lv2-crtn.o` / `lv2-sprx.o`) and the linker
  script (`lv2.ld`) into `$PS3DK/ppu/lib/` so the GCC driver's
  `STARTFILE_LV2_SPEC %s` search resolves them ahead of any
  PSL1GHT-stock copies still on disk.
- Default sample `ICON0.PNG` redrawn.

### CI / release infrastructure

- `release.yml` honours a manual workspace-version override on
  `workflow_dispatch` runs so test releases tagged off branches
  don't fail the version-stamp check.
- MSYS2 setup retries up to 3× on transient HTTP 502s during the
  Windows host-tools build.
- Portlibs build falls back through three zlib mirrors (`zlib.net`,
  GitHub release, Sourceforge) so a transient one-mirror outage no
  longer aborts the SDK build.
- `release.yml` pins `CC=<host>-gcc` / `CXX=<host>-g++` alongside
  `CC_FOR_BUILD=gcc` / `CXX_FOR_BUILD=g++` for the mingw cross-build,
  defeating the workflow's global `CC=ccache gcc` setting that
  otherwise leaked into the cross-host probe and made
  `binutils --host=mingw` configure error out at "C compiler cannot
  create executables".
- Bootstrap `git clone` no longer uses `--filter=blob:none` (saves
  the periodic re-clone when later `git archive` calls hit the
  partial-clone slow path).
- `apt` / `dnf` / `pacman` host-deps lists refreshed: `libssl-dev`,
  `libelf-dev` for PSL1GHT host tools.
- Shell scripts marked executable in the git index so fresh clones
  stay runnable on Linux without an explicit `chmod`.
- `vita-cg-compiler` clone references dropped from
  `scripts/bootstrap.sh`; the upstream donor for early `rsx-cg-compiler`
  bring-up is no longer cloned at bootstrap time.

### Known issues

- `samples/gcm/hello-ppu-cellgcm-{triangle,loops,chain,laneelision}`
  render correctly in RPCS3 but stutter PS-button menu navigation —
  same per-frame VP-ucode upload + missing sysutil
  `DRAWING_BEGIN/END` handling carried forward from v0.2.0.

## [v0.2.0] — 2026-04-25

### `rsx-cg-compiler` — major shader-feature expansion

The Cg→NV40 compiler grew from the v0.1.0 stage-4 baseline (46/46
byte-match against the reference Cg compiler) to **57/57** byte-match.
Highlights:

- **Static for-loop unroll** in the IR builder.  Recognises
  `for (int i = K; i CMP L; i += k) { … }` shapes (DeclStmt or
  ExprStmt init, increment by ±k, no break/continue), simulates the
  trip count up to a 64-iter cap, and re-runs the body straight-line
  with `nameToValue_[i]` rebound to a fresh integer constant per
  iteration.  Pairs with a hand-wired DCE pass between IR build and
  the existing if-convert pass so dead-store iterations clean up.
- **FP arithmetic-chain ADDR** support.  The NV40 emit path now
  handles `temp + uniform` / `temp + varying` / `temp + temp`
  arithmetic, not just the `(varying, uniform)` lone-add shape.
  Chained `vcol + a + b + c`-style runtime-uniform sums emit the
  full N-instruction MOVH-preload + `R0 = R0 + c[k]` link sequence
  byte-exact against the reference compiler.
- **Repeated-add scalar-lane scale fold** (`acc=0; for (i<3) acc+=x;`
  → `MUL 3.0, x`).  CSE collapses the per-iteration swizzle reads;
  algebraic-simplification drops the `0 + x` head; a new scale-fold
  pattern matcher recognises the chained `Add(scale, x)` shape and
  emits a single MAD/MUL with the literal coefficient.
- **VecConstruct lane-elision MOVR** — generalised to 1-, 2-, and
  3-unique-value shapes including zero-skip, `scaledLane` X / Y / Z /
  W, and the FENCBR-bracketed sequence the reference compiler emits
  when the scaled lane lives at non-canonical positions.
- **Three new GCM smoke samples** that exercise the above features
  end-to-end on hardware:
  `samples/gcm/hello-ppu-cellgcm-{loops,chain,laneelision}` — each
  defaults to `USE_RSX_CG_COMPILER=1` and renders a visibly distinct
  triangle that proves both byte-match and execution.
- New `tools/rsx-cg-compiler/docs/KNOWN_ISSUES.md` documents the
  remaining deferred byte-match cases (variable-trip-count loops,
  multi-instruction if-else diamonds, varying-default if-only).

### SDK surface — `cellFs`

- New `cell/cell_fs.h` + `cell/fs/cell_fs_file_api.h` +
  `cell/fs/cell_fs_errno.h`.  Ships every type
  (`CellFsStat` / `CellFsDirent` / `CellFsUtimbuf` /
  `CellFsDirectoryEntry` / `CellFsRingBuffer` / `CellFsAio` /
  `CellFsDiscReadRetryType`), every `CELL_FS_*` macro, every
  `CELL_FS_ERROR_E*` errno alias (including PS3-specific values like
  `EFPOS` / `ENOTMOUNTED` / `ENOTSDATA` / `EAUTHFATAL` not in
  newlib), and 49 documented `cellFs*` declarations covering file /
  dir / stat / block / truncate / IO-buffer / offset-IO / sdata /
  disc-retry / streaming-read / async-IO surface.
- New `tools/nidgen/nids/cellFs.yaml` (curated) carries all 59 SPRX
  exports — the 49 documented signatures plus 10 NID-only entries
  for symbols stripped from 475 public headers but still exported by
  the SPRX (`cellFsAllocateFileArea*WithInitialData`,
  `cellFsTruncate2`, `cellFsArcadeHddSerialNumber`,
  `cellFsRegisterConversionCallback`,
  `cellFsSdataOpenWithVersion`, `cellFsUnregisterL10nCallbacks`,
  etc.).
- `libfs_stub.a` builds via `scripts/build-cell-stub-archives.sh`.
  Coverage: `libfs_stub` jumps **0% → 100%** in
  [docs/coverage.md](docs/coverage.md).
- Verified end-to-end: `samples/sysutil/hello-ppu-cellfs/` round-
  trips a payload through `cellFsOpen → Write → Close → Stat →
  Open(RDONLY) → Read → Close → Unlink` against `/dev_hdd0/tmp/`.
  Every step returns `CELL_FS_OK` in RPCS3; `cellFsStat` reports
  the right `st_size` + `st_mode` (`CELL_FS_S_IFREG | rw-rw-rw-`),
  confirming the struct layout marshals correctly across the SPRX
  boundary.

### Sample suite

Sample count: **34 → 38** green (incl. the four new samples above).

### Known issues

- `samples/gcm/hello-ppu-cellgcm-{triangle,loops,chain,laneelision}`
  render correctly in RPCS3 but stutter the PS-button menu navigation.
  Likely traces to the per-frame VP-ucode upload + missing sysutil
  `DRAWING_BEGIN/END` handling in the canonical flip-immediate
  template; see `~/.claude/projects/.../memory/project_gcm_render_chop.md`
  for investigation notes.

## [v0.1.0] — 2026-04-24

First public, version-tagged snapshot.  Status:

- **PPU toolchain (Phase 1):** binutils 2.42, GCC 12.4.0, newlib 4.4.0,
  GDB 14.2.  Targets `powerpc64-ps3-elf`.  C++17 PPU host runs in RPCS3.
- **SPU toolchain (Phase 2a):** GCC 9.5.0 + binutils 2.42 + newlib 4.4.
  Targets `spu-elf`.  C/C++17 SPU smoke samples pass.
- **PSL1GHT (Phase 3):** v3 RFC tree builds clean against the new
  toolchain; one local patch (`patches/psl1ght/0008`) tightens parsing
  for strict C99/C++17.
- **SDK runtime libraries:**
  - `libgcm_cmd`, `libdbgfont`, `libgcm_sys_legacy`, `libio_legacy`,
    `libc_stub`, `liblv2_stub` shipped under `$PS3DK/ppu/lib/`.
  - `libspurs` PPU surface (Spurs2 / Taskset / Taskset2 / event-flag /
    queue / semaphore / barrier / task / workload).
  - `libspurs_task` SPU runtime + linker script — `hello-spurs-task`
    runs end-to-end in RPCS3 (PPU spawns embedded SPU task, SPU DMAs
    a flag back, Taskset Shutdown+Join clean).
  - `libsputhread` SPU lv2-syscall wrappers (event_flag_*, spu_thread_*)
    byte-exact with the reference SDK; full `hello-event-flag-spu`
    end-to-end run.
  - `libc_stub` carries a working `spu_printf` that bypasses RPCS3's
    broken `libc.spu_thread_printf` path.
- **SDK headers:** `cell/{audio,cell_audio_out,cell_video_out,dbgfont,
  gcm,msg_dialog,pad,pngdec,spurs,sysutil,...}` plus `cell/spurs/*`
  sub-surfaces.  Decoupled from PSL1GHT's `ppu-lv2.h` for the parts our
  own samples consume; PSL1GHT remains the back-compat glue layer for
  legacy homebrew.
- **Tools:**
  - `nidgen` — FNID/NID database + Sony-style stub archive emitter.
  - `coverage-report` — reference SDK vs install-tree coverage matrix.
  - `abi-verify` — CellOS Lv-2 ABI conformance checker.  All 8 invariants
    pass on `hello-ppu-abi-check`.
  - `sprx-linker` — fork of `make_self`/`make_fself` companion.
  - `rsx-cg-compiler` — Cg → RSX (NV40) compiler.  46/46 byte-match
    against `sce-cgc` for the stage-4 test corpus.
- **ABI:**
  - Compact 8-byte `.opd` descriptors (`ADDR32 + TLSGD-ABS`) replacing
    the 24-byte ELFv1 layout.  Two-read indirect-call sequence verified
    end-to-end in RPCS3 against decrypted reference SELFs.
  - `ATTRIBUTE_PRXPTR` (32-bit EA) applied to all pointer fields in
    `cell/*` structs that cross the SPRX boundary.
- **Reference-SDK builds:** unmodified `basic.cpp` (RSX triangle) and
  `5spu_spurs_without_context` (Spurs class surface) compile against
  this SDK using stock `sce-cgc` shaders, and run in RPCS3.
- **Versioning infrastructure:** this changelog, `scripts/version.sh`,
  `cell/sdk_version.h` install hook, GitHub Releases workflow.

### Release contents

This is a **source-plus-host-tools** release.  Pre-built PPU/SPU
toolchains and the SDK runtime libraries are not yet shipped; users
still bootstrap them via `scripts/bootstrap.sh` + the per-phase build
scripts.  See `docs/VERSIONING.md` for the path to a full
out-of-the-box tarball (target: v0.2.x runtime, v0.3.x toolchain,
v0.4.x Windows host).

Assets attached to this release:

- `ps3-sdk-src-v0.1.0.tar.xz` — curated source tree (`git archive`).
- `ps3-sdk-tools-v0.1.0-linux-x86_64.tar.xz` — host tool binaries:
  `nidgen`, `abi-verify`, `coverage-report`, `sprxlinker`,
  `rsx-cg-compiler`.  Drop into `$PS3DEV/bin/` or anywhere on `$PATH`.
  Dynamic-linked against system libelf/zlib/zstd.
- `ps3-sdk-tools-v0.1.0-windows-x86_64.zip` — the same five host tool
  binaries built for Windows via MSYS2 mingw64.  Statically linked
  against the MinGW-w64 runtime, libelf, zlib, and zstd, so each `.exe`
  is self-contained — no MSYS2 install required on the user's machine.
- Companion `*.sha256` files for each archive.

### Known limitations

- Phase 2b (forward-port SPU back-end to GCC 12+) deferred.
- Phase 4 portlibs surface partially populated.
- Phase 5 Canadian-cross to `x86_64-w64-mingw32` not yet wired.
- GDB 14.2 is PPU-only; SPU combined-debug is orphaned upstream.
- `spu_printf` TTY visibility on RPCS3 still routes through the
  PSL1GHT-shape path; native channel-protocol path works on real HW.

[Unreleased]: https://github.com/FirebirdTA01/PS3_Custom_Toolchain/compare/v0.3.0...HEAD
[v0.3.0]: https://github.com/FirebirdTA01/PS3_Custom_Toolchain/compare/v0.2.0...v0.3.0
[v0.2.0]: https://github.com/FirebirdTA01/PS3_Custom_Toolchain/compare/v0.1.0...v0.2.0
[v0.1.0]: https://github.com/FirebirdTA01/PS3_Custom_Toolchain/releases/tag/v0.1.0
