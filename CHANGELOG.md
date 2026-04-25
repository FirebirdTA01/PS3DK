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

[Unreleased]: https://github.com/FirebirdTA01/PS3_Custom_Toolchain/compare/v0.1.0...HEAD
[v0.1.0]: https://github.com/FirebirdTA01/PS3_Custom_Toolchain/releases/tag/v0.1.0
