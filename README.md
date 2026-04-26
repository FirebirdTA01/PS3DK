# PS3 Custom Toolchain

A modern, open-source toolchain and SDK for the PlayStation 3, supporting **C++20** on the PowerPC 64 PPU (PPE) and **C++17** on both the PPE and the Synergistic Processing Units (SPU) of the IBM Cell Broadband Engine.

Built on the ps3dev baseline (`ps3toolchain` + `PSL1GHT` + `ps3libraries`) rebased onto current-generation compilers, with auto-generated stub libraries driven by a NID/FNID database and a ≥95% coverage target against the reference cell-SDK surface for the subsystems homebrew actually uses.

## Direction

This project started fully on PSL1GHT and is incrementally growing out
from it.  Our own SDK lives under `sdk/` and replaces pieces of the
PSL1GHT runtime one subsystem at a time — GCM command emitters, the
sysutil forwarder family, stub archives built from a NID database, the
RSX Cg compiler, etc.  The long-term shape is:

- **Our own runtime + libraries** carry the bulk of the SDK surface,
  written against the reference cell-SDK ABI where that matters for
  binary / SPRX compatibility.
- **PSL1GHT stays a permanently-supported front-end.** A
  source-compat shim keeps PSL1GHT-targeted homebrew building
  unchanged even as the runtime underneath it is replaced.  Existing
  homebrew codebases do not have to migrate; new code can pick either
  the PSL1GHT-style or the cell-SDK-style surface and both land on the
  same runtime.

Nothing about PSL1GHT API is getting deprecated — it stays a supported
input forever.  What's changing is how much of the runtime behind it
is ours.

## Toolchain components

| Component | Version | Target | Purpose |
|---|---|---|---|
| GCC (PPU) | **12.4.0** | `powerpc64-ps3-elf` | PowerPC 64 cross-compiler for the PPE |
| GCC (SPU) | **9.5.0** | `spu-elf` | Cross-compiler for the SPE's SPU cores + `libgcc` cache manager |
| Binutils | **2.42** | both targets | Assembler, linker, `ar` / `objcopy` / `nm` / `strip` for both PPU and SPU; `spu-elf` ships upstream intact |
| Newlib | **4.4.0.20231231** | both targets | C standard library + PS3 `libsysbase` / `libgloss` lv2 syscall glue (CRT0, `_write_r` / `_sbrk_r` / `_read_r` etc.) |
| GDB | **14.2** | PPU | Debugger for RPCS3's gdbstub and (eventually) real-hardware targets |
| PSL1GHT (v3) | main | PPU + SPU runtime | Homebrew runtime rebased for cell-SDK-style naming; source-compat shim keeps PSL1GHT-targeted homebrew building |
| portlibs | current stable | PPU | zlib 1.3.1, libpng 1.6.43, SDL2 2.30, libcurl 8.9, mbedTLS 3.6, etc. |
| Tooling | in-tree | host | Rust CLIs for NID/FNID generation, stub-archive emission, coverage reports |

### Why these GCC versions

- **PPU at GCC 12.4.0.** 12.x is the highest line that rebases onto the PSL1GHT PPU patch set without middle-end churn bleeding into the backend.  12.4.0 gives full **C++17**, substantially complete **C++20**, and partial **C++23** (`-std=c++2b`).  libstdc++ ships with ranges, `<format>`, concepts, coroutines, most of the parallel algorithms, and the `<chrono>` calendar/time-zone surface.
- **SPU at GCC 9.5.0.** 9.5.0 is the **last upstream GCC that still ships a working Cell SPE backend** — GCC 10 removed `gcc/config/spu/`, `libgcc/config/spu/`, the SPU intrinsic headers, and the SPU test suite outright (≈34 000 lines total).  9.5.0 gives full **C++17** (core language + libstdc++).  C++20 support is **early / partial**: some constexpr extensions, aggregate-init fixes, three-way-comparison groundwork, and experimental `-std=c++2a` are in; the bulk of C++20 (finalised concepts, ranges, `<format>`, spaceship library, `constinit` / `consteval`, modules, coroutines) landed in GCC 10+ and is **not** available on SPU today.  Usability is constrained mostly by the 256 KiB local-store budget rather than the compiler version: libstdc++ headers are available but feature selection is pragmatic (no RTTI / no exceptions / `-Os` by default).

### Upgrade roadmap

We intend to track upstream GCC over time for **both** PPU and SPU —
the current version pins are a starting point.  Each upgrade is gated 
on the PPU patch set rebasing cleanly and the SPE backend coming 
along for the ride, but both are active goals.

- **PPU: GCC 13 → 14 → current stable.** Routine patch-set rebase
  work across GCC 13's middle-end-to-backend interface shifts and GCC
  14's further `machine_mode` / `target hook` API tightening.  Pay-off
  is modules in libstdc++, fuller C++23 library coverage, and
  progressively more C++26 paper coverage.  Planned as the PPU patch
  stack stabilises.
- **SPU: resurrect the SPE backend and track GCC forward.** GCC 10
  removed `gcc/config/spu/`, `libgcc/config/spu/`, the SPU intrinsic
  headers, and the SPU test suite (≈34 000 lines total); the
  forward-port workstream (see `patches/spu/FORWARD_PORT_README.md`)
  picks 9.5.0's SPE backend up, modernises it against GCC 12's
  middle-end (the `rs6000.c` → `rs6000.cc` split pattern, new pass
  infrastructure, `machine_mode` → `scalar_int_mode` /
  `scalar_float_mode` tightening, libgcc build-system updates, and
  intrinsic-header validation), and then tracks upstream alongside
  PPU.  End state is a unified GCC version across PPU + SPU, full
  C++20 / partial C++23 on SPU, and a single CI matrix.  This is a
  substantial compiler-engineering effort, but it *is* the plan — not
  a perpetual maybe.
- **Newlib.** Tracking upstream; 4.4.0 is the current pin.  Next bump
  is re-evaluated when a feature we need lands upstream or when the
  PS3 `libsysbase` glue needs a material rewrite.
- **GDB.** Same — 14.2 is the current pin; bumps happen when RPCS3's
  gdbstub gains features we want to drive, or to track debugger
  improvements upstream.

### Other components

- **PSL1GHT v3**: cell-SDK-style naming (`cellXxx`, `CELL_XXX_*`, `CellXxx`) alongside a source-compat shim so PSL1GHT-targeted homebrew continues to build unchanged — PSL1GHT stays a supported front-end even as we replace most of its runtime behind the scenes.  Fragment-shader-capable RSX (NV40-FP assembler first, full Cg compiler at `tools/rsx-cg-compiler/`).

## Status

Active development; toolchain and core SDK are usable end-to-end.

- **Toolchain (PPU + SPU):** built and validated.  Both cross-compilers,
  `binutils`, `newlib`, and PPU `gdb` produce working artefacts; the SPU
  GCC 9.5.0 fork is patched and stable on `spu-elf`.
- **SDK surface:** 41.6% of the reference cell-SDK export set across 98
  libraries (1981 / 4758 symbols), driven by the NID/FNID database and
  the stub-archive pipeline.  Several subsystems are at 100% — `libaudio`,
  `libgcm_sys` (with legacy-name shims), `libio` (pad/kb/mouse), `libc`,
  `liblv2`, `libspurs` (PPU + SPU task runtime), `libsysutil_audio_out`,
  and a dozen more.  See `docs/coverage.md` for the full matrix.
- **Runtime path:** native compact-OPD ELFv1 LV2 runtime (`runtime/lv2/`)
  ships and is the default for new PPU samples; PSL1GHT's runtime stays
  available as a fallback.  Compact `.opd` (8-byte descriptors) is end-
  to-end runnable in RPCS3 with full `printf` output.
- **Samples:** 34 samples build successfully and a representative subset
  (cellGcm rendering, cellAudio playback, cellPad input, Spurs taskset
  + SPU task, lv2 event-flag with PPU↔SPU sync, sysutil callbacks,
  msgdialog, savedata, gamedata, screenshot, l10n, jpg/png decode,
  cellGcmDbgFont) runs end-to-end in RPCS3.
- **rsx-cg-compiler:** 57/57 test shaders byte-identical to the
  reference Cg compiler at `--O2 --fastmath`. See "Optional: rsx-cg-compiler"
  below for what's covered.

For the up-to-date API coverage matrix, regenerate
`docs/coverage.md` after a build with
`cargo run --release --bin coverage-report` from `tools/`.

## Build host

**Native Linux** is the primary build host — any reasonably current distribution (`glibc` ≥ 2.31, GCC ≥ 11 on the host, Python 3, Rust 1.70+) works.  Linux gives the cleanest GCC cross-build experience and matches CI.  **Windows users should build inside WSL2** (Ubuntu 24.04 recommended; see [Windows (via WSL2)](#windows-via-wsl2) below); native MSYS2 is not a supported bootstrap path.  macOS is untested.

The toolchain itself is targeted at running on **both Linux and Windows** for end users. Windows-hosted binaries (`powerpc64-ps3-elf-gcc.exe` etc.) are produced by cross-building from Linux with `--host=x86_64-w64-mingw32` and a Mingw-w64 toolchain.  This is a follow-up infra deliverable; the initial shipment is Linux-hosted only.

## Getting started

### Host dependencies

Pick the command that matches your distribution.  Any recent version of the listed distros works; older LTS releases may need a PPA / module / COPR for a modern Rust toolchain.

Debian / Ubuntu (`apt`):
```bash
sudo apt install -y build-essential gcc-12 g++-12 cmake ninja-build texinfo \
    bison flex libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev \
    libreadline-dev libncurses-dev libexpat1-dev python3 python3-dev git wget \
    rustc cargo patch autoconf automake libtool zip unzip
```

Fedora / RHEL / Rocky / Alma (`dnf`):
```bash
sudo dnf install -y @development-tools gcc gcc-c++ gmp-devel mpfr-devel \
    libmpc-devel isl-devel zlib-devel readline-devel ncurses-devel \
    expat-devel python3 python3-devel rust cargo git wget bison flex \
    texinfo cmake ninja-build patch zip unzip
```

Arch / Manjaro / EndeavourOS (`pacman`):
```bash
sudo pacman -S --needed base-devel gcc gmp mpfr libmpc isl zlib readline \
    ncurses expat python rust git wget bison flex texinfo cmake ninja patch \
    zip unzip
```

On any other distribution, install equivalents of: a C/C++ host toolchain, GMP / MPFR / MPC / ISL / zlib / readline / ncurses / expat development headers, Python 3, Rust + Cargo, Git, Wget, Bison, Flex, Texinfo, CMake, Ninja, and Patch.

### Windows (via WSL2)

Native Windows builds aren't supported for the toolchain bootstrap — autotools and GCC's combined-tree build are reliable on Linux and brittle under MSYS2.  The recommended path for Windows users is **WSL2** with one of the supported Linux distributions.

End users who just want to *use* a prebuilt toolchain on Windows do not need WSL — tagged releases will ship `ps3-sdk-vX.Y.Z-windows-x86_64.zip` with native `.exe` binaries (planned for v0.4.x; see [`docs/VERSIONING.md`](docs/VERSIONING.md)).  WSL2 is required only when *building from source*.

1. **Install WSL2 and a distro** from an elevated PowerShell or `cmd`:
   ```powershell
   wsl --install -d Ubuntu-24.04
   # other supported picks: archlinux, Debian, Fedora-43
   ```
   The first run prompts for a Linux username and password.

2. **Open the WSL shell** (`wsl -d Ubuntu-24.04`) and install the distro's host dependencies using the matching command from [Host dependencies](#host-dependencies) above.  Everything from `bootstrap.sh` onward runs identically to a native Linux host.

3. **Clone inside the WSL filesystem**, not on `/mnt/c/`.  GCC bootstrap performs millions of small-file operations and ext4 finishes in minutes where the 9P-bridged Windows volume can take hours:
   ```bash
   git clone https://github.com/FirebirdTA01/PS3_Custom_Toolchain.git ~/PS3_Custom_Toolchain
   cd ~/PS3_Custom_Toolchain
   ```
   If you also keep the repo checked out on the Windows side for editing, add it as a remote so you can pull edits in without round-tripping through GitHub:
   ```bash
   git remote add windows /mnt/c/path/to/PS3_Custom_Toolchain
   git fetch windows && git pull windows main   # after committing on the Windows side
   ```

4. **Continue with the normal build**: `source scripts/env.sh`, then `./scripts/bootstrap.sh`, then `./scripts/build-ppu-toolchain.sh` etc. as in [Building the toolchain + SDK](#building-the-toolchain--sdk).

A few WSL2-specific notes:

- The default `PS3_BUILD_ROOT` (`$HOME/ps3tc/build`) already lives in ext4 — no override needed.
- If you want to use the Linux-hosted toolchain from Windows-native tools, copy or expose `stage/ps3dev/` back to the Windows side: `cp -r stage/ps3dev /mnt/c/ps3dev`, then set `PS3DEV=C:\ps3dev` in your Windows shell.
- Cross-building the Windows toolchain release (the `.exe` binaries) requires the additional `mingw-w64 g++-mingw-w64-x86-64` packages on top of the standard Ubuntu install line (Debian/Fedora analogues exist).  This is documented separately under the v0.4.x build flow.

### Building the toolchain + SDK

```bash
# Activate the toolchain environment (exports PS3DEV, PPU_PREFIX, ...)
source ./scripts/env.sh

# First-time setup (clones upstream + ps3dev repos under src/).
./scripts/bootstrap.sh

# Smoke test — binutils only, both targets (~1 minute total).
./scripts/build-ppu-toolchain.sh --only binutils
./scripts/build-spu-toolchain.sh --only binutils

# Full toolchain + PSL1GHT + portlibs + our SDK.
#
# ** Heads-up: this takes a while. **  Bootstrapping GCC+newlib twice
# (once for PPU, once for SPU) is CPU-bound and is the bulk of the
# wait.  Kick it off and go do something else.
./scripts/build-ppu-toolchain.sh
./scripts/build-spu-toolchain.sh
./scripts/build-psl1ght.sh
./scripts/build-portlibs.sh
./scripts/build-sdk.sh
```

Both toolchain build scripts accept `--only <step>` (binutils | gcc-newlib | gdb | symlinks) and `--clean` to wipe intermediates before rebuilding.

After a successful build, toolchain binaries live under `$PS3DEV/bin/` (aliased as `ppu-gcc`, `spu-gcc`, etc.).  The PSL1GHT runtime lands under `$PS3DEV/psl1ght/ppu/`; our own SDK headers and archives (e.g. `libgcm_cmd.a`, `libdbgfont.a`) land under `$PS3DEV/ps3dk/ppu/` next to it.

### Verify the install

```bash
# Toolchain binaries respond with their version banner.
$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc --version   # -> 12.4.0
$PS3DEV/spu/bin/spu-elf-gcc         --version     # -> 9.5.0

# SDK headers + libraries are in place.
ls $PS3DEV/ps3dk/ppu/lib/libgcm_cmd.a
ls $PS3DEV/ps3dk/ppu/include/cell/gcm.h
```

All four checks should succeed silently (or print the expected version).  If any fails, the corresponding build step didn't finish cleanly — rerun it with `--clean` to wipe stale intermediates.

### Building and running samples

Samples under `samples/` each build with `make` from inside their own directory:

```bash
cd samples/toolchain/hello-ppu-c++17
make
# Produces: hello-ppu-c++17.elf / .self / .fake.self
```

Each sample produces three artefacts:

| Artefact | Target | How to run |
|---|---|---|
| `<name>.elf` | raw PPU ELF | `ps3load` or GDB against RPCS3's gdbstub |
| `<name>.self` | CEX-signed SELF | `rpcs3 --no-gui <name>.self` (boots on RPCS3 or signed hardware) |
| `<name>.fake.self` | fake-signed SELF | `ps3load` / XMB install on CFW hardware |

Quickest end-to-end check under RPCS3:

```bash
cd samples/toolchain/hello-ppu-abi-check
make
rpcs3 --no-gui hello-ppu-abi-check.self
# Stdout lands in ~/.cache/rpcs3/TTY.log — a clean run ends with
#   result: PASS
```

`samples/README.md` has the full sample index plus per-sample notes on what each one validates.

### Optional: Rust tooling

The repo ships Rust CLIs under `tools/` that drive the stub-archive pipeline and the coverage report:

- `nidgen` — NID extraction from reference stub archives, stub-archive emission, FNID verification.
- `coverage-report` — cell-SDK export set vs. our install tree coverage matrix → `docs/coverage.md`.

A normal toolchain + SDK + sample build doesn't need them.  Build them when you want to regenerate stub archives for non-PSL1GHT-backed sysutil libraries, or rerun the coverage report:

```bash
cd tools
cargo build --release
# Binaries land in tools/target/release/{nidgen,coverage-report}
```

Once `nidgen` is built, `scripts/build-cell-stub-archives.sh` produces the stub archives the declaration-only cell headers (libsysutil_screenshot, libsysutil_ap, libsysutil_music*, libl10n, …) need at link time:

```bash
./scripts/build-cell-stub-archives.sh
```

### Optional: rsx-cg-compiler (experimental)

`tools/rsx-cg-compiler/` is a clean-room Cg → RSX (NV40) shader compiler we're growing as an eventual drop-in replacement for PSL1GHT's `cgcomp`.

**It is experimental and growing.** The byte-diff harness against the reference Cg compiler at `--O2 --fastmath` reports **46 / 46 shaders byte-identical** today, covering vertex passthrough, MVP transforms, struct-flattened VP inputs, FP arithmetic + MAD fusion, saturation + fp16 precision, dot products, min/max, abs/neg, TXP (projective texture), samplerCUBE, the full Select scope (all 6 compare ops × all lanes × uniform/literal/varying RHS, ternary, if-only and if-else diamonds via if-conversion), and the SFU unaries `length` / `normalize` / `rcp` / `rsqrt`.  Open gaps: `for`/`while` loops (blocked on loop-carry SSA in the IR builder), multi-instruction if/else diamonds (only single-`stout` branches if-convert today), and varying-default if-only.  For production shader builds the recommendation is still PSL1GHT's `cgcomp` until the loop work lands; the `hello-ppu-cellgcm-triangle` sample can opt into our compiler with `make USE_RSX_CG_COMPILER=1` and renders byte-identically end-to-end in RPCS3.

To build it:

```bash
cmake -B tools/rsx-cg-compiler/build -S tools/rsx-cg-compiler
cmake --build tools/rsx-cg-compiler/build
# Binary: tools/rsx-cg-compiler/build/rsx-cg-compiler
```

## Layout

- `scripts/` — build orchestrators and environment setup
- `src/` — vendored upstream and ps3dev sources (reproducible via `bootstrap.sh`; `.gitignored`)
- `patches/` — rebased and new patches against upstream sources
- `tools/` — Rust CLIs: `nidgen` (NID + stub-archive emit), `coverage-report`, `abi-verify`, `sprx-linker`; plus the C++ `rsx-cg-compiler`
- `stage/ps3dev/` — the `$PS3DEV` install prefix (bin, ppu, spu, psl1ght, ps3dk, portlibs)
- `samples/` — minimal C++17 demos for validation
- `ci/` — Docker images and GitHub Actions
- `.github/workflows/` — CI (`ci.yml`) + tag-driven release (`release.yml`)
- `docs/` — quickstart, migration guide, ABI reference, `VERSIONING.md`
- `CHANGELOG.md` — release-by-release history (Keep a Changelog format)

## Versioning and releases

The SDK ships a single version string sourced from the most recent
`vMAJOR.MINOR.PATCH` git tag.  See `docs/VERSIONING.md` for the cut-a-release
workflow and `CHANGELOG.md` for the release-by-release history.  Tagged
releases are published on the GitHub Releases page; pre-release tarballs of
the source tree and the Rust tools are attached automatically by CI.

## License

The glue code in this repository is released under the MIT License (see `LICENSE`). Imported sources retain their original licenses (GCC: GPLv3 with exception; binutils, GDB: GPLv3; newlib: BSD variants; PSL1GHT: MIT; ps3libraries components: various). See `NOTICE` for attribution and per-component license details.
