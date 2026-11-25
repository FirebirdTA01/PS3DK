# PS3 Custom Toolchain

A modern, open-source toolchain and SDK for the PlayStation 3. It is C++17-first on both the PowerPC 64 PPU and the Synergistic Processing Units (SPU), with GCC 16.1.0 on both PPU and SPU (the SPU backend is forward-ported from GCC 9.5.0, the last release that carried it). The PPU compiler builds C++ as GNU C++17 by default; C++20 and C++23 are one `-std=` option away.

The SDK now has its own runtime, native cell-style headers, NID/FNID-driven stub archives, fragment-shader tooling, and CMake sample flow. PSL1GHT remains a supported front-end for existing homebrew, while new code can use either PSL1GHT-style or cell-style APIs against the same installed runtime.

## Current status

- **Toolchain:** PPU GCC 16.1.0, SPU GCC 16.1.0, binutils 2.42, newlib 4.4.0, and PPU GDB 14.2 build and install end-to-end on Linux. libstdc++ has the POSIX thread model (`std::thread`, `std::mutex` and friends).
- **PPU ABI:** default PPU binaries use ELF64 with 32-bit C pointers for CellOS compatibility. `-mlp64` selects the LP64 multilib. Plain `char` is signed in the default data model, `wchar_t` is 16 bits in both, and the `E*` names LV2 defines take its status codes. Objects and archives built by a GCC 12.4 release must be rebuilt. See [docs/abi/cellos-lv2-abi-spec.md](docs/abi/cellos-lv2-abi-spec.md).
- **C dialect:** GCC 16 builds C as GNU C23, and an implicit function declaration is an error.
 - **SDK export coverage:** 4338 of 4805 tracked exports (90.3%) across 99 tracked libraries are covered by the v0.28.8 install tree; 95 libraries are complete, and the remaining gaps are in `libm_stub`, `libstdc++_stub`, `libsync2_stub` and `liblv2_stub`. The full generated matrix is in [docs/coverage.md](docs/coverage.md).
- **Network and NP coverage:** the current network-stack wave covers cellNetCtl, sys_net, SSL, HTTP/HTTPUtil, and the NP family (`sysutil_np`, NP2, trophy, TUS, clans, commerce2, SNS, and utility surfaces), adding roughly 700 exports; 13 network validation samples exercise it.
 - **Samples:** 228 CMake sample projects: toolchain 52, sysutil 41, GCM 37, LV2 20, SPURS 15, codec 14, network 13, profiling 10, SPU 6, audio 5, firmware helper 4, sprx 3, font 2, input 2, dbgfont 1, debug 1, util 1 and vision 1, plus 21 PSL1GHT sample ports. See [samples/README.md](samples/README.md).
- **Install contract:** `make -C sdk install` produces the installed headers, native archives, multilib stub archives, and `$PS3DK/.ps3dk-install-manifest`; `cmake/ps3-self.cmake` refuses stale installs at configure time.

## Direction

The repo is growing from a refreshed ps3dev/PSL1GHT baseline into a fuller SDK under `sdk/`: GCM command emitters, sysutil and NP stub archives, native LV2 runtime pieces, shader tooling, ABI verification, and samples all live here. Existing PSL1GHT code stays supported through compatibility headers and library aliases; new code can use the native cell-style surface directly.

## Toolchain components

| Component | Version | Target | Purpose |
|---|---|---|---|
| GCC (PPU) | **16.1.0** | `powerpc64-ps3-elf` | PowerPC 64 cross-compiler for the PPE |
| GCC (SPU) | **16.1.0** | `spu-elf` | Cross-compiler for the SPE's SPU cores + `libgcc` cache manager |
| Binutils | **2.42** | both targets | Assembler, linker, `ar` / `objcopy` / `nm` / `strip` for both PPU and SPU; small PS3 patch series for each target (`patches/ppu/binutils-2.42`, `patches/spu/binutils-2.42`) |
| Newlib | **4.4.0** | both targets | C standard library + PS3 `libsysbase` / `libgloss` lv2 syscall glue (CRT0, `_write_r` / `_sbrk_r` / `_read_r` etc.) |
| GDB | **14.2** | PPU | Debugger for RPCS3's gdbstub and (eventually) real-hardware targets |
| PSL1GHT (v3) | pinned `f649a08` | PPU + SPU runtime | Homebrew runtime rebased for the platform ABI-style naming; source-compat shim keeps PSL1GHT-targeted homebrew building |
| portlibs | pinned per recipe | PPU | zlib 1.3.1, libpng 1.6.58, pixman 0.42.2, FreeType 2.13.2, cairo 1.16.0, libzip 1.11.4, tiny3d + libfont3d; ps3soundlib opt-in (`scripts/portlibs/`) |
| Tooling | in-tree | host | `rsx-cg-compiler` (Cg vertex/fragment shader compiler); Rust tools for NID/FNID and stub archives, ABI checks, PRX/SPRX and SFO generation, SPU image embedding, SDATA, overlays, coverage reports; `sprxlinker`; `pkg` ([tool guides](docs/tools/README.md)) |

Toolchain-version rationale is in [docs/toolchain-design.md](docs/toolchain-design.md). Current upgrade work is tracked in [docs/roadmap.md](docs/roadmap.md). The detailed PPU ABI contract, including the ELF64+ILP32 default and LP64 multilib, is in [docs/abi/cellos-lv2-abi-spec.md](docs/abi/cellos-lv2-abi-spec.md). POSIX threads (v0.12.0+) come from a pthread shim inside `librt.a` — where its semantics match hosted POSIX and where they deliberately differ is in [docs/sdk/pthreads.md](docs/sdk/pthreads.md).

## Documentation

| Topic | Guide |
|---|---|
| Compilers, binutils, GDB, flags, environment, CMake toolchain files and helpers, building without CMake | [docs/tools/toolchain.md](docs/tools/toolchain.md) |
| Every host tool, one guide each | [docs/tools/README.md](docs/tools/README.md) |
| Shader compiler | [docs/tools/rsx-cg-compiler.md](docs/tools/rsx-cg-compiler.md) |
| PARAM.SFO (`sfo`, `sfo-gui`) | [docs/tools/sfo.md](docs/tools/sfo.md) |
| Installable packages (`ps3_add_pkg`) | [docs/tools/pkg.md](docs/tools/pkg.md) |
| PRX / SPRX modules (`prx-gen`, `ps3_add_prx`) | [docs/tools/prx-gen.md](docs/tools/prx-gen.md) |
| Embedding SPU programs | [docs/tools/spu-elf-to-ppu-obj.md](docs/tools/spu-elf-to-ppu-obj.md) |
| Running a SELF on a console over the network (`ps3dk-run`) | [docs/tools/ps3dk-run.md](docs/tools/ps3dk-run.md) |
| Profiler | [docs/profiler-guide.md](docs/profiler-guide.md) |
| Building from source | [docs/building.md](docs/building.md) |
| Samples | [samples/README.md](samples/README.md) |
| ABI | [docs/abi/cellos-lv2-abi-spec.md](docs/abi/cellos-lv2-abi-spec.md) |
| Debugging in RPCS3 | [docs/debugging-rpcs3.md](docs/debugging-rpcs3.md) |

## Profiler

`libps3dkprof` records named PPU zones and frames into a `.prof` capture, optionally with SPU-lane and RSX-domain
zones (experimental); `ps3dk-prof` checks and reports a capture, and `ps3dk-prof-viewer` shows it, from a file or live
over the network while the app runs.  How to enable and link it, use it on the PS3 and on the PC, and capture a frame:
[docs/profiler-guide.md](docs/profiler-guide.md).

## Install (Windows release)

The Windows SDK zip `ps3-sdk-vX.Y.Z-windows-x86_64.zip` is attached to every release on the GitHub Releases page. It is self-contained: the `.exe` cross-compilers, the runtime libraries (PSL1GHT + our SDK), the CMake toolchain files and helper modules under `cmake\`, portlibs, and the `samples\` source tree. Linux users can take the `ps3dev-ppu` / `ps3dev-spu` toolchain tarballs from the same page, or [build from source](#building-from-source).

1. **Extract the archive** somewhere stable. The path you pick is the value of `%PS3DK%`. `tar -xf` (Windows 10+) and 7-Zip are the tested extractors. (The v0.11.15 archive only: PowerShell `Expand-Archive` and Explorer read its timestamps as local time, so within 12 hours of publication files could land "in the future" and make CMake/Ninja loop; later archives carry normalised timestamps and extract correctly with any tool.)
2. **Set `PS3DK` permanently** for your user account, in a regular `cmd` window, then open a new terminal (`setx` does not update the shell that runs it):
   ```cmd
   setx PS3DK "C:\path\to\extracted\PS3DK"
   ```
3. **Activate the environment** in each new shell. `setup.cmd` puts the toolchain on `PATH` and sets `PS3DEV` / `PSL1GHT` as aliases ([details](docs/tools/toolchain.md#windows-setupcmd)):
   ```cmd
   cd "%PS3DK%"
   setup.cmd
   ```
4. **Check the compilers.** Both print `(GCC) 16.1.0`:
   ```cmd
   powerpc64-ps3-elf-gcc.exe --version
   spu-elf-gcc.exe --version
   ```

## Quick start

Samples build with CMake 3.20+ and a generator (Ninja recommended; neither ships in the zip: `winget install --id Kitware.CMake -e` and `winget install --id Ninja-build.Ninja -e`, or `choco install cmake ninja`). Each sample is its own CMake project:

```cmd
:: Windows, after %PS3DK%\setup.cmd
cd "%PS3DK%\samples\toolchain\hello-ppu-c++17"
cmake -S . -B cmake-build -G Ninja -DCMAKE_TOOLCHAIN_FILE="%PS3DK%\cmake\ps3-ppu-toolchain.cmake"
cmake --build cmake-build
```

```bash
# Linux, after source scripts/env.sh
cd samples/toolchain/hello-ppu-c++17
cmake -S . -B cmake-build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PS3DK/../cmake/ps3-ppu-toolchain.cmake
cmake --build cmake-build
```

The build writes `hello-ppu-c++17.elf` (stripped), `.self` (signed: RPCS3, `rpcs3 --no-gui hello-ppu-c++17.self`) and `.fake.self` (CFW hardware via `ps3load` or XMB install) to `cmake-build/`. A sample's `CMakeLists.txt` calls `include(ps3-self)` and `ps3_add_self(<target>)`; shaders, embedded SPU programs, PRX modules and `.pkg` packages are one more helper call each. See [docs/tools/toolchain.md](docs/tools/toolchain.md) for the helpers, the output-directory options and the equivalent commands without CMake, and [samples/README.md](samples/README.md) for what each sample validates.

## Building from source

Build on native Linux, or inside WSL2 on Windows:

```bash
source ./scripts/env.sh
./scripts/bootstrap.sh
./scripts/build-spu-toolchain.sh &&
./scripts/build-ppu-toolchain.sh &&
./scripts/build-psl1ght.sh &&
./scripts/build-sdk.sh --headers-only &&
./scripts/build-cell-stub-archives.sh &&
./scripts/build-portlibs.sh &&
./scripts/build-sdk.sh
```

Host packages for each distribution, WSL2 setup, per-step options, install checks and the Windows `.exe` cross-build are in [docs/building.md](docs/building.md).

## Layout

- `scripts/` — build orchestrators and environment setup
- `src/` — vendored upstream and ps3dev sources (reproducible via `bootstrap.sh`; `.gitignored`)
- `patches/` — rebased and new patches against upstream sources
- `tools/` — the Rust workspace (`nidgen`, `coverage-report`, `abi-verify`, `prx-gen`, `sfo`, `make-sdata`, `ovis`, `spu-elf-to-ppu-obj`, the profiler tools), the C++ `rsx-cg-compiler`, the C `sprx-linker` and `pkg`; user guides in [docs/tools/](docs/tools/README.md)
- `stage/ps3dev/` — the `$PS3DEV` install prefix (bin, ppu, spu, ps3dk, portlibs)
- `samples/` — tracked CMake sample projects across toolchain, GCM, audio, codec, sysutil, firmware helper, LV2, SPU, SPURS, and network categories
- `.github/workflows/` — CI (`ci.yml`) + tag-driven release (`release.yml`)
- `docs/` — tool guides, build guide, coverage matrix, ABI reference, toolchain design notes, roadmap, `VERSIONING.md`
- `CHANGELOG.md` — release-by-release history (Keep a Changelog format)

## Versioning and releases

The SDK ships a single version string sourced from the most recent
`vMAJOR.MINOR.PATCH` git tag.  See `docs/VERSIONING.md` for the cut-a-release
workflow and `CHANGELOG.md` for the release-by-release history.  Tagged
releases are published on the GitHub Releases page, with the Windows SDK zip,
the Windows and Linux host-tools packages, the source tarball and the Linux
`ps3dev-ppu` / `ps3dev-spu` toolchain tarballs attached by CI (the source,
host-tools and Windows SDK assets each carry a `.sha256`).

## License

The glue code in this repository is released under the MIT License (see `LICENSE`). Imported sources retain their original licenses (GCC: GPLv3 with exception; binutils, GDB: GPLv3; newlib: BSD variants; PSL1GHT: MIT; ps3libraries components: various). See `NOTICE` for attribution and per-component license details.
