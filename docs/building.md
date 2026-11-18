# Building the toolchain and SDK from source

Most users do not need this: the Windows release zip and the Linux toolchain
tarballs on the GitHub Releases page are complete. Build from source to work on
the toolchain itself or to run it on a Linux host without a release.

## Build host

**Native Linux** is the primary build host. Any reasonably current distribution
works (`glibc` >= 2.31, GCC >= 11 on the host, Python 3, Rust 1.88+). It gives
the cleanest GCC cross-build and matches CI. **On Windows, build inside WSL2**
(Ubuntu 24.04 recommended; see [below](#windows-via-wsl2)); native MSYS2 is not a
supported bootstrap path. macOS is untested.

The toolchain itself runs on both Linux and Windows. The Windows-hosted `.exe`
binaries are cross-built from Linux with `--host=x86_64-w64-mingw32` and a
MinGW-w64 toolchain, and every release ships them in the Windows SDK zip.

## Host dependencies

Debian / Ubuntu (`apt`):

```bash
sudo apt install -y build-essential gcc-12 g++-12 cmake ninja-build texinfo \
    bison flex libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev \
    libreadline-dev libncurses-dev libexpat1-dev libssl-dev libelf-dev \
    python3 python3-dev git wget patch autoconf automake libtool \
    zip unzip
```

Ubuntu 24.04 ships `rustc` 1.75 in apt, which cannot build the Rust workspace
under `tools/` (its locked dependencies need Rust 1.88+). Install Rust with
`rustup` instead:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
. "$HOME/.cargo/env"
rustc --version    # should report 1.88+
```

CI uses `dtolnay/rust-toolchain@stable`; CachyOS, Fedora and Arch already ship a
recent enough `rustc`.

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

On any other distribution, install equivalents of: a C/C++ host toolchain, GMP /
MPFR / MPC / ISL / zlib / readline / ncurses / expat development headers,
Python 3, Rust + Cargo, Git, Wget, Bison, Flex, Texinfo, CMake, Ninja and Patch.

## Windows (via WSL2)

Autotools and GCC's combined-tree build are reliable on Linux and brittle under
MSYS2, so on Windows build inside WSL2. Using a prebuilt release on Windows does
not need WSL.

1. Install WSL2 and a distribution from an elevated PowerShell or `cmd`:
   ```powershell
   wsl --install -d Ubuntu-24.04
   # other supported picks: archlinux, Debian, Fedora-43
   ```
   The first run asks for a Linux user name and password.
2. Open the WSL shell (`wsl -d Ubuntu-24.04`) and install the
   [host dependencies](#host-dependencies) for that distribution.
3. Clone inside the WSL file system, not under `/mnt/c/`. The GCC build does
   millions of small-file operations; ext4 finishes in minutes where the
   9P-bridged Windows volume can take hours:
   ```bash
   git clone https://github.com/FirebirdTA01/PS3_Custom_Toolchain.git ~/PS3_Custom_Toolchain
   cd ~/PS3_Custom_Toolchain
   ```
   If you also keep a checkout on the Windows side for editing, add it as a
   remote:
   ```bash
   git remote add windows /mnt/c/path/to/PS3_Custom_Toolchain
   git fetch windows && git pull windows main   # after committing on the Windows side
   ```
4. Continue with the [normal build](#building-the-toolchain-and-sdk).

WSL2 notes:

- The default `PS3_BUILD_ROOT` (`$HOME/ps3tc/build`) is already on ext4.
- To use the Linux-hosted toolchain from Windows-native tools, copy
  `stage/ps3dev/` to the Windows side (`cp -r stage/ps3dev /mnt/c/ps3dev`) and
  set `PS3DEV=C:\ps3dev` in your Windows shell.
- Cross-building the Windows `.exe` toolchain needs the extra MinGW-w64 packages
  listed [below](#windows-release-builds).

## Building the toolchain and SDK

```bash
# Activate the environment (exports PS3DEV, PS3DK, PPU_PREFIX, ...)
source ./scripts/env.sh

# First-time setup: clones the upstream sources under src/.
./scripts/bootstrap.sh

# Quick check: binutils only, both targets (about a minute).
./scripts/build-ppu-toolchain.sh --only binutils
./scripts/build-spu-toolchain.sh --only binutils

# Full toolchain + PSL1GHT + SDK headers + stub archives + portlibs + SDK.
# This takes a while: GCC and newlib are built twice (PPU and SPU).  The
# steps must run in this order, and each exits non-zero on failure, so
# chaining with && stops at the first failure.
./scripts/build-spu-toolchain.sh &&
./scripts/build-ppu-toolchain.sh &&
./scripts/build-psl1ght.sh &&
./scripts/build-sdk.sh --headers-only &&
./scripts/build-cell-stub-archives.sh &&
./scripts/build-portlibs.sh &&
./scripts/build-sdk.sh
```

- The final `build-sdk.sh` first builds and installs the host tools
  (`rsx-cg-compiler`, `sprxlinker`, `pkg`, `psgl-shader-builder` and the Rust
  tools, through `scripts/install-host-tools.sh`), because the SDK build
  compiles shaders with `rsx-cg-compiler`. It needs CMake, a host C/C++ compiler
  and Rust. `--no-host-tools` skips them for an SDK-only rebuild once they are
  installed.
- `build-cell-stub-archives.sh` uses `nidgen` to produce the stub archives the
  declaration-only cell headers need at link time; see
  [tools/nidgen.md](tools/nidgen.md).
- `build-portlibs.sh` builds every recipe in `scripts/portlibs/`, or only the
  ones you name (`build-portlibs.sh 060-tiny3d`).
- Both toolchain scripts take `--only <step>` and `--clean` (wipe intermediates
  before rebuilding). PPU steps: `binutils`, `gcc-newlib`, `gdb`, `symlinks`.
  SPU steps: `binutils`, `gcc-newlib`, `symlinks`, `linker-script`.
- `build-ppu-toolchain.sh` builds GCC 16.1.0 only and refuses a `$PS3DEV` that
  already holds a GCC 12.4 PPU compiler: install into a fresh prefix, then
  rebuild PSL1GHT, the portlibs, the SDK and your own objects against it. In an
  existing checkout, re-run `bootstrap.sh` first so it fetches
  `releases/gcc-16.1.0`.

The toolchain installs under `$PS3DEV` (`ppu/bin`, `spu/bin`, with the short
`ppu-gcc`, `spu-gcc`, ... names). The PSL1GHT runtime and the SDK headers and
archives (for example `libgcm_cmd.a`, `libdbgfont.a`) install into `$PS3DK`
(`$PS3DEV/ps3dk`).

To build only the Rust tools:

```bash
cd tools
cargo build --release      # binaries in tools/target/release/
```

To build only `rsx-cg-compiler`:

```bash
cmake -B tools/rsx-cg-compiler/build -S tools/rsx-cg-compiler
cmake --build tools/rsx-cg-compiler/build
# binary: tools/rsx-cg-compiler/build/rsx-cg-compiler
```

## Verify the install

```bash
$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc --version   # 16.1.0
$PS3DEV/spu/bin/spu-elf-gcc --version             # 16.1.0
ls $PS3DEV/ps3dk/ppu/lib/libgcm_cmd.a
ls $PS3DEV/ps3dk/ppu/include/cell/gcm.h
```

If one fails, that build step did not finish cleanly; rerun it with `--clean`.

A quick end-to-end check under RPCS3:

```bash
cd samples/toolchain/hello-ppu-abi-check
cmake -S . -B cmake-build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=../../../cmake/ps3-ppu-toolchain.cmake
cmake --build cmake-build
rpcs3 --no-gui cmake-build/hello-ppu-abi-check.self
# Output goes to RPCS3's TTY.log (~/.cache/rpcs3/TTY.log on Linux);
# a clean run ends with "result: PASS".
```

## Windows release builds

Only needed to produce `ps3-sdk-vX.Y.Z-windows-x86_64.zip` yourself. Install
MinGW-w64 on the Linux host:

```bash
sudo apt install -y mingw-w64 g++-mingw-w64-x86-64 zip           # Debian / Ubuntu
sudo dnf install -y mingw64-gcc mingw64-gcc-c++ mingw64-binutils \
    mingw64-headers mingw64-crt mingw64-winpthreads zip           # Fedora / RHEL / Rocky / Alma
sudo pacman -S --needed mingw-w64-gcc zip                         # Arch / Manjaro / EndeavourOS
which x86_64-w64-mingw32-gcc && x86_64-w64-mingw32-gcc --version
```

Then:

```bash
source ./scripts/env.sh
./scripts/build-ppu-toolchain.sh --host=x86_64-w64-mingw32   # ~15-30 min
./scripts/build-spu-toolchain.sh --host=x86_64-w64-mingw32   # ~10-20 min
./scripts/build-host-tools-windows.sh                        # ~10-15 min
./scripts/package-windows-release.sh                         # ~5 min
# stage/release/ps3-sdk-vX.Y.Z-windows-x86_64.zip
```

`build-host-tools-windows.sh` cross-compiles `make_self.exe`,
`make_self_npdrm.exe`, `package_finalize.exe`, `fself.exe`, `bin2s.exe`,
`cgcomp.exe`, `sprxlinker.exe`, `rsx-cg-compiler.exe`, GNU make and the
`tool-alias` forwarder (the short `ppu-*.exe` / `spu-*.exe` names), with their
static zlib / gmp / openssl dependencies built from upstream tarballs, into
`stage/host-tools-windows/bin/`; `package-windows-release.sh` picks them up.

The Rust tools (every binary in `tools/Cargo.toml`, such as `nidgen.exe`,
`abi-verify.exe`, `prx-gen.exe`) need a `rustup`-managed Rust, because Arch's
`rust` package and Ubuntu's `rustc` ship only the host standard library.
Install rustup (`pacman -S rustup` on Arch, `curl https://sh.rustup.rs | sh`
elsewhere), then:

```bash
rustup default stable && rustup target add x86_64-pc-windows-gnu
```

Without rustup, `build-host-tools-windows.sh` skips the Rust step; take those
`.exe`s from the CI `host-tools-windows-x86_64` artifact and pass it with
`package-windows-release.sh --tools-zip=PATH`.

Release packaging and versioning: [RELEASING.md](RELEASING.md),
[VERSIONING.md](VERSIONING.md).
