# PS3 Custom Toolchain

A modern, open-source toolchain and SDK for the PlayStation 3. It is C++17-first on both the PowerPC 64 PPU and the Synergistic Processing Units (SPU), with GCC 12.4.0 on PPU and GCC 9.5.0 on SPU. The PPU compiler also has substantial C++20 and partial C++23 library support.

The SDK now has its own runtime, native cell-style headers, NID/FNID-driven stub archives, fragment-shader tooling, and CMake sample flow. PSL1GHT remains a supported front-end for existing homebrew, while new code can use either PSL1GHT-style or cell-style APIs against the same installed runtime.

## Current status

- **Toolchain:** PPU GCC 12.4.0, SPU GCC 9.5.0, binutils 2.42, newlib 4.4.0, and PPU GDB 14.2 build and install end-to-end on Linux.
- **PPU ABI:** default PPU binaries use ELF64 with 32-bit C pointers for CellOS compatibility. `-mlp64` selects the LP64 multilib. See [docs/abi/cellos-lv2-abi-spec.md](docs/abi/cellos-lv2-abi-spec.md).
- **SDK export coverage:** 3451 of 4796 tracked exports are covered by the installed SDK tree, or 72.0% across 99 tracked libraries. The full generated matrix is in [docs/coverage.md](docs/coverage.md).
- **Network and NP coverage:** the current network-stack wave covers cellNetCtl, sys_net, SSL, HTTP/HTTPUtil, and the NP family (`sysutil_np`, NP2, trophy, TUS, clans, commerce2, SNS, and utility surfaces), adding roughly 700 exports and 12 tracked network validation samples.
- **Samples:** 122 tracked CMake sample projects: toolchain 10, GCM 21, audio 2, codec 14, sysutil 37, firmware helper 4, LV2 7, SPU 3, SPURS 11, network 12, and dbgfont 1. See [samples/README.md](samples/README.md).
- **Install contract:** `make -C sdk install` produces the installed headers, native archives, multilib stub archives, and `$PS3DK/.ps3dk-install-manifest`; `cmake/ps3-self.cmake` refuses stale installs at configure time.

## Direction

The repo is growing from a refreshed ps3dev/PSL1GHT baseline into a fuller SDK under `sdk/`: GCM command emitters, sysutil and NP stub archives, native LV2 runtime pieces, shader tooling, ABI verification, and samples all live here. Existing PSL1GHT code stays supported through compatibility headers and library aliases; new code can use the native cell-style surface directly.

## Toolchain components

| Component | Version | Target | Purpose |
|---|---|---|---|
| GCC (PPU) | **12.4.0** | `powerpc64-ps3-elf` | PowerPC 64 cross-compiler for the PPE |
| GCC (SPU) | **9.5.0** | `spu-elf` | Cross-compiler for the SPE's SPU cores + `libgcc` cache manager |
| Binutils | **2.42** | both targets | Assembler, linker, `ar` / `objcopy` / `nm` / `strip` for both PPU and SPU; `spu-elf` ships upstream intact |
| Newlib | **4.4.0** | both targets | C standard library + PS3 `libsysbase` / `libgloss` lv2 syscall glue (CRT0, `_write_r` / `_sbrk_r` / `_read_r` etc.) |
| GDB | **14.2** | PPU | Debugger for RPCS3's gdbstub and (eventually) real-hardware targets |
| PSL1GHT (v3) | main | PPU + SPU runtime | Homebrew runtime rebased for cell-SDK-style naming; source-compat shim keeps PSL1GHT-targeted homebrew building |
| portlibs | current stable | PPU | zlib 1.3.1, libpng 1.6.43, SDL2 2.30, libcurl 8.9, mbedTLS 3.6, etc. |
| Tooling | in-tree | host | Rust CLIs for NID/FNID generation, stub-archive emission, coverage reports |

Toolchain-version rationale is in [docs/toolchain-design.md](docs/toolchain-design.md). Current upgrade work is tracked in [docs/roadmap.md](docs/roadmap.md). The detailed PPU ABI contract, including the ELF64+ILP32 default and LP64 multilib, is in [docs/abi/cellos-lv2-abi-spec.md](docs/abi/cellos-lv2-abi-spec.md).

## Build host

**Native Linux** is the primary build host. Any reasonably current distribution (`glibc` >= 2.31, GCC >= 11 on the host, Python 3, Rust 1.85+) works. Linux gives the cleanest GCC cross-build experience and matches CI. **Windows users should build inside WSL2** (Ubuntu 24.04 recommended; see [Windows (via WSL2)](#windows-via-wsl2) below); native MSYS2 is not a supported bootstrap path. macOS is untested.

The toolchain itself is targeted at running on **both Linux and Windows** for end users. Windows-hosted binaries (`powerpc64-ps3-elf-gcc.exe` etc.) are produced by cross-building from Linux with `--host=x86_64-w64-mingw32` and a Mingw-w64 toolchain.  This is a follow-up infra deliverable; the initial shipment is Linux-hosted only.

## Using a prebuilt Windows release

End users who don't need to build the toolchain from source can use `ps3-sdk-vX.Y.Z-windows-x86_64.zip` from the GitHub Releases page when a Windows release artifact is published. See [`docs/VERSIONING.md`](docs/VERSIONING.md) for release packaging. The archive is self-contained: it ships the `.exe` cross-compilers, the runtime libraries (PSL1GHT + our SDK), the CMake toolchain files + helper modules under `cmake\`, portlibs, and the `samples/` source tree.

### Install

1. **Extract the archive** somewhere stable.  The path you pick is the value of `%PS3DK%`.

2. **Set `PS3DK` permanently** for your user account.  In a regular `cmd` window:
   ```cmd
   setx PS3DK "C:\path\to\extracted\PS3DK"
   ```
   `setx` writes to the registry but does not update the shell that runs it — open a new terminal afterwards.  The PPU GCC driver reads `%PS3DK%` via `getenv()` in its link spec, so `-L` resolves directly against `%PS3DK%\ppu\lib` at link time.

3. **Activate the environment** in each new shell:
   ```cmd
   cd "%PS3DK%"
   setup.cmd
   ```
   `setup.cmd` prepends `bin\`, `ppu\bin\`, and `spu\bin\` to `%PATH%` for the current session and exports `PS3DEV` / `PSL1GHT` as in-session aliases for `%PS3DK%` (some legacy code paths still read those names).

4. **Check the toolchain binaries:**
   ```cmd
   powerpc64-ps3-elf-gcc.exe --version
   spu-elf-gcc.exe --version
   ```
   You should see `(GCC) 12.4.0` and `(GCC) 9.5.0`.

### Building samples from the prebuilt release

The bundled `%PS3DK%\samples\` tree contains the same tracked samples as `samples/` in this repo: toolchain checks, SPU examples, GCM rendering, SPURS taskset demos, sysutil callbacks, savedata, network-module validation, and more.

Samples build with **CMake**.  You need `cmake` (3.20+) and a generator on `PATH` — Ninja is recommended, but Unix Makefiles or "MinGW Makefiles" also work.  None of these ship in the toolchain zip; install via `choco install cmake ninja`, the Visual Studio installer's optional CMake/Ninja components, or any equivalent.

Each sample is its own standalone CMake project — `cd` into a sample, point CMake at the PPU toolchain file, and build:

```cmd
:: From a cmd shell after running %PS3DK%\setup.cmd
cd "%PS3DK%\samples\toolchain\hello-ppu-c++17"
cmake -S . -B cmake-build -DCMAKE_TOOLCHAIN_FILE="%PS3DK%\cmake\ps3-ppu-toolchain.cmake" -G Ninja
cmake --build cmake-build
```

The toolchain file (`cmake\ps3-ppu-toolchain.cmake` for PPU, `cmake\ps3-spu-toolchain.cmake` for standalone SPU work) reads `%PS3DK%` from the environment, sets the cross-compiler, and adds the SDK include / library search paths.  Each sample's `CMakeLists.txt` then `include(ps3-self)` to pull in the helper module that drives the strip → `sprxlinker` → `make_self` / `fself` post-build chain (and `bin2s` / SPU embedding / Cg shader compilation when needed).

Final artefacts land at the sample directory next to `CMakeLists.txt`; `cmake-build/` holds intermediates only:

| Artefact            | Target              | How to run                                     |
|---|---|---|
| `<name>.elf`        | stripped PPU ELF    | `ps3load` or GDB against RPCS3's gdbstub       |
| `<name>.self`       | CEX-signed SELF     | `rpcs3 --no-gui <name>.self` (RPCS3 / signed HW) |
| `<name>.fake.self`  | fake-signed SELF    | `ps3load` / XMB install on CFW hardware        |

`samples\README.md` has the full sample index plus per-sample notes on what each one validates.

### Manual / direct toolchain invocation (no CMake)

If you don't want to use CMake, the toolchain can be driven directly from `cmd` or PowerShell.  This is also the right path for IDE integration, custom build systems, or one-off compiles outside the CMake-driven sample tree.

The end-to-end pipeline for a typical PS3 homebrew app is **compile → link → strip → sprxlinker → sign**.  Below shows the command sequence for a PPU C/C++ sample; replace `powerpc64-ps3-elf-` with `spu-elf-` for SPU code.  Flag set matches what `cmake/ps3-ppu-toolchain.cmake` exports via `CMAKE_C_FLAGS_INIT` so the docs translate one-to-one.

**1. Compile each source file** (loop over .c / .cpp).  These are the same flags the PPU toolchain file emits:

```cmd
powerpc64-ps3-elf-gcc.exe ^
    -O2 -Wall -mcpu=cell ^
    -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections ^
    -I"%PS3DK%\ppu\include" ^
    -I"%PS3DK%\ppu\include\simdmath" ^
    -c source\main.c -o build\main.o
```

For C++ swap `-gcc` → `-g++` and append `-std=c++17` (or whichever standard the sample's `Makefile` sets in `CXXFLAGS`).

**2. Compile shaders** (only for samples with `.vcg` / `.fcg` files under `shaders\`).  Vertex profile is `sce_vp_rsx`, fragment profile is `sce_fp_rsx`:

```cmd
rsx-cg-compiler.exe -p sce_vp_rsx --emit-container build\vpshader.vpo shaders\vpshader.vcg
rsx-cg-compiler.exe -p sce_fp_rsx --emit-container build\fpshader.fpo shaders\fpshader.fcg
```

**3. Embed binary data** (shaders, images, fonts — anything the CMake helper `ps3_bin2s()` would handle).  Each `.vpo` / `.fpo` / `.png` / `.jpg` / `.bin` becomes an object file the linker pulls in:

```cmd
bin2s.exe -a 64 build\vpshader.vpo > build\vpshader_vpo.s
powerpc64-ps3-elf-as.exe -o build\vpshader_vpo.o build\vpshader_vpo.s
```

The CMake helper additionally writes a header (three `extern const u8`/`u32` declarations per blob, derived from the input filename) so C/C++ code can `#include "<name>_<ext>.h"` and reference the symbols.  When driving the toolchain by hand, generate or hand-write that header to match.

**4. Link everything into a PPU ELF.**  The linker spec inside `powerpc64-ps3-elf-gcc` reads `%PS3DK%` via `getenv()` and auto-includes `-L%PS3DK%\ppu\lib --start-group -lsysbase -lc -lrt -llv2 --end-group` plus the LV2 CRT objects (`lv2-crt0.o`, `lv2-crti.o`, `lv2-crtn.o`, `lv2-sprx.o`) and the linker script `lv2.ld`.  You only need to specify the *additional* libs the sample uses:

```cmd
powerpc64-ps3-elf-gcc.exe ^
    build\main.o build\vpshader_vpo.o build\fpshader_fpo.o ^
    -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections ^
    -Wl,-Map,sample.elf.map ^
    -L"%PS3DK%\ppu\lib" ^
    -lgcm_cmd -lgcm_sys -lio -lsysutil -lsysmodule -lrt -llv2 -lm ^
    -o sample.elf
```

**5. Strip + sprxlinker + SELF signing.**  Three host tools (shipped in `%PS3DK%\bin\`):

```cmd
powerpc64-ps3-elf-strip.exe sample.elf -o build\sample.elf
sprxlinker.exe              build\sample.elf
make_self.exe               build\sample.elf sample.self
fself.exe                   build\sample.elf sample.fake.self
```

`sample.self` boots on RPCS3 / signed HW; `sample.fake.self` boots on CFW hardware via XMB install or `ps3load`.

**6. (Optional) Build a `.pkg`** for installing the app on CFW hardware.  Requires `make_self_npdrm`, `sfo.py`, `pkg.py`, and `package_finalize`:

```cmd
mkdir build\pkg\USRDIR
copy "%PS3DK%\bin\ICON0.PNG" build\pkg\ICON0.PNG
make_self_npdrm.exe build\sample.elf build\pkg\USRDIR\EBOOT.BIN UP0001-HELPPU017_00-0000000000000000
sfo.py --title "Hello PPU" --appid HELPPU017 -f "%PS3DK%\bin\sfo.xml" build\pkg\PARAM.SFO
pkg.py  --contentid UP0001-HELPPU017_00-0000000000000000 build\pkg\ sample.pkg
copy sample.pkg sample.gnpdrm.pkg
package_finalize.exe sample.gnpdrm.pkg
```

`sample.pkg` is unsigned (debug-installable on CFW); `sample.gnpdrm.pkg` after `package_finalize` is the npdrm-finalised version.

The exact flag set each sample needs (extra `-l` libs, additional `-I` include paths, C++ standard overrides, etc.) is in that sample's `CMakeLists.txt`.  `cmake --build cmake-build --verbose` prints every command verbatim, so you can copy the lines you need and skip CMake entirely.

## Getting started

### Host dependencies

Pick the command that matches your distribution.  Any recent version of the listed distros works; older LTS releases may need a PPA / module / COPR for a modern Rust toolchain.

Debian / Ubuntu (`apt`):
```bash
sudo apt install -y build-essential gcc-12 g++-12 cmake ninja-build texinfo \
    bison flex libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev \
    libreadline-dev libncurses-dev libexpat1-dev libssl-dev libelf-dev \
    python3 python3-dev git wget patch autoconf automake libtool \
    zip unzip
```

Ubuntu 24.04 ships `rustc` 1.75 in apt, which can't build the Rust workspace under `tools/` (`clap_lex` 1.1+ requires Rust 1.85+).  Install Rust via `rustup` instead:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
. "$HOME/.cargo/env"
rustc --version    # should report 1.85+
```

(CI uses `dtolnay/rust-toolchain@stable` and CachyOS / Fedora / Arch already ship a recent enough `rustc` — this only matters when bootstrapping on a stale Debian/Ubuntu host.)

Fedora / RHEL / Rocky / Alma (`dnf`):
```bash
sudo dnf install -y @development-tools gcc gcc-c++ gmp-devel mpfr-devel \
    libmpc-devel isl-devel zlib-devel readline-devel ncurses-devel \
    expat-devel openssl-devel elfutils-libelf-devel python3 python3-devel \
    rust cargo git wget bison flex texinfo cmake ninja-build patch zip unzip
```

Arch / Manjaro / EndeavourOS (`pacman`):
```bash
sudo pacman -S --needed base-devel gcc gmp mpfr libmpc isl zlib readline \
    ncurses expat openssl libelf python rust git wget bison flex texinfo \
    cmake ninja patch zip unzip
```

On any other distribution, install equivalents of: a C/C++ host toolchain, GMP / MPFR / MPC / ISL / zlib / readline / ncurses / expat development headers, Python 3, Rust + Cargo, Git, Wget, Bison, Flex, Texinfo, CMake, Ninja, and Patch.

#### Optional: cross-build the Windows-hosted toolchain release

These extras are **only required** if you plan to run `./scripts/build-ppu-toolchain.sh --host=x86_64-w64-mingw32` (and the SPU equivalent) followed by `./scripts/package-windows-release.sh` to produce `ps3-sdk-vX.Y.Z-windows-x86_64.zip`.  A normal native Linux build does not need them.

`zip` is also listed below — it is already part of the main host-deps line above, repeated here so this section is self-contained for someone wiring up Windows-release builds on a host that hasn't run the full host-deps install.

Debian / Ubuntu (`apt`):
```bash
sudo apt install -y mingw-w64 g++-mingw-w64-x86-64 zip
```

Fedora / RHEL / Rocky / Alma (`dnf`):
```bash
sudo dnf install -y mingw64-gcc mingw64-gcc-c++ mingw64-binutils \
    mingw64-headers mingw64-crt mingw64-winpthreads zip
```

Arch / Manjaro / EndeavourOS (`pacman`):
```bash
sudo pacman -S --needed mingw-w64-gcc zip
```

After installing, confirm the cross compiler resolves:
```bash
which x86_64-w64-mingw32-gcc       # /usr/bin/x86_64-w64-mingw32-gcc
x86_64-w64-mingw32-gcc --version
```

The end-to-end Windows-release flow then runs as:

```bash
source ./scripts/env.sh
./scripts/build-ppu-toolchain.sh --host=x86_64-w64-mingw32   # ~15-30 min
./scripts/build-spu-toolchain.sh --host=x86_64-w64-mingw32   # ~10-20 min
./scripts/build-host-tools-windows.sh                        # ~10-15 min
./scripts/package-windows-release.sh                         # ~5 min
# stage/release/ps3-sdk-vX.Y.Z-windows-x86_64.zip ready to ship
```

`build-host-tools-windows.sh` cross-compiles `make_self.exe`, `make_self_npdrm.exe`, `package_finalize.exe`, `fself.exe`, and `rsx-cg-compiler.exe` plus their static `zlib`/`gmp`/`openssl` deps from upstream tarballs — those land under `stage/host-tools-windows/bin/` and `package-windows-release.sh` picks them up automatically.  The Rust workspace (`nidgen.exe`/`abi-verify.exe`/`coverage-report.exe`) requires a `rustup`-managed Rust because Arch's `rust` package and Ubuntu apt's `rustc` ship only the host's std lib; install rustup (`pacman -S rustup` on Arch, `curl https://sh.rustup.rs | sh` elsewhere) then `rustup default stable && rustup target add x86_64-pc-windows-gnu` before re-running.  Without rustup, `build-host-tools-windows.sh` skips the Rust step gracefully — pull those `.exe`s from the CI `host-tools-windows-x86_64` artifact instead and pass it via `package-windows-release.sh --tools-zip=PATH`.

### Windows (via WSL2)

Native Windows builds aren't supported for the toolchain bootstrap — autotools and GCC's combined-tree build are reliable on Linux and brittle under MSYS2.  The recommended path for Windows users is **WSL2** with one of the supported Linux distributions.

End users who just want to *use* a prebuilt toolchain on Windows do not need WSL when a tagged Windows release artifact is available. WSL2 is required only when *building from source*.

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
- Cross-building the Windows toolchain release (the `.exe` binaries) requires the additional `mingw-w64 g++-mingw-w64-x86-64` packages on top of the standard Ubuntu install line (Debian/Fedora analogues exist).

### Building the toolchain + SDK

```bash
# Activate the toolchain environment (exports PS3DEV, PPU_PREFIX, ...)
source ./scripts/env.sh

# First-time setup (clones upstream + ps3dev repos under src/).
./scripts/bootstrap.sh

# Quick validation: binutils only, both targets (~1 minute total).
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

Samples under `samples/` each build with **CMake** from inside their own directory:

```bash
cd samples/toolchain/hello-ppu-c++17
cmake -S . -B cmake-build \
      -DCMAKE_TOOLCHAIN_FILE=$PS3DK/../cmake/ps3-ppu-toolchain.cmake \
      -G Ninja
cmake --build cmake-build
# Produces: hello-ppu-c++17.elf / .self / .fake.self next to CMakeLists.txt
```

The toolchain file under `cmake/ps3-ppu-toolchain.cmake` reads `$PS3DEV` / `$PS3DK` from the environment (set by `source scripts/env.sh` at repo root or `setup.cmd` on Windows), points CMake at the cross-compiler, and adds the SDK include / library search paths.  Each sample's `CMakeLists.txt` calls `include(ps3-self)` and `ps3_add_self(<target>)` to wire up the strip → `sprxlinker` → `make_self` / `fself` post-build chain — and adds `ps3_add_cg_shader(...)`, `ps3_add_spu_image(...)`, or `ps3_bin2s(...)` for samples that need shader compilation, embedded SPU code, or data embedding.

Each sample produces three artefacts at the sample root (`cmake-build/` holds intermediates only):

| Artefact | Target | How to run |
|---|---|---|
| `<name>.elf` | stripped PPU ELF | `ps3load` or GDB against RPCS3's gdbstub |
| `<name>.self` | CEX-signed SELF | `rpcs3 --no-gui <name>.self` (boots on RPCS3 or signed hardware) |
| `<name>.fake.self` | fake-signed SELF | `ps3load` / XMB install on CFW hardware |

Quickest end-to-end check under RPCS3:

```bash
cd samples/toolchain/hello-ppu-abi-check
cmake -S . -B cmake-build \
      -DCMAKE_TOOLCHAIN_FILE=../../../cmake/ps3-ppu-toolchain.cmake \
      -G Ninja
cmake --build cmake-build
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

**It is experimental and growing.** The byte-diff harness covers vertex passthrough, MVP transforms, struct-flattened VP inputs, file-scope `: register(CN)` uniforms, FP arithmetic and MAD fusion, precision modifiers, texture ops, select/if-conversion, SFU unary intrinsics, constant folding, and FP tex-LHS write-back. Open gaps include loop lowering. For production shader builds the recommendation is still PSL1GHT's `cgcomp` until the loop work lands; the `hello-ppu-cellgcm-triangle` sample can opt into our compiler with `make USE_RSX_CG_COMPILER=1`.

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
- `samples/` — tracked CMake sample projects across toolchain, GCM, audio, codec, sysutil, firmware helper, LV2, SPU, SPURS, and network categories
- `ci/` — Docker images and GitHub Actions
- `.github/workflows/` — CI (`ci.yml`) + tag-driven release (`release.yml`)
- `docs/` — coverage matrix, ABI reference, toolchain design notes, roadmap, `VERSIONING.md`
- `CHANGELOG.md` — release-by-release history (Keep a Changelog format)

## Versioning and releases

The SDK ships a single version string sourced from the most recent
`vMAJOR.MINOR.PATCH` git tag.  See `docs/VERSIONING.md` for the cut-a-release
workflow and `CHANGELOG.md` for the release-by-release history.  Tagged
releases are published on the GitHub Releases page; pre-release tarballs of
the source tree and the Rust tools are attached automatically by CI.

## License

The glue code in this repository is released under the MIT License (see `LICENSE`). Imported sources retain their original licenses (GCC: GPLv3 with exception; binutils, GDB: GPLv3; newlib: BSD variants; PSL1GHT: MIT; ps3libraries components: various). See `NOTICE` for attribution and per-component license details.
