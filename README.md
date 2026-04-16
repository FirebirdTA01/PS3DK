# PS3 Custom Toolchain

A modern, open-source toolchain and SDK for the Sony PlayStation 3, supporting **C++17** on both the PowerPC 64 PPU (PPE) and the Synergistic Processing Units (SPU) of the IBM Cell Broadband Engine.

Built on the ps3dev baseline (`ps3toolchain` + `PSL1GHT` + `ps3libraries`) rebased onto current-generation compilers, with auto-generated stub libraries driven by a NID/FNID database and a ≥95% coverage target against the official Sony PS3 SDK surface for the subsystems homebrew actually uses.

## What this is

- **PPU compiler**: GCC 12.4.0 targeting `powerpc64-ps3-elf`. Full C++17, solid C++20.
- **SPU compiler**: GCC 9.5.0 targeting `spu-elf` (last GCC with an intact SPU backend). C++17 supported.
- **Binutils**: 2.42 for both PPU and SPU. Still ships `spu-elf` upstream.
- **Newlib**: 4.4.0 with PS3 `libsysbase` / `libgloss` syscall glue.
- **GDB**: 14.2 for PPU-side debugging against RPCS3's gdbstub.
- **PSL1GHT v3**: Sony-style naming (`cellXxx`, `CELL_XXX_*`, `CellXxx`) with a compatibility shim for legacy homebrew. Fragment-shader-capable RSX (NV40-FP assembler first, GLSL/Cg compiler later).
- **portlibs**: zlib 1.3.1, libpng 1.6.43, SDL2 2.30, libcurl 8.9, mbedTLS 3.6, and more.
- **Tooling**: Rust CLIs for NID/FNID generation, stub archive emission, Sony-SDK-vs-PSL1GHT coverage reports.

## Status

Early development. See `docs/coverage.md` once Phase 3 lands for an up-to-date API coverage matrix.

The authoritative implementation plan is at `C:/Users/FirebirdTA01/.claude/plans/i-m-going-to-create-melodic-dawn.md`.

## Build host

**Native Linux** is the primary build host (CachyOS / any Arch derivative or Ubuntu 22.04+). Linux gives the cleanest GCC cross-build experience and matches CI.

The toolchain itself is targeted at running on **both Linux and Windows** for end users. Windows-hosted binaries (`powerpc64-ps3-elf-gcc.exe` etc.) are produced by Canadian-cross builds — building on Linux with `--host=x86_64-w64-mingw32` and a Mingw-w64 cross-compiler. This is a Phase 5 infra deliverable; the initial shipment is Linux-hosted only.

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

# Phase 1a smoke test — binutils only
./scripts/build-ppu-toolchain.sh --only binutils
./scripts/build-spu-toolchain.sh --only binutils

# Full Phase 1+2+3+4 (each takes 10s of minutes; gcc-newlib is the longest)
./scripts/build-ppu-toolchain.sh
./scripts/build-spu-toolchain.sh
./scripts/build-psl1ght.sh
./scripts/build-portlibs.sh
```

Both toolchain build scripts accept `--only <step>` (binutils | gcc-newlib | gdb | symlinks) and `--clean` to wipe intermediates.

After a successful build, toolchain binaries live under `$PS3DEV/bin/` (aliased as `ppu-gcc`, `spu-gcc`, etc.).

## Sony SDK reference mount

The official Sony PS3 SDK is used privately as an API coverage oracle. It is **never committed** and **never shipped**. To mount:

```bat
cmd /c mklink /J reference\sony-sdk "D:\path\to\your\Sony\SDK\475.001"
icacls reference\sony-sdk /deny %USERNAME%:(WD,DC)
```

`reference/sony-sdk/` is in `.gitignore` at the top level.

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
