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

### Tools — host-tool install rule

- Added `scripts/install-host-tools.sh`, a Linux-host installer that
  builds repo-owned host tools and stages them into `$PS3DEV/bin` with
  a `$PS3DK/bin` mirror.  The installed set is `nidgen`,
  `coverage-report`, `abi-verify`, `spu-elf-to-ppu-obj`,
  `rsx-cg-compiler`, and `sprxlinker`.
- `scripts/build-sdk.sh` now runs the host-tool installer after the SDK
  install path, so CMake consumers find the clean-room tools from the
  staged toolchain by default.  Use `--no-host-tools` for SDK-only
  rebuilds.

## [v0.9.0] — 2026-05-25

### Toolchain — kernel-target (powerpc64-ps3-kernel-elf)

A second PPU toolchain target ships alongside the userland
`powerpc64-ps3-elf`: `powerpc64-ps3-kernel-elf` produces
classical ELFv1 LP64 PPC64 binaries suitable for CFW LV2 and
COBRA stage-2 payloads, with 24-byte function descriptors,
8-byte TOC slots, and no compact-OPD / SPRX userland
machinery.  Driven by a separate build script
(`scripts/build-ppu-kernel-toolchain.sh`) so userland and
kernel toolchains coexist on one host.

- **`72aec60`** — `kernel-target: binutils alias for
  powerpc64-ps3-kernel-elf`.  Binutils gains the kernel target
  alias so `as`, `ld`, `objcopy`, `objdump`, and `nm` all
  recognise the kernel target name.
- **`3f39568`** — `gcc: kernel-target compiler config +
  ps3kernel64.h + build script`.  GCC config gains a new
  config-machine entry, a `gcc/config/rs6000/ps3kernel64.h`
  target header that pins classical ELFv1 LP64 with 24-byte
  function descriptors, and a `scripts/build-ppu-kernel-toolchain.sh`
  driver that builds the kernel-target gcc + binutils +
  libgcc against the same upstream sources as the userland
  toolchain.
- **`12e995f`** — `libgcc: build kernel-target libgcc +
  un-stub build script item`.  libgcc builds for the kernel
  target with the correct ABI flags so kernel-target code can
  pull `_savegpr_*` / `_restgpr_*` out-of-line helpers when
  generated.
- **`196b2e5`** — `gcc: allow ppc64 out-of-line save/restore
  helpers on PS3 targets`.  Drops an unnecessary block that
  disabled the upstream out-of-line save/restore generation
  path on PS3-flavoured ppc64 targets.
- **`72f0f30`** — `samples: freestanding kernel-toolchain
  smoke (classical ELFv1 payload)`.  A minimal sample under
  `samples/toolchain/hello-ppu-kernel-elfv1` exercises the
  kernel-target compile + link path and produces a classical
  ELFv1 LP64 payload byte-shape that an unmodified CFW LV2 /
  COBRA stage-2 build chain can ingest.

### Toolchain — GCC mlp64 / ILP32-hybrid correctness

A batch of GCC patches that lock in correct codegen across
both data models — TOC width, TLS base selection, DWARF
address size, pointer arithmetic canonicalisation under
ILP32-hybrid, and a shared option file factor.

- **`9a4bda6`** — `gcc: factor mlp64/TARGET_LP64 option into
  shared ps3-lp64.opt`.  Both userland and kernel-target
  configs reference the same option definition so behaviour
  stays in sync.
- **`b830bb9`** — `gcc: pin DWARF2_ADDR_SIZE = 8 for
  cell64lv2 ELF64 wrapper`.  DWARF address size now always
  matches the ELF64 wrapper width regardless of pointer mode;
  debuggers no longer truncate addresses on LP64 binaries
  with ILP32 pointers visible in the source.
- **`83ce544`** — `gcc: pick TLS base register from ELF
  class, not pointer mode`.  TLS access uses the correct base
  register (`r13` on ELF64) independent of whether pointer
  mode is 32-bit or 64-bit.
- **`ae34350`** — `gcc/convert: use unsigned intermediate for
  pointer-to-int cast`.  Pointer-to-int casts under
  ILP32-hybrid go through an unsigned intermediate so a
  high-bit-set 32-bit pointer doesn't sign-extend into an
  arbitrary 64-bit value when widened.
- **`ae87903`** — `gcc/expr: canonicalize POINTER_PLUS_EXPR
  offset under ILP32-hybrid`.  POINTER_PLUS_EXPR offsets are
  truncated to pointer width before the add so 64-bit-typed
  offsets cannot accidentally widen the pointer result.
- **`ea4cf16`** — `gcc/rs6000-logue: save/restore GPRs in
  DImode under TARGET_64BIT`.  Prologue/epilogue save/restore
  of general registers uses DImode under TARGET_64BIT even
  when Pmode is 32-bit, so the full 64-bit register contents
  are preserved across calls.
- **`e7ed864`** — `gcc/rs6000+expr: emit explicit
  pointer-canon unspec on POINTER_PLUS_EXPR under
  ILP32-hybrid`.  POINTER_PLUS_EXPR codegen emits an explicit
  pointer-canonicalisation unspec so RTL passes cannot CSE the
  add with a widened operand and produce a 64-bit pointer in
  an ILP32-hybrid binary.
- **`3d83563`** — `gcc/expr: canonicalize pointer extracted
  from aggregate register under ILP32-hybrid`.  Pointers
  loaded out of aggregate-typed registers are canonicalised to
  pointer width before use, preventing the same widening
  issue at field-extract time.

### Toolchain — auto-link defaults

- **`b7cab94`** — `gcc/cell64lv2: auto-link -lc_stub in
  LIB_LV2_SPEC`.  Every PPU userland link now resolves the
  spu_printf_* family without requiring `-lc_stub` on the
  command line.
- **`688727e`** — `gcc/spu-elf: auto-link -lsputhread in
  LIB_SPEC`.  Every SPU link resolves
  `sys_event_flag_set_bit` and its siblings without requiring
  `-lsputhread`.  A post-install symlink hook puts the
  archive in `spu-elf/lib/`.

### Toolchain — newlib C++ compatibility

- **`4c0bc4e`** — `newlib/stdint: gate SIZE_MAX behind
  __STDC_LIMIT_MACROS in C++`.  C99 §7.18 convention; unblocks
  C++ TUs that declare their own `SIZE_MAX` constant and
  collide with the macro.

### Toolchain — bootstrap + host compatibility

- **`7659be5`** — `scripts: pin host C++11 for GCC 12 libcody
  on rolling hosts`.  Rolling-release hosts with newer host
  GCC build libcody under a default `-std=` that breaks; we
  pin `-std=gnu++11`.
- **`1953c3d`** — `bootstrap: fix newlib tag fetch (drop
  bogus .20231231 suffix)`.  The upstream newlib tag is
  `newlib-4.4.0`, not `newlib-4.4.0.20231231`.

### SDK — sys / runtime surface

- **`3271d52`** — `sdk/sys: ship sys/sys_types.h + sys/types.h
  wrapper`.  `usecond_t` / `second_t` become system-wide
  visible.  `sys/types.h` uses `#include_next` to layer on top
  of newlib's POSIX types.
- **`e397905`** — `runtime: translate POSIX open() flags to
  CellOS LV2 sys_fs_open bits`.  newlib `open()` flag bits
  did not match LV2's `CELL_FS_O_*` bit layout; the runtime
  glue now translates so POSIX file IO routes correctly into
  the LV2 filesystem syscall.
- **`8b2f257`** — `sdk: drop sys-name indirection in
  cell/sysutil_savedata + sysutil_gamecontent`.  Two cell-side
  headers stop forwarding through a vendor-named indirection
  and declare the API surface directly.
- **`8b2150f`** — `sdk: std header wrappers, LV2 timer
  syscalls, cellGcmSetReportLocation`.
  `sdk/include/{stdio,string,stdarg}.h` are thin SDK wrappers
  that `#include_next` the toolchain C headers and, under
  `__cplusplus`, inject the minimal `namespace std`
  using-declarations (`std::memset`, `std::vfprintf`,
  `std::vsnprintf`, `std::vsprintf`, `std::va_list`) that
  legacy C++98 sources need when they include `<stdio.h>` /
  `<string.h>` directly instead of the `<cXXX>` form.
  `va_start` / `va_end` stay as GCC macros and become visible
  via the stdio wrapper.  `sys/timer.h` replaces the previous
  static-inline alias to libc `usleep` with direct LV2 syscall
  entries — `sys_timer_usleep -> lv2syscall1(141, usec)`,
  `sys_timer_sleep -> lv2syscall1(142, sec)`; `sys/sys_time.h`
  pulls `sys/timer.h` so consumers including only
  `<sys/sys_time.h>` see the declarations.  `rsxSetReportLocation`
  (librsx) emits `NV4097_SET_CONTEXT_DMA_REPORT` (method
  `0x01A8`) with the DMA handle selected on location:
  `GCM_LOCATION_CELL` maps to `0xBAD68000` (main memory),
  other locations map to `0x66626660` (local video).
  `cellGcmSetReportLocation` in `cell/gcm/gcm_command_c.h`
  forwards to the rsx primitive, with a C++ no-context
  overload in `gcm_cmd_overloads.h`.

### SDK — cell/gcm

- **`c597665`** — `sdk/cell/gcm: reference-SDK naming aliases
  and no-context wrappers`.  A large batch of cell-side
  aliases over existing rsx primitives plus C++ no-context
  overloads — `CELL_GCM_KEEP` / `INCR` / `COMPMODE_C32_2X1` /
  `MRT_MAXCOUNT` / `SCULL_SFUNC_NOTEQUAL` / `TRANSFER_*`
  family; `cellGcmSet{ZMinMaxControl,CurrentBuffer,DefaultCommandBuffer,FlipStatus}`
  + `cellGcmGetLastFlipTime`; 13 C++ no-context overloads;
  the owned `CellGcmReportData` struct.
- **`a0aad9f`** — `sdk/cell: ship gcm_pm.h (API surface
  only)`.  `CellGcmPerfMonDomain` (6 named clocks), opaque
  per-domain typedefs, 4 prototypes, error code.  Per-domain
  event-ID enums deliberately not shipped until clean-room
  sourced.
- **`efe671c`** — `sdk/cell/gcm: texture border color, zcull
  stats, user-clip constants`.  Cell-side wrappers + C++
  no-context overloads for three GCM command surfaces that
  consume existing rsx primitives:
  `cellGcmSetTextureBorderColor(ctx, index, color)` forwards
  to `rsxTextureBorderColor` (`NV4097_SET_TEXTURE_BORDER_COLOR`,
  method `0x1A1C`); the zcull-stats trio
  (`cellGcmSetZcullStatsEnable` / `cellGcmSetZcullLimit` /
  `cellGcmSetClearZcullSurface`) forwards to the existing
  `rsxSetZCull*` primitives; and the new
  `CELL_GCM_ZCULL_STATS` / `STATS1` / `STATS2` / `STATS3`
  constant aliases plus `CELL_GCM_USER_CLIP_PLANE_DISABLE` /
  `ENABLE_LT` / `ENABLE_GE` aliases land in `gcm_enum.h`.

### SDK — cell/spurs SPU-side surface

- **`5e7737d`** — `sdk/cell/spurs: slim SPU umbrella + 4
  sub-headers`.  A new SPU-flavored `cell/spurs.h` umbrella
  lives under `sdk/include-spu/` and is installed to the SPU
  sysroot, exposing only the SPU-safe subset (shared types,
  error codes, job descriptors, semaphore SPU primitives, and
  the four new sub-headers below).  The PPU umbrella stays
  the full 17-header surface.  Four new sub-headers ship the
  previously-missing spurs API surface:
  `cell/spurs/task_exit_code.h` (`CellSpursTaskExitCode` +
  alignment constant), `cell/spurs/policy_module.h`
  (`CellSpursModulePollStatus`),
  `cell/spurs/lv2_event_queue.h`
  (`cellSpursAttachLv2EventQueue` /
  `cellSpursDetachLv2EventQueue` prototypes), and
  `cell/spurs/lfqueue.h` (opaque `CellSpursLFQueue` type).
  A defense-in-depth `#ifndef __SPU__` guard around the PPU
  inline `cellSpursSendSignal` wrapper in `task.h` keeps it
  from leaking into SPU compiles via the PPU umbrella.
  `sdk/Makefile` gains a new `SPU_ONLY_HEADERS` list +
  matching `install-spu-only-headers` target and a
  `clean-stale-spu-spurs` step that purges leftover PPU-only
  sub-headers (`task.h`, `workload.h`, `barrier.h`,
  `event_flag.h`, `queue.h`) from the SPU sysroot on every
  install.

### SDK — cell/dbgfont Console API

- **`5f237cc`** — `sdk/cell/dbgfont: Console API impl +
  sys/system_types.h forwarder`.  A real virtual-console
  subsystem authored in `libdbgfont` ships the
  `cellDbgFontConsole*` API.  Each console owns an 8-slot
  pool; each slot holds a ring buffer of 64 lines x 256
  chars.  `cellDbgFontConsoleOpen` / `Close` / `Vprintf` /
  `Printf` manage the slot lifecycle.  Slot 0 is reserved as
  `CELL_DBGFONT_STDOUT_ID` and is auto-created by
  `cellDbgFontInitGcm` with sensible defaults (24 lines
  visible, white, top-left).  `cellDbgFontDrawGcm` renders
  every active console's ring buffer through the existing
  `cellDbgFontPuts` path so the console text shares the
  overlay font atlas and shader; `cellDbgFontExitGcm` closes
  every slot.  The public `CELL_DBGFONT_VERTEX_SIZE` constant
  is corrected from 32 to 36 to match actual per-vertex
  storage (DbgPos 12 + DbgUV 8 + DbgColor 16); existing
  overlay-only consumers auto-correct via the public-constant
  change with no regression.  Dead `DBGFONT_MAX_VERTICES`
  constant is removed.  `sys/system_types.h` ships as a
  catch-all system-types forwarder over `sys/types.h`,
  `sys/sys_types.h`, and `ppu-types.h`.

### rsx-cg-compiler

- **`20062e8`** — `rsx-cg-compiler: texRECT / texDepth2D /
  h{3,4}tex2D + SamplerRect type`.  Symbol-table overloads
  for the texture-rectangle and half-precision texture
  intrinsics, the `SamplerRect` type, IR plumbing, and NV40
  emit pass-through.

### Tools — sprx-linker

- **`2885595`** — `sprx-linker: tolerate absent
  .sys_proc_prx_param section`.  Initial fix to allow
  freestanding ELFs to pass through the post-link pass.
- **`6596088`** — `sprx-linker: distinguish freestanding ELF
  from dropped param section`.  Fix-forward: gate the
  pass-through on `.sceStub.text` absence so real link bugs
  still error loudly.

### Samples

- **`81f279b`** — `samples/spurs: add hello-spu-spurs-umbrella
  regression gate`.  The SPU TU includes `<cell/spurs.h>` as
  the primary spurs include and exercises all four new
  sub-header surfaces (`CellSpursTaskExitCode` +
  `CELL_SPURS_TASK_EXIT_CODE_ALIGN/SIZE`,
  `CellSpursModulePollStatus`, `CellSpursLFQueue`) with
  `_Static_assert` checks plus runtime references that
  prevent dead-code elimination.  PPU side creates an LV2
  event queue and exercises `cellSpursAttachLv2EventQueue` /
  `cellSpursDetachLv2EventQueue`.  Runtime path: SPURS
  Spurs2 + class-0 Taskset spawns the embedded SPU task,
  which DMA-writes `0xC0FFEE` into a PPU flag buffer and
  exits via `cellSpursTaskExit`; PPU prints `SUCCESS` on
  flag match.
- Bundled in `efe671c`:
  `samples/gcm/hello-ppu-gcm-zcull-clip-border` exercises the
  texture-border-color path with `BORDER` wrap +
  out-of-range UVs, the zcull stats / limit / clear-surface
  trio, and the user-clip-plane `ENABLE_LT` mode through
  `cellGcmSetUserClipPlaneControl`.
- Bundled in `5f237cc`:
  `samples/dbgfont/hello-ppu-dbgfont-console` exercises the
  Console API end-to-end with the auto-created stdout console
  (white, top-left) plus a custom console at `(0.5, 0.55)`
  with scale `0.75` in green, rendering both through the same
  draw call.

### Documentation

- **`b73772b`** — `docs(readme): refresh for ELF64+ILP32
  default, LP64 multilib, SDK growth`.  Top-level README
  rewritten to describe the current ELF64+ILP32-hybrid
  default + `-mlp64` opt-in shape, the native SDK surface,
  and the kernel-target toolchain.

## [v0.8.0] — 2026-05-23

### Toolchain — LP64 multilib runtime

`-mlp64` is now runtime-functional alongside the ILP32 hybrid
default.  The toolchain sample tier (`hello-ppu-c++17`,
`hello-ppu-abi-check`, `gcm-config-abi`, `hello-ppu-backfill`,
`compact-opd-probe`, `hello-ppu-mlp64-types`,
`hello-ppu-mlp64-tagged-pointer`) all RPCS3-RAN-CLEAN under LP64;
a broader 10-sample cross-category sweep (audio / codec / lv2 /
gcm / sysutil) is RAN-CLEAN with no LP64-specific regressions
against the ILP32 baseline.

- **`7bd5098`** — `LP64 SPRX trampoline: reference tail-call shape
  + 4B fstub stride`.  Three coordinated pieces ship the
  ABI-correct LP64 SPRX dispatch: nidgen's LP64 trampoline now
  emits the bare tail-call form (`std r2,40(r1); lis r12,...;
  lwz r12,...; lwz r0,0(r12); lwz r2,4(r12); mtctr r0; bctr`,
  matching the Cell Lv-2 §5.1 LP64 normative shape); the LP64
  `.data.sceFStub` slot stride is 4 bytes regardless of data
  model (compact-OPD descriptor pointers stay 32-bit under both
  ABIs), with the trampoline loading the slot via `lwz`; and the
  sprxlinker post-link pass gains a `--lp64` flag that rewrites
  the compiler-emitted `nop` after every `bl .sceStub.text` call
  site to `ld r2, 40(r1)`, completing the cross-DSO TOC restore
  that the LP64 tail-call shape delegates to the caller.  The
  ILP32 wrapping `bctrl` trampoline is unchanged.
- **`dad3c20`** — `librsx: native multilib SUBLIB (off-PSL1GHT)`.
  The PSL1GHT `librsx.a` was ILP32-only; under `-mlp64` the
  linker silently fell through to the ILP32 archive, producing
  an ABI-mismatched binary that crashed during static-init.
  `sdk/librsx` is now a first-class SUBLIBS entry that builds
  both data models side-by-side; the matching headers move to
  `sdk/include/rsx/` (with the necessary supporting headers
  `sys/heap.h`, `ppu-types.h`, `ppu-asm.h`) so the SDK is
  self-contained.  `rsxFree` carries an explicit contract:
  allocator-only, caller-owned unbind, process-exit frees
  usually unnecessary — same semantics as the reference
  `cellGcmUtilFree`.

### Toolchain — installed-stage atomicity + freshness gate

- **`9446214`** — `sdk install: atomic owner of stage + freshness
  gate in ps3-self`.  `make -C sdk install` is now the single
  user-facing contract that produces a consistent installed
  stage in one invocation: headers, `cell/sdk_version.h`,
  `VERSION`, every nidgen-emitted stub archive (both ABIs),
  `libsysutil.a` symlink aliases pointing at `libsysutil_stub.a`
  in both ABI dirs, native multilib `liblv2.a`, and a final
  `$(PS3DK)/.ps3dk-install-manifest`.  `cmake/ps3-self.cmake`
  fails CMake configure when the manifest is missing or the
  installed `sysutil/video.h` sha256 doesn't match the source
  tree — sample builds never silently consume a stale
  installed SDK.  `make -C sdk verify-install` is a fast
  no-rebuild assertion of the same invariants.
- **`0af2a6f`** — `build-cell-stub-archives: always rebuild
  release nidgen`.  The script always runs
  `cargo build --release` for nidgen instead of only when the
  binary is missing; a stale-but-present release nidgen would
  otherwise silently mask stubgen changes.  Cargo handles
  incremental builds so the no-source-change case is fast.

### Toolchain — diagnostic + bisection infrastructure

- **`a4de38f`** — `librt-bisect-harness: verifiably-injecting
  per-arm librt + per-arm SDK`.  The bisect harness was
  silently linking the host stage `librt.a` regardless of the
  swap configuration because the arm-prefix gcc symlink let
  `/proc/self/exe` canonicalise back to the host install.
  `prepare_temp_prefixes` now copies `ppu/lib/gcc` and
  `ppu/powerpc64-ps3-elf/lib`, symlinks
  `ppu/powerpc64-ps3-elf/{bin,include}`, and the harness
  exports `GCC_EXEC_PREFIX` for cmake configure + build.  An
  in-harness `die()` on `gcc -print-file-name=librt.a`
  resolving outside the arm prefix prevents future regression.
  A per-arm `make -C sdk install` keeps cell/* headers and
  nidgen stub archives current.  A msgdialog-only CMakeLists
  injection is gated on the sample name so other samples'
  link lines stay untouched.

### Toolchain — ABI verifier expansion

- **`55f395f`** — `abi-verify: e_flags 0 + .toc data-model
  invariants + sceStub shape decode`.  The verifier now
  enforces three additional Lv-2 invariants and the binutils
  emit is brought into sync: `e_flags = 0x00000000` exactly
  (the previous `0x01000000` was a stale legacy value); `.toc`
  reloc/stride must be uniformly `R_PPC64_ADDR32`/4-byte under
  ILP32 or `R_PPC64_ADDR64`/8-byte under LP64 with no
  cross-data-model mixing; and `.sceStub.text` trampolines are
  decoded per-symbol and matched against the normative shape
  for each data model (ILP32 frame-less wrapping `bctrl`, LP64
  bare tail-call `bctr`).  Anything else fails the check.
  `docs/abi/cellos-lv2-abi-spec.md §5.1` and the matching
  sections in `section-reference.md` and
  `toolchain-architecture.md` carry the contract directly.

### SDK — native off-PSL1GHT surface

- **`6e5cbf2`** — `sdk: native off-PSL1GHT sysutil/video/liblv2
  + native librt + rollout tooling`.  Lands the native
  `<sysutil/video.h>` compat shim (a thin static-inline adapter
  layer mapping the legacy `videoConfigure` /
  `videoGetState` / `videoGetResolution` calls + types onto
  the native `cellVideoOut*` nidgen stubs) and the
  `scripts/rollout-off-psl1ght-sysutil.sh` cutover tool that
  symlink-flips `libsysutil.a -> libsysutil_stub.a` in both ABI
  dirs and installs the native multilib `liblv2.a` alongside.
  The cutover preserves existing sample link lines while
  closing the previously-absent `lp64/libsysutil.a` and
  `lp64/liblv2.a` gaps.
- **`e0befbc`** — `samples/gcm: link sysutil_stub on
  alpha-mask, blend, chain`.  Three gcm samples called
  `cellSysutilRegisterCallback` / `cellSysutilCheckCallback`
  / `cellVideoOut*` directly but only linked the
  PSL1GHT-forwarder `-lsysutil`; the native nidgen-emitted
  symbols live in `libsysutil_stub.a`.  Add the stub library
  to the link line, matching the existing
  `samples/sysutil/hello-ppu-osk` pattern.
- **`a4e06c6`** — `samples/gcm: skip rsxFree at process exit on
  the smoke-render set`.  Seven smoke-render samples called
  `rsxFree` on render-target buffers at process exit while
  RSX still considered them bound; `rsxFree` is allocator-only
  (no implicit unbind / Finish / invalidate) so this freed
  metadata that the heap subsequently walked during teardown,
  ending in an access violation in the allocator.  Removing
  the process-exit `rsxFree` (and matching `free(host_addr)`)
  on success paths fixes the crash; the comment block points
  callers at the canonical structured display-safe teardown
  pattern (Finish; set ref; black-out surface; flip; waitFlip;
  Finish; waitFlip; onFinish-free) when one is actually
  needed.

### Samples — LP64 bisection harnesses

- **`a25be3d`** + **`901b1ea`** — `samples/toolchain: add
  hello-ppu-lp64-video-out-probe + hello-ppu-lp64-syscall-probe`.
  Small parameterized LP64 bisect harnesses that exercise the
  cellGcm + sysutil/video init prefix and the libsysbase
  syscall path respectively, so future LP64 regressions can be
  re-bisected against known harnesses rather than re-derived
  from scratch.

### Validation

- 87/87 of the v0.7.x RPCS3-validated sample set continues to
  pass under the default ILP32 hybrid path post-rollout (zero
  emulator fatal markers; 17 RAN-CLEAN + 3 display-loop across
  the 20 deferred gcm samples closed out for this release).
- Under `-mlp64`, the toolchain sample tier is 7/7 RAN-CLEAN,
  and the cross-category broader sweep (audio / codec / lv2 /
  gcm / sysutil) is RAN-CLEAN or RENDER-LOOP with no fatal
  markers post librsx + rsxFree-discipline fixes.

## [v0.7.1] — 2026-05-10

### CI

- **`scripts/bootstrap.sh`** — switch the GCC clone source from
  `gcc.gnu.org/git/gcc.git` to `github.com/gcc-mirror/gcc.git`.
  Under load gcc.gnu.org has answered HTTP requests at 15-30 s
  each; the GitHub mirror answers at ~250 ms (~100x faster) and
  is the official mirror maintained by the GCC team.  Tag names
  match (`releases/gcc-12.4.0`, `releases/gcc-9.5.0`).  This
  alone closes the `RPC failed; HTTP 502 ... fatal: the remote
  end hung up unexpectedly` failure that was blocking the
  Release workflow's `bootstrap upstream sources` step.
- **`scripts/bootstrap.sh`** — add retry-with-backoff (3 attempts,
  15 s and 30 s delays) around every git clone / fetch operation,
  plus `http.postBuffer = 1 GB`, `http.lowSpeedLimit = 1000`, and
  `http.lowSpeedTime = 600`.  Rides through transient slowness on
  the sourceware.org-hosted binutils-gdb and newlib-cygwin clones
  (no GitHub mirrors exist for those repos in their combined-tree
  form) without aborting.  Equivalent to the implicit retry the
  `actions/checkout` action does for our own repo.

## [v0.7.0] — 2026-05-10

### Toolchain — ELF64 + ILP32 hybrid ABI as default

PPU userland output is now ELF64 + ILP32 (32-bit C pointers in a
64-bit ELF wrapper) by default, matching the reference SDK.  LP64 is
retained as a `-mlp64` multilib opt-in so that LP64 inputs still
build, but every conforming Lv-2 PPU executable emitted by the
default driver path now follows the ILP32 contract throughout —
compact 8-byte `.opd`, ADDR32 TOC slots, ILP32 SPRX trampolines.

> **Note — `-mlp64` runtime is in progress (planned feature).**  The
> `-mlp64` opt-in compiles and links today (GCC patches 0010-0021
> emit LP64 user code; multilib `libc` / `libstdc++` / `libsysbase`
> install under `powerpc64-ps3-elf/lib/lp64/`), but the runtime
> linkage tree — `lv2-crt0.o`, `lv2-crti.o`, `lv2-crtn.o`,
> `lv2-sprx.o`, and the nidgen-emitted SPRX stub archives — is not
> yet built as a multilib variant.  Default `-mlp64` link silently
> falls through to the ILP32 crt + ILP32 stubs, and the resulting
> binary crashes in pre-main libc init.  Tracked as planned work;
> the default ILP32 path is the daily-use shape today.

- **GCC 12.4.0 patch 0021** — `rs6000.cc` `output_toc` rewritten to
  emit a 4-byte TOC slot when `Pmode == SImode` and the existing
  8-byte slot only when `Pmode == DImode`.  Closes the silent
  stale-build bug where a big-endian `lwz @toc@l(r2)` against an
  8-byte slot returned the high half (always zero).
- **GCC 12.4.0 patch 0022** — `cell64lv2.h` `TARGET_OS_CPP_BUILTINS`
  defines `__ILP32__` + `_ILP32` in the default-mode else branch
  (mirroring the existing `__LP64__` + `_LP64` path under
  `TARGET_LP64`).  Closes the portability gap where third-party
  configure probes branching on `__ILP32__` / `__LP64__` saw
  neither macro and fell through to a wrong default.
- **nidgen `stubgen.rs`** — emits the frame-less SPRX trampoline
  shape under the ILP32 hybrid: caller LR saved to `caller_sp+24`
  (callee-TOC scratch slot, reserved by ELFv1 for the trampoline's
  use), caller TOC saved to `caller_sp+40` (reserved scratch),
  **no `stwu`**.  Frame-less is load-bearing for `>8`-arg SPRX
  exports (`cellSpursJobChainAttributeInitialize` and
  `cellSpursCreateJobQueue` in libspurs) — any SP shift across
  the `bctrl` moves caller stack-spilled args out from under the
  SPRX callee, which then rejects with
  `CELL_SPURS_JOB_ERROR_INVAL` / `CELL_SPURS_JOB_ERROR_ALIGN`.
  See [docs/abi/cellos-lv2-abi-spec.md §5.1](docs/abi/cellos-lv2-abi-spec.md).
- **nidgen `db.rs` + YAML** — new `aliases:` field on each export.
  `liblv2_stub.yaml` carries 113 PSL1GHT-style aliases
  (`sysGetRandomNumber` etc.) so legacy code links against the same
  trampoline as the canonical Cell name (`sys_get_random_number`).
- **newlib patch 0013-libsysbase-drop-lwmutex-wrappers** — strips
  `libgloss/libsysbase/lock.c` to a comment-only placeholder.  The
  `__syscalls`-indirection wrappers conflicted with nidgen's direct
  SPRX trampolines for the same NIDs.
- **PSL1GHT patch 0012-liblv2-drop-sprx-duplicate-trampolines** —
  removes `sprx.o` from `liblv2.a`'s OBJS list so the ~100
  `sysPrxForUser` entries that overlap with nidgen no longer emit a
  parallel `.lib.stub` Stub record.
- **build-ppu-toolchain.sh** — defensive force-rebuild of
  `libgloss / newlib / libstdc++` when a patch under
  `patches/{gcc-12.x,newlib-4.x}` is newer than the install marker.
  Catches the stale-target-libs class of bug that hid pre-main
  libc init regressions.

### Toolchain — abi-verify enforcement of new spec invariants

`tools/abi-verify` extended with three new checks implementing rules
that landed in
[docs/abi/cellos-lv2-abi-spec.md §4 + §5.1](docs/abi/cellos-lv2-abi-spec.md)
during the ABI rewrite but were previously unenforced:

- **`.toc` data-model + stride consistency** — when the section is
  present, all relocations must be `R_PPC64_ADDR32` with 4-byte
  stride (ILP32) or all `R_PPC64_ADDR64` with 8-byte stride (LP64).
  Mixed reloc types or stride/data-model mismatch fails.
- **No `R_PPC64_ADDR64` in `.toc` under ILP32** — stated
  independently as a regression guard.
- **Frame-less `.sceStub.text` trampoline shape** — section bytes
  are scanned for the `stwu r1, X(r1)` instruction (top 16 bits
  `0x9421`); any occurrence fails with the offset reported.
  Catches accidental re-introduction of the framed shape that
  broke libspurs `>8`-arg exports.

Six new unit tests in `tools/abi-verify/src/lib.rs` cover both
pass and fail paths for each invariant.

### Samples

- **`samples/toolchain/hello-ppu-mlp64-types`** — `sizeof` probe.
  Prints `sizeof` of every width-sensitive primitive (`void *`,
  `long`, `intmax_t`, `uintptr_t`, `intptr_t`, `ptrdiff_t`,
  `size_t`, `time_t`, `off_t`) plus the state of `__LP64__` /
  `__ILP32__` macros.  CMakeLists builds two `.fake.self` artefacts
  side by side: default ILP32 hybrid and `-mlp64` opt-in.
- **`samples/toolchain/hello-ppu-mlp64-tagged-pointer`** —
  high-bit pointer-tagging demonstration.  Packs a 16-bit type tag
  into the top 16 bits of a pointer and recovers via mask + shift.
  Requires `sizeof(uintptr_t) >= 8`; under default ILP32 the shift
  is undefined behaviour on a 32-bit type and the sample reports
  `FAIL` (the demonstration).  Under `-mlp64` the round-trip
  succeeds and reports `PASS`.

### CI

- **`.github/workflows/release.yml`** — Stage 0 ordering hardened
  for the new build dependency graph.  `cell stub archives` now
  runs after the PSL1GHT runtime and before portlibs (fixes
  `ld: cannot find -llv2_stub` from zlib's `example` / `minigzip`
  link tests, since GCC's `LIB_LV2_SPEC` now auto-pulls
  `-llv2_stub`).  A new `SDK headers` step runs between the
  PSL1GHT runtime and cell stub archives so
  `build-cell-stub-archives.sh`'s `libgcm_sys_legacy` wrapper
  compile finds `<sys/lv2_types.h>` and friends.
- **`scripts/build-sdk.sh`** — `--headers-only` flag added.  Runs
  only `install-headers` / `install-spu-headers` /
  `install-shared-spurs-spu-headers` / `install-version`; used by
  the CI workflow's new `SDK headers` step.

### ABI documentation

- `docs/abi/cellos-lv2-abi-spec.md` §4 rewritten for the ILP32
  hybrid pointer model.
- `docs/abi/cellos-lv2-abi-spec.md` §5.1 added — normative
  frame-less SPRX trampoline shape (LR @ `caller_sp+24`, TOC @
  `caller_sp+40`, no frame allocation), with the ELFv1 linkage-area
  rationale baked in.
- `docs/abi/section-reference.md` §5 trampoline sketch updated to
  match the frame-less form.
- `docs/abi/toolchain-architecture.md` conformance table extended
  for the `__ILP32__` macro fix and the frame-less SPRX trampoline,
  and an in-progress note for the `-mlp64` runtime added.

### Validation

- 87/87 PS3DK samples build clean under default ILP32.
- 67/87 samples RPCS3-validated PASS across toolchain / lv2 / spu
  / audio / codec / spurs / sysutil.  The gcm category (20 samples)
  was green in prior sessions and is deferred.
- `hello-ppu-c++17` runs the C++17 standard library end-to-end in
  RPCS3.
- `hello-ppu-mlp64-types-ilp32` confirms the `__ILP32__` macro
  fix: TTY now reports `__LP64__=0 / __ILP32__=1` in default mode
  (was `0 / 0`).
- libspurs `>8`-arg SPRX exports validated end-to-end:
  `hello-ppu-spurs-job-chain`, `hello-spu-job`,
  `hello-spurs-jq` all PASS with the frame-less trampoline.  Soak
  across 64 SPRX-call samples spanning every category — zero
  regressions from the trampoline-shape change.
- `tools/abi-verify` cargo tests: 8/8 PASS (2 original opd-stride
  + 6 new for the §4 + §5.1 invariants).

## [v0.6.0] — 2026-05-05

### SDK — multimedia, codec, and sysutil surface expansion

18 stub-archive libraries promoted from 0% to 100% coverage, each
with cell-named headers under `sdk/include/cell/`, install-tree
archive (`libfoo_stub.a` at the canonical path), smoke sample under
`samples/`, and tracked ABI reference under `docs/sdk/`:

- **Cooperative threading + USB device driver**
  (`docs/sdk/libfiber.md`, `libusbd.md`):
  `libfiber` (47 entry points + `cellFiberPpuInitialize` wrapper) and
  `libusbd` (35 entry points + 35 legacy `usb*`-named forwarders;
  inverted symlink so `libusb.a` resolves to the new
  `libusbd_stub.a`).
- **Demux + audio decode framework + video post-processing**
  (`libpamf.md`, `libadec.md`, `libvpost.md`): `libpamf` (23),
  `libadec` (9), `libvpost` (5).
- **Audio codecs** (`libatrac3plus.md`, `libatrac3multi.md`,
  `libcelpenc.md`, `libcelp8enc.md`): `libatrac3plus` (23),
  `libatrac3multi` (24), `libcelpenc` (9), `libcelp8enc` (9).
- **Multimedia playback + recording framework** (`libsail.md`,
  `libsail_rec.md`): `libsail` (119; 17 sub-headers under
  `sdk/include/cell/sail/` for the player + source + descriptor +
  adapter + renderer + feeder surface) and `libsail_rec` (58; muxer +
  recorder + composer).
- **Image codec encoders + GIF decoder** (`libpngenc.md`,
  `libjpgenc.md`, `libgifdec.md`): `libpngenc` (9 + shared
  `pngcom.h` ancillary chunks), `libjpgenc` (10), `libgifdec` (12).
- **Sysutil + LV2 supplement** (`libdmux.md`,
  `libsysutil_oskdialog_ext.md`, `libsysutil_search.md`,
  `libcrashdump.md`): `libdmux` (20), `libsysutil_oskdialog_ext` (17),
  `libsysutil_search` (21), `libcrashdump` (2).
- **`docs/sdk/README.md`** indexes the per-library reference docs.
  Each per-library doc lists the function table, error codes,
  initialisation lifecycle, and Notable ABI quirks (`size_t`-as-
  `uint32_t` in caller-allocated structs, required sysmodule load
  order, async init pump, callback non-NULL contracts,
  late-firmware-only entries) so shipped behaviour is documented
  directly.

`sdk/include/cell/sysmodule.h` grows the constants required to load
each library (DMUX/DMUX_PAMF/DMUX_AL, ATRAC3+ / ATRAC3MULTI,
ADEC_CELP / ADEC_CELP8, FIBER, SAIL_REC, GIF_DEC, plus the existing
PAMF / ATRAC3PLUS already present).  PRXPTR audit applied across all
new headers — pointer fields in caller-allocated structs that cross
the SPRX boundary carry `ATTRIBUTE_PRXPTR`, and width-sensitive
typedefs (`size_t`, `time_t`) declared as `uint32_t` to match the
in-SPRX layout.

### SDK — `cellGcmSetFragmentProgramParameter` correctness fixes

Two crashes in the FP-uniform patcher predating the matrix work, both
RPCS3 access-violation regressions:

- **Writes against `.rodata`.**  The patcher used to write FP uniform
  values into the read-only embedded `CgBinaryProgram` blob bundled
  with the .text section, instead of into the writable RSX local
  memory copy at `cfg.localAddress + addrOffset`.  Resolved
  `addrOffset` against the GCM config's `localAddress` so all writes
  land in RSX-visible memory.
- **`embeddedConst==0` early-bail.**  A zero `embeddedConst` offset
  means the parameter has no embedded constants to patch (samplers
  are the canonical case).  The previous check only validated the
  computed pointer (`prog + 0 == prog != NULL`) and proceeded to
  read the .fpo file header as if it were an embed count.  Gate on
  the offset itself, not the computed pointer.

### SDK — `cellGcmSetFragmentProgramParameter` matrix uniforms

Float `matrix4x4` uniforms in fragment programs were silently no-op'd
in the previous implementation: a matrix parent
`CgBinaryParameter` has `embeddedConst=0` and the row leaves at
`pp+1..pp+N` carry the actual ucode patch points, but the patcher
checked the parent's `embeddedConst` and bailed.  Walk the row leaves
gated by `pp->res == 0x0CB8` (the uniform/parameter resource code) to
avoid overmatching shapes — the same matrix type codes appear on
varyings, output colors, and function entries with different `res`
values, and a row-walk on those would land in unrelated parameters'
embed lists and corrupt the FP ucode.  Per-leaf
`var == 0x1006` (CG_UNIFORM) gate inside the walk acts as a defensive
backstop for shaders whose parameter table happens to deviate from
the parent-then-rows layout.

### Samples — `hello-ppu-cellgcm-render-to-texture`

Render-target switch smoke: allocates an off-screen surface from the
RSX local-memory bump allocator, switches the active render target via
`cellGcmSetSurface`, clears it, and reads the cleared bytes back
through the PPU mapping at `cfg.localAddress + offset` to verify the
clear actually landed.  Validates the single-target RTT path
end-to-end (surface registers + clear + pipeline drain + shared-memory
addressing) and documents the PPU/RSX address-space split inline as
shipped reference.

### `rsx-cg-compiler` — VP/FP byte-match expansion

The byte-diff suite against the reference Cg compiler at `--O2 --fastmath`
grows from 65/65 to **96/96** byte-identical:

- **FP — compound-discard lowering.**  Two-pass emit (analysis + emit)
  for `IROp::Discard`: the analysis pass records all discard sites with
  their gating conditions; the emit pass walks each site, resolves
  compound `LogicalAnd` conditions to leaf `CmpBinding` entries, and
  emits the full TEX → CMP → MULXC → KIL sequence with correct lane
  masks, `NVFXSR_NONE` CC registers, and post-discard MOV/MULR
  swizzle.  Both `discard_tex_alpha_f` (compound condition) and
  `discard_tex_varying_f` (simple condition with post-discard
  output) now byte-match the reference Cg compiler at the full container level.
- **FP — dual-texture discard ordering.**  When a fragment program
  contains two discard sequences gated by tex-LHS comparisons,
  the uniform-load MOV for the *second* discard must precede its
  TEX instruction.  A new `isSubsequentDiscard` flag and
  `emittedTexResults` tracking set ensure the exact instruction
  schedule the reference Cg compiler produces.
- **FP — `VecConstructTexMul` split-write shape.**  Recognises
  `oColor.xyz = tex.xyz * varying.x; oColor.w = tex.w * varying.y`
  and emits a split-write StoreOutput (two MOVs with per-lane masks)
  instead of a single MOV, matching the reference Cg compiler's decomposition of
  post-discard output mixing.
- **FP — implicit varying-binding inference.**  Front-end's default-
  binding pass assigns sequential `TEXCOORD<N>` slots to unbound FP
  entry-point inputs in declaration order, skipping any `TEXCOORD<N>`
  already taken by a sibling parameter.  A new `Semantic.inferred`
  flag plumbs through AST → IR → container so the .fpo carries the
  resource code (`0x0c94 + N`) in the parameter table while
  suppressing the semantic *string*.  Closes the gap that blocked
  any FP shader written without explicit per-parameter semantics.
- **FP — alpha-mask if-else lowering.**  Recognises the
  `if (tex.a <= K) oColor = float4(0,0,0,0); else oColor = colorVarying;`
  shape and lowers it to the canonical 3-instruction form:
  `TEX R1.<lane>, f[TC<m>], TEX<m>` + `SLE COND R1.<lane>, c[0].x`
  (with inline scalar literal) + `MOV(EQ.xxxx) R0, f[falseBranch]`.
  The literal-vec4 THEN-branch is *elided* — R0's implicit
  `(0,0,0,0)` default at FP entry plays its role.  Three coupled
  emitter changes land together: pre-stout hoistable-op promotion in
  the if-converter, `CmpBinding::LhsKind::TexResult` in the cmp
  handler, and the tex-LHS schedule in Select StoreOutput emit.
- **FP — `isReferenced=0` for unused entry params.**  `emitFragmentProgramEx`
  now walks every IR instruction's operand list once and records a
  `referencedParamIndices` set on `FpAttributes`; the container reads
  that set instead of defaulting to 1.  Output params (StoreOutput
  keys off semantic, not operand) stay at 1 unconditionally.
- **VP — narrow-type passthroughs.**  `float2` / `float3` outputs emit
  a partial-mask MOV with last-lane-replicated source swizzle
  (e.g. `oTexCoord = inTexCoord` lowers to `MOV o[7].xy, v[8].xyxx`)
  instead of a full `.xyzw` mask.
- **VP — generic `float4(scalar, ..., literal)` constructors.**  Lanes
  are classified as `InputLane` / `ConstLane` / `Literal` and emit
  one MOV per source group, with literals packed top-down from
  `c[467]` into a per-shader pool.  The container emits matching
  `internal-constant-N` parameter-table entries
  (`var=0x1007`, `paramno=0xFFFFFFFE`) with a 16-byte float[4] block
  embedded in the strings region pointed at by `defaultValue` —
  exactly the layout the reference compiler produces.
- **VP — chained `mul(matrix, X)` with temp ping-pong.**  Prior
  matvecmul results materialise into temp registers; the temp is
  recycled after the next chain step so we ping-pong R0 → R1 → R0
  instead of cascading R0 → R1 → R2.  Includes a DPH-fold path 2
  for `mul(M, float4(in.x, in.y, 0, 1))` when the W lane is a
  literal `1.0f`.
- **VP/FP — `: register(BANK<N>)` explicit register binding.**  Front-
  end parses the trailing `register(...)` clause on uniform / param
  declarations and threads an `explicitRegisterBank` + `explicitRegisterIndex`
  field through AST → IR → container so the param table writes the
  user-requested const-bank slot instead of the default allocator pick.

### GCM — cellGcm path coverage and shader-pipeline migration

- **`libgcm_cmd` cellGcmCg* / cellGcmSet* helpers.**
  `cellGcmSetVertexProgramParameter`,
  `cellGcmSetFragmentProgramParameter`, and the missing CG parameter-
  mutator entry points implemented in `sdk/libgcm_cmd/src/gcm_cg.c`.
- **`cellGcmSetVertexProgram` literal-pool upload.**  The inline
  emitter in `gcm_cg_bridge.h` now walks the param table for entries
  flagged with `paramno == 0xFFFFFFFE` (the internal-constant marker)
  and uploads each one through `NV40TCL_VP_UPLOAD_CONST_ID`.
  Without this, shaders that read literal values via the const bank
  (e.g. `float4(in.x, in.y, 0, 1)`) draw against stale const-bank
  state.
- **GCM samples migrated to rsx-cg-compiler.**  `hello-ppu-cellgcm-{blend,
  cube, textured-cube, triangle, quad, sysmenu}` switch off cgcomp +
  the legacy `rsx*Program*` walkers and onto rsx-cg-compiler +
  the cellGcmCg* / cellGcmSet* path that parses the
  `CgBinaryProgram` layout.  Vertex-attribute indices come from
  `cellGcmCgGetParameterResource(...) - CG_ATTR0` instead of hard-
  coded `GCM_VERTEX_ATTRIB_*` slots.  `vp-loop` stays on cgcomp
  pending MAD-fusion + interleaved emit ordering.
- **`hello-ppu-cellgcm-alpha-mask` validation sample.**  Exercises
  rsx-cg-compiler's two newest FP emitter features end-to-end
  through `ps3_add_cg_shader_rsxcgc` + `cellGcmCgInitProgram` —
  per-corner-coloured fullscreen quad with a circular alpha-mask
  hole punched from a 64×64 procedural texture.
- **`libdbgfont` compiles shaders at build time.**  Drops the
  committed `dbgfont_shaders.c` blob in favour of a Makefile pipeline
  that re-derives `.vpo` / `.fpo` from `.vcg` / `.fcg` sources via
  rsx-cg-compiler → bin2s → as → ar.  `dbgfont.c` switches off the
  legacy `rsx*Program*` walkers and onto the cellGcmCg* / cellGcmSet*
  API.  The fragment shader is rewritten off the split-write else
  (`oColor.rgb = ...; oColor.a = ...`) and onto a single
  `oColor = color` write so rsx-cg-compiler's alpha-mask if-convert
  matches the reference compiler byte-for-byte.

### SDK — SPURS JOB CHAIN runtime

End-to-end `cellSpursJobMain2` bring-up: `samples/spurs/hello-spu-job`
runs cleanly in RPCS3.

- **SPU toolchain — `-mspurs-job` / `-mspurs-job-initialize` /
  `-mspurs-task` driver flags.**  Patches against binutils 2.42 +
  GCC 9.5.0 add a `spu_elf_set_spurs_job_type()` setter on the SPU
  ELF backend, three new `--spurs-job*` long options on `ld`, and
  matching `-m` driver flags in GCC's `spu.opt` whose `LINK_SPEC` rules
  forward them.  Stock SPU GCC + binutils emit `e_flags=0` (treated as
  "unspecified" and rejected by jobbin2 packaging); the new flags emit
  `e_flags=1` / `2` / `3` natively, retiring the post-link byte-hack.
- **`sdk/libspurs_job/`** ships a `_start` trampoline (the canonical
  4-instruction xori-pair signature followed by `bin2` magic at
  LS 0x20 — required by the JM2 dispatcher's deferred validator),
  `cellSpursJobInitialize` PIC bss-clear, `spurs_job.ld` linker
  script with `.before_text` at LS 0 and 16-byte end-padding, plus
  install drops to `$(PS3DK)/spu/{lib,ldscripts,include/cell/spurs}`.
- **`cmake/ps3-self.cmake`** grows a `JOBBIN` flag on `ps3_add_spu_image`
  that runs `spu-elf-objcopy -O binary` on the linked SPU ELF and
  bin2s-embeds the resulting flat raw image (the dispatcher consumes
  raw bytes, not an ELF wrapper — embedding the linked ELF parks
  `\x7fELF` at LS 0 and trips `JOB_DESCRIPTOR`).
- **PPU header surface.**  14 new headers under `cell/spurs/`:
  `error.h`, `exception_types.h`, `job_chain.h`, `job_chain_types.h`,
  `job_commands.h`, `job_descriptor.h`, `job_guard.h`, `version.h`,
  plus `cell/sync/barrier.h`, `cell/padfilter.h`,
  `sys/spu_initialize.h`, `sys/spu_utility.h`.
- **`docs/abi/spurs-job-entry-point.md`** is the normative ABI spec:
  ELF identity (e_flags semantics), section layout, `_start`
  trampoline byte signature, register-passing convention, required
  symbols, `binaryInfo[0..3]` as the high-32 bits of the 64-bit
  `eaBinary` union (NOT a magic word), the LS 0x20 `bin2` magic.

### SDK — SPURS JOB QUEUE runtime

`samples/spurs/hello-spurs-jq` runs end-to-end: 6 jobs, 2 SPUs,
semaphore-driven sync, clean shutdown.

- **`sdk/libspurs_jq/`** ships the cooperative-scheduler entry-point
  surface (53 entry points covering info / open / close / push
  variants / port lifecycle / scheduler / hash-check / trace-dump /
  semaphore / wait-signal), a `cellSpursJobMain2` wrapper that runs
  `__initialize → cellSpursJobQueueMain (user) → __finalize`, and
  the with-CRT `_start` trampoline carrying the `JOBCRT Ver13` magic
  at offset 0x20 of `.before_text` plus ADDR32 relocations to
  `_end` / `__bss_start` (the JQ kernel reads BSS clear range from
  these).  A `__job_start` wrapper runs
  `_cellSpursJobCrtAuxInitialize → _init → cellSpursJobMain2 →
  __do_atexit → _fini → _cellSpursJobCrtAuxFinalize` on every
  per-job invocation.
- **`cmake/ps3-self.cmake` `JOBBIN_WRAP` flag** runs
  `spu_elf-to-ppu_obj.exe --format=jobbin2` (under wine) on the linked
  SPU ELF, producing the wrapped `jobbin2` blob (256-byte ELF header
  prefix + LS image bytes — required by the JQ dispatcher; flat raw
  images are rejected) plus a `.spu_image.jobheader` template the
  user copies into `s_job.header`.  Default toolchain lookup probes
  the user-provided host-win32 install paths; both are env / cache-
  var overridable.
- **PPU + SPU job-queue header surface** under `cell/spurs/`:
  `job_queue_define.h` (bracket macros + DPRINTF + RETURN_IF),
  `job_queue_types.h` (opaque containers + enums + pipeline-info +
  exception-handler typedefs), `job_queue.h`, `job_queue2.h`.
- **`--start-group` / `--end-group`** wrapping in `ps3_add_spu_image`
  for multi-lib link lines so cross-archive references (e.g.
  libspurs_job's `_start` referencing libspurs_jq's `cellSpursJobMain2`)
  resolve in one pass.  Single-lib lists go through unwrapped.

### SDK — libspurs SPU runtime expansion

Eleven new SPU-side libspurs entry points added to `libspurs_task.a`,
each byte-equivalent to the reference SDK's `libspurs.a`:

- **Module getters (round 1):** `cellSpursGetSpursAddress`,
  `cellSpursGetCurrentSpuId`, `cellSpursGetTagId`,
  `cellSpursGetWorkloadId`, `cellSpursGetSpuCount`.  Plain volatile
  LS reads at the kernel-populated offsets
  (`0x1c0` / `0x1c8` / `0x1cc` / `0x1dc` / `0x176`).  SPU GCC 9.5
  emits the byte-equivalent `lqa + (rotqbyi) + bi $0` shape.  New
  `samples/spurs/hello-spurs-getters` validates each against the
  runtime spurs pointer / `numSpus`.
- **Dispatch helpers (round 2):** `cellSpursModuleExit`,
  `cellSpursModulePoll`, `cellSpursModulePollStatus`, `cellSpursPoll`,
  `cellSpursGetElfAddress`, `_cellSpursGetIWLTaskId`.  Plain C with
  volatile LS reads + indirect-call casts; build requires
  `-fno-schedule-insns2` for byte-correct sizes.
- **Sync primitives (round 3):** `cellSpursSemaphoreP` (480 B),
  `cellSpursSemaphoreV` (432 B), `cellSpursSendSignal` (464 B),
  `_cellSpursTaskCanCallBlockWait` (152 B),
  `_cellSpursGetWorkloadFlag` (48 B),
  `_cellSpursSendWorkloadSignal` (168 B).  Hand-written `.S` files
  byte-matched against the reference `libspurs.a` object disassembly
  via `cmp -l` of `.text` byte slices.  `samples/spurs/hello-spurs-semaphore`
  exercises the producer/consumer P/V path; status reports DONE
  rather than SUCCESS because the BLOCK service round-trip is
  RPCS3-HLE-blocked (taskset PM syscall HLE is `Broken (TODO)`).
- **`docs/abi/libspurs-sync-primitives.md`** captures the binary
  contract: calling convention, cache-line layout, error codes,
  caller obligations re: context save area + `lsPattern`.

### SDK — libsputhread SPU lv2-syscall expansion

Six SPU lv2-syscall wrappers added to `libsputhread.a`, byte-equivalent
to the reference SDK:

- `sys_spu_thread_send_event` / `throw_event` / `receive_event` /
  `tryreceive_event` (event-port; `ch28+ch30` mailbox shape;
  `+0x40000000` marker on throw; stop `0x110` / `0x111` on receive
  variants).
- `sys_spu_thread_group_yield` (stop `0x100`),
  `sys_spu_thread_switch_system_module` (stop `0x120`).
- Public surface: `<sys/spu_event.h>` declares the four event-port
  entry points + `EVENT_DATA0_MASK` / `EVENT_PORT_MAX_NUM`;
  `<sys/spu_thread.h>` grows declarations for `group_yield` +
  `switch_system_module`.
- `samples/lv2/hello-event-port-spu/` drives a six-step round-trip
  in RPCS3.

Two byte-match gotchas surfaced and are documented inline: SPU GAS
folds `nop $127` to `nop $0` (use `.long 0x4020007f` for byte-
identical output); `hbrr` branch-hint must sit on the line of the
actual branch, not the line before, for the encoded immediate to
match the reference.

### SDK — SPU C++ SIMD class wrappers

14 `simd::` class headers under `sdk/include-spu/{simd,bits/simd_*.h}`:
`bool{2,4,8,16}`, `{u}char16`, `{u}short8`, `{u}int4`, `{u}llong2`,
`float4`, `double2` — each with the full operator set (arith, compare
→ `boolN`, shifts, select, gather/any/all, `[]_idx` lvalue proxy).
Implementation deltas vs. the canonical surface: scalar-extract
fallback for integer `/` and `%` (correctness over throughput);
`spu_cmpgt` / `spu_cmpeq` directly on `vec_double2` (GCC 9.5 builtin
handles double compares natively, no `libsimdmath` cmp dep);
`divf4` / `divd2` for float/double divide.  `samples/spu/hello-spu-simd`
exercises every operator class; nine result groups print expected
values in RPCS3.

### SDK — reference-SDK source-compat headers

Surfaced while attempting to build code originally written for older
SDKs unmodified against our toolchain.  None of these are CellOS ABI
changes — they're naming / layout aliases over what we already ship:

- `cell/gcm.h` (was authored in-tree, never installed),
  `simdmath.h` top-level forwarder, `sys/cdefs.h` wrapper exposing
  `CDECL_BEGIN` / `CDECL_END` / `NAMESPACE_LV2_BEGIN` / `_END`,
  `sys/sce_cdefs.h` standalone copy, `sys/event.h` umbrella over
  the LV2 sync split headers, `sysutil/sysutil_common.h` request-id
  defines, `cell/keyboard.h` + `cell/mouse.h` cellKb* / cellMouse*
  alias surfaces over `libio.a`'s PSL1GHT-named entries, four new
  validation samples.
- **`sdk/include-spu/`** picks up `cell/dma.h` (thin DMA helpers
  wrapper), `cell/sync.h`, `cell/sync/barrier.h`, plus
  `vectormath/cpp/vectormath_soa.h` for the Spurs SPU include set
  and a top-level `sdk_version.h` forwarder.  New
  `samples/spu/hello-spu-dma` exercises the surface.
- **SDK header content updates** — enums, structs, inline functions,
  and type aliases added to existing `cell/cell_video_out.h`,
  `cell/cgb.h`, `cell/dbgfont.h`, `cell/gcm.h`,
  `cell/gcm/gcm_cg_bridge.h`, `cell/gcm/gcm_command_c.h`,
  `cell/gcm/gcm_enum.h`, `cell/keyboard.h`, `sys/event.h`,
  `sys/process.h`, `sys/spu_thread.h`, `sys/sys_time.h`,
  `sysutil/sysutil_common.h`, `vectormath/cpp/vectormath_soa.h` so
  reference-SDK code parses without local edits.

### Tooling — NID stub generator

- **`nidgen` SPRX stub trampoline shape.**  Switches from a frame-
  ful `bctr` tail-call to a frame-less wrapping shape (`bctrl`
  preserving `LR @ sp+24` and `r2 @ sp+40`, no `stdu`).  The
  reference `bctr` tail-call breaks on intra-DSO `bl` because PPU
  GCC doesn't restore the TOC after the call; the wrapping shape
  fixes calls with more than 8 arguments through any SPRX boundary
  (libspurs `job_chain` etc.).

### Documentation

- **README** grows a "True 64-bit PPU ABI" section under "Toolchain
  components" explaining the choice to ship a true PPU64 toolchain
  rather than the historical PPU32 setup, and the porting
  implications for code originally written assuming 4-byte naked
  pointers.  Names `ATTRIBUTE_PRXPTR` as the migration path for
  cross-SPRX struct fields and `-fpermissive` as the bridge for
  old-sample `(int)void *` casts.
- **`tools/rsx-cg-compiler/docs/KNOWN_ISSUES.md`** refreshed:
  implicit-varying inference + alpha-mask if-else marked DONE; new
  entries for FP `tex2D` emit gaps and FP arithmetic-lowering for
  non-passthrough writes.
- **`docs/known-issues.md`** updated — known-issue entries
  retired/added across the GCM / SPURS / shader-compiler boundary.

### Tooling — `sprxlinker` cross-build

`scripts/build-host-tools-windows.sh` now vendors libelf 0.8.13
(fossies.org primary, sources.openwrt.org fallback) as a static
cross-built dep and produces a fully-static `sprxlinker.exe`
(412 KB PE32+ x86-64) alongside the other host tools.  Drops the
"skip locally" workaround that punted to the MSYS2 CI path; WSL2 /
native Linux runs of the script now stage the full host-tool set.
Cross-compile quirks documented inline: `ac_exeext` sed-patch for
autoconf-2.13's missing `EXEEXT` support, `-std=gnu89` to keep
GCC 14+ from rejecting the configure probe's K&R `main()`,
`ac_cv_sizeof_{long_long,___int64}=8` overrides so 64-bit ELF
support stays enabled when cross-compiling.

### Build / install layout — canonical stub-archive names

Stub archives now install under their canonical `_stub` filenames
(`libgcm_sys_stub.a`, `libio_stub.a`, `libsysmodule_stub.a`,
`libusbd_stub.a`, etc.) so reference Makefiles that link against
`-lgcm_sys_stub` resolve out of the box.  PSL1GHT-style legacy names
(`libgcm_sys.a`, `libio.a`, `libusb.a`) are retained as symlinks back
to the `_stub` archive, so existing PSL1GHT consumers keep building
without changes.  `cell_video_out.h` switches its forwarders off
PSL1GHT's `videoGetState` etc. and onto `extern` declarations of the
`cellVideoOut*` SPRX trampolines that resolve through
`libsysutil_stub.a`; `sdk.makedef-ppu-gcc.mk` drops its `-lsysutil`
auto-link as part of the same cleanup.

### CI / release infrastructure

- **`build-host-tools-windows.sh`** auto-fetches the PSL1GHT source
  before the geohot/fself build — previously the geohot step ran
  before `ensure_psl1ght_source` and died with "PSL1GHT geohot tools
  source missing" on a clean CI checkout.
- **Windows release packager** stages the complete host-tool set
  (PSL1GHT host tools, geohot/fself, rsx-cg-compiler, sprxlinker,
  `package_finalize`) plus required Python helpers.  `cg.dll` is
  treated as optional rather than required.
- **`release.yml`** grows the matching staging steps, fixes the
  optional-asset path (single missing optional asset no longer fails
  the run), and pulls Python helpers into the Windows release.
- **`extract-release-notes.sh`** new helper pulls the changelog
  entries for the version being released and feeds them straight into
  the GitHub Release body, so tags ship with the matching changelog
  section without manual copy-paste.
- **`CCACHE_DIR=~/.ccache`** pinned across PPU/SPU/cross toolchain
  jobs.  ccache 4.x on the runner defaults to
  `$XDG_CACHE_HOME/ccache` (`~/.cache/ccache`), so builds populated a
  path the `actions/cache@v5` step never saw and the cache step
  skipped saving with "Path Validation".  Pinning the directory keeps
  cache hits across runs.

### Other

- **`hello-spu-job` / `hello-spurs-jq` timeout-exit polish.**
  `sys_process_exit` on JQ halt or sentinel-poll timeout so RPCS3
  closes cleanly when a test fails (no PS-button-out needed).
- **`NOTICE`** updated for the NV_fragment_program reference
  attribution.
- **`tools/docs/coverage.md`** — symbol coverage report committed
  under tracked docs.  Generated by `coverage-report`, diff-by-name
  against the install tree, backed by
  `tools/nidgen/nids/aliases.yaml` for renames.
- **`AGENTS.md`** parallels `CLAUDE.md` for the Codex CLI agent —
  same project context with paths repointed under `~/.Codex/...` so
  both agents pick up the same locked-in decisions, status snapshot,
  workspace layout, and house rules.
- **`libdaisy_stub.yaml`** sets `archive_name: daisy` so the
  cell-stub-archive build produces `libdaisy_stub.a` at the canonical
  install path instead of falling back to the library field.

## [v0.4.0] — 2026-04-27

### SDK surface — four new cell-named libraries land at 100% coverage

Headline: 4 stub-only libraries promoted from 0% to 100% in this
release, plus one fully-rounded out.  Aggregate coverage across the
tracked reference export set climbs **46.2% -> ~52.0%** (+254 covered
exports across 5 libraries).

- **libsysutil_stub (cellSysutil PRX) — 174/174.**  Full PRX surface
  through nidgen archive flow; covers `cellVideoOut*` late-SDK
  extensions (DeviceInfo, callbacks, monitor type, cursor-color
  range), `cellMsgDialogOpenSimulViewWarning`, `cellSysCache*`,
  `cellSysconf*`, `cellStorageData*`, `cellSysutilAvc*` (legacy
  voice-chat panel), `cellSysutil*BgmPlayback*`, `cellOskDialog*`
  extension entries, `cellSysutilGame{Exit,PowerOff,Reboot}_I`,
  `cellSysutilGameDataExit/AssignVmc`.  Eight new
  `samples/sysutil/hello-ppu-{videoout-info, syscache, bgm, osk,
  avc, sysconf, storagedata, msgdialog-3d}` exercise the surface.
  AVC signatures cross-checked against the only available
  reverse-engineered reference (RPCS3's `cellSysutilAvc.{h,cpp}`)
  since the late-firmware headers stripped them.

- **librtc_stub (cellRtc) — 33/33.**  Real-time clock surface:
  calendar conversion (`CellRtcDateTime` <-> `CellRtcTick`),
  arithmetic (add/sub days/hours/seconds), day-of-week / leap-year
  helpers, parse/format ISO-style strings.  New
  `samples/sysutil/hello-ppu-rtc` exercises the round-trip path.

- **libsync_stub (cellSync) — 42/42.**  User-space sync primitives:
  `CellSyncMutex`, `CellSyncBarrier`, `CellSyncRwm`,
  `CellSyncSemaphore`, `CellSyncQueue`, plus the LFQueue family.
  `CellSyncQueue` requires 32-byte and 128-byte alignment to satisfy
  SPRX-side asserts; documented in `cell/sync.h`.  New
  `samples/sysutil/hello-ppu-sync` round-trips each primitive.

- **libsync2_stub (cellSync2) — 32/32.**  Thread-pluggable sync
  layer over any thread library.  RPCS3's HLE is partial — basic
  create/destroy/lock paths work, the thread-pluggability layer is
  incomplete.

- **libsysutil_savedata_extra_stub — 27/27** (and the existing
  `libsysutil_savedata_stub` direct-extern surface filled in).
  39 new entry points exposed as plain externs in
  `cell/sysutil_savedata.h` (legacy v1 entries, the
  `cellSysutilSaveDataExtra` family, `cellSaveDataEnableOverlay`).
  New `samples/sysutil/hello-ppu-savedata-link` is a link-time
  test that pulls every declared symbol so name typos surface as
  link errors rather than runtime ones.  Link line for downstreams:
  `-lsysutil_savedata_stub -lsysutil_savedata_extra_stub -lsysutil`.

### Bug fix — RPCS3 vertex-program upload desync

PSL1GHT's `rsxLoadVertexProgramBlock` historically batched 8
instructions per UPLOAD_INST method packet (count=32 method words).
RPCS3 desyncs on the second consecutive count=32 packet — once a VP
crosses 16 instructions the FIFO trips with `RSX: FIFO error:
possible desync event (last cmd = 0x1c0000d)`.  Real hardware
accepts both shapes; the small-packet form (count=4, one per VP
instruction) is what every shipping reference sample uses, so it's
the empirically-safe option.

- `patches/psl1ght/0009-librsx-vp-upload-count4.patch` rewrites
  `rsxLoadVertexProgramBlock` to emit one count=4 packet per insn.
- `sdk/libgcm_cmd/src/rsx_legacy/commands_impl.h` — matching change
  in the rsx-legacy code path so libgcm_cmd-built emitters stay
  aligned with PSL1GHT.
- `sdk/include/cell/gcm/gcm_cg_bridge.h::cellGcmSetVertexProgram`
  inline emitter rewritten to the same shape; reservation
  recomputed (`3 + N*5 + 6` words instead of `3 + loop*33 + ...`).
- `samples/gcm/hello-ppu-cellgcm-vp-loop` reproduces the failure
  on unpatched PSL1GHT and renders cleanly on patched.

### GCM samples — XMB-pause render-loop yield

PS-menu sluggishness during GCM samples (the symptom previously
tracked as a `hello-ppu-cellgcm-triangle` known-issue) was caused
by the render loop contending with the XMB for the FIFO.  Five
existing GCM samples (chain, laneelision, loops, triangle,
rsx-clear) now switch on the sysutil callback's `DRAWING_BEGIN` /
`SYSTEM_MENU_OPEN` events to set a paused flag, drain the current
frame, and idle until `DRAWING_END` / `SYSTEM_MENU_CLOSE` clear it.
PS-button navigation is now smooth.  The previously-deferred
known issue has been retired from `docs/known-issues.md`.

### Build helpers — `ps3_add_pkg`, `ps3_add_cg_shader_rsxcgc`

- `ps3_add_pkg(target)` wraps `make_self_npdrm` + `sfo.py` + `pkg.py`
  end-to-end, using a default `PARAM.SFO` from
  `cmake/templates/sfo.xml` and an ICON0 from `sdk/assets/`.
- `ps3_add_cg_shader_rsxcgc(target file)` compiles via
  rsx-cg-compiler instead of cgcomp.  Mandatory for samples
  calling `cellGcmCgInitProgram` / `cellGcmCgGetUCode` —
  rsx-cg-compiler emits the `CgBinaryProgram` layout (magic
  0x1b5b/0x1b5c) those helpers walk; cgcomp's `"VP\0\0"` header
  crashes the helper at +28.
- Three GCM samples updated to use the new helpers.  New
  `samples/gcm/hello-ppu-cellgcm-{drawenv,textured-cube,vp-loop}`
  build PKGs and exercise the new paths end-to-end.
- `cmake/ps3-self.cmake` captures `CMAKE_CURRENT_LIST_DIR` at file
  load time so default asset paths resolve relative to the
  toolchain root rather than the caller's CMakeLists.
- `scripts/package-windows-release.sh` stages `cmake/`, `templates/`,
  and `sdk/assets/ICON0.PNG` inside the release tarball so the
  `ps3_add_pkg` defaults work for users who unpack and run
  `cmake -S sample`.

### Other

- `cell/sysmodule.h` — add `CELL_SYSMODULE_SYNC2 = 0x0055`.
- `tools/Cargo.lock` synced to workspace 0.4.0 (was missed in the
  v0.3.0 round).
- Comment-only sweep: rename "smoke test" -> "validation" across
  12 samples and 5 docs to match what the samples actually are
  (end-to-end functional validation, not minimal smoke-tests).

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

The byte-diff suite against `the reference Cg compiler` at `--O2 --fastmath` grows from
59/59 to **65/65** byte-identical:

- **Repeated-add scale-fold (full vec4)** generalises the pattern
  matcher to N=2..64.  The N≤3 path keeps the short MOVH+MAD shape;
  longer chains lower to `MOVH H0; MULR R1 = H0*2; (MADR R1 += H0*2)
  × (K-1); [FENCBR if K odd]; (ADD/MAD) R0[END]`.  Final write picks
  ADD vs MAD by carry parity.  Scalar-lane N≥4 stays capped at 3 —
  `the reference Cg compiler` switches to a DP2R shape there which is documented in
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
- **Three new GCM validation samples** that exercise the above features
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
  for symbols stripped from late-firmware public headers but still exported by
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
  Targets `spu-elf`.  C/C++17 SPU validation samples pass.
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
  - `nidgen` — FNID/NID database + reference-format stub archive emitter.
  - `coverage-report` — reference SDK vs install-tree coverage matrix.
  - `abi-verify` — CellOS Lv-2 ABI conformance checker.  All 8 invariants
    pass on `hello-ppu-abi-check`.
  - `sprx-linker` — fork of `make_self`/`make_fself` companion.
  - `rsx-cg-compiler` — Cg → RSX (NV40) compiler.  46/46 byte-match
    against `the reference Cg compiler` for the stage-4 test corpus.
- **ABI:**
  - Compact 8-byte `.opd` descriptors (`ADDR32 + TLSGD-ABS`) replacing
    the 24-byte ELFv1 layout.  Two-read indirect-call sequence verified
    end-to-end in RPCS3 against decrypted reference SELFs.
  - `ATTRIBUTE_PRXPTR` (32-bit EA) applied to all pointer fields in
    `cell/*` structs that cross the SPRX boundary.
- **Reference-SDK builds:** unmodified `basic.cpp` (RSX triangle) and
  `5spu_spurs_without_context` (Spurs class surface) compile against
  this SDK using stock `the reference Cg compiler` shaders, and run in RPCS3.
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
