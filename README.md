# PS3 Custom Toolchain

A modern, open-source toolchain and SDK for the PlayStation 3, supporting **C++17** on both the PowerPC 64 PPU (PPE) and the Synergistic Processing Units (SPU) of the IBM Cell Broadband Engine.

Built on the ps3dev baseline (`ps3toolchain` + `PSL1GHT` + `ps3libraries`) rebased onto current-generation compilers, with auto-generated stub libraries driven by a NID/FNID database and a ≥95% coverage target against the reference cell-SDK surface for the subsystems homebrew actually uses.

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
the current version pins are a starting point, not a ceiling.  Each
upgrade is gated on the PPU patch set rebasing cleanly and the SPE
backend coming along for the ride, but both are active goals.

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

Early development.  See `docs/coverage.md` for an up-to-date API
coverage matrix once the PSL1GHT runtime has been built.

## Build host

**Native Linux** is the primary build host (CachyOS / any Arch derivative or Ubuntu 22.04+). Linux gives the cleanest GCC cross-build experience and matches CI.

The toolchain itself is targeted at running on **both Linux and Windows** for end users. Windows-hosted binaries (`powerpc64-ps3-elf-gcc.exe` etc.) are produced by Canadian-cross builds — building on Linux with `--host=x86_64-w64-mingw32` and a Mingw-w64 cross-compiler.  This is a follow-up infra deliverable; the initial shipment is Linux-hosted only.

## Getting started

On CachyOS / Arch:
```bash
sudo pacman -S --needed base-devel gcc gmp mpfr libmpc isl python rust \
    git wget bison flex texinfo cmake ninja patch
```

On Ubuntu/Debian:
```bash
sudo apt install -y build-essential gcc-12 g++-12 cmake ninja-build texinfo \
    bison flex libgmp-dev libmpfr-dev libmpc-dev libisl-dev python3 \
    git wget rustc cargo patch
```

Then:
```bash
# Activate the toolchain environment
source ./scripts/env.sh

# First-time setup (clones upstream + ps3dev repos)
./scripts/bootstrap.sh

# Smoke test — binutils only
./scripts/build-ppu-toolchain.sh --only binutils
./scripts/build-spu-toolchain.sh --only binutils

# Full toolchain + PSL1GHT + portlibs
# (each takes 10s of minutes; gcc-newlib is the longest)
./scripts/build-ppu-toolchain.sh
./scripts/build-spu-toolchain.sh
./scripts/build-psl1ght.sh
./scripts/build-portlibs.sh
```

Both toolchain build scripts accept `--only <step>` (binutils | gcc-newlib | gdb | symlinks) and `--clean` to wipe intermediates.

After a successful build, toolchain binaries live under `$PS3DEV/bin/` (aliased as `ppu-gcc`, `spu-gcc`, etc.).

## Layout

- `scripts/` — build orchestrators and environment setup
- `src/` — vendored upstream and ps3dev sources (reproducible via `bootstrap.sh`; `.gitignored`)
- `patches/` — rebased and new patches against upstream sources
- `tools/` — Rust CLIs: `nidgen`, `stubgen`, `coverage-report`
- `stage/ps3dev/` — the `$PS3DEV` install prefix (bin, ppu, spu, psl1ght, portlibs)
- `samples/` — minimal C++17 demos for validation
- `ci/` — Docker images and GitHub Actions
- `docs/` — quickstart, migration guide, ABI reference

## License

The glue code in this repository is released under the MIT License (see `LICENSE`). Imported sources retain their original licenses (GCC: GPLv3 with exception; binutils, GDB: GPLv3; newlib: BSD variants; PSL1GHT: MIT; ps3libraries components: various). See `NOTICE` for attribution and per-component license details.
