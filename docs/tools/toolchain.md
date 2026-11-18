# The cross toolchain: compilers, binutils, GDB and CMake

This page covers the parts of the SDK that every PS3 program goes through: the
PPU and SPU cross-compilers, binutils, the PPU debugger, the environment
variables, the CMake toolchain files and helper module, and the post-link chain
that turns an ELF into a bootable SELF. Each of the newer host tools has its own
page; see the [tool index](README.md).

## Components

| Component | Version | Target triple | Purpose |
|---|---|---|---|
| GCC (PPU) | 16.1.0 | `powerpc64-ps3-elf` | C, C++ and assembler for the PPE |
| GCC (SPU) | 16.1.0 | `spu-elf` | C, C++ and assembler for the SPUs, plus the `libgcc` software cache manager. The SPU backend is forward-ported from GCC 9.5.0, the last release that carried it. |
| binutils | 2.42 | both | `as`, `ld`, `ar`, `ranlib`, `objcopy`, `objdump`, `nm`, `strip`, `readelf`, `size`, `strings`, `addr2line`, `c++filt`; a small PS3 patch series for each target (`patches/ppu/binutils-2.42`, `patches/spu/binutils-2.42`) |
| newlib | 4.4.0 | both | C library, with the PS3 `libsysbase` / lv2 syscall glue (CRT0, `_write_r`, `_sbrk_r`, `_read_r` and so on) |
| GDB | 14.2 | PPU only | `powerpc64-ps3-elf-gdb`, for RPCS3's GDB server |
| PSL1GHT (v3) | pinned in `scripts/bootstrap.sh` | PPU + SPU | homebrew runtime and the classic host helpers (`make_self`, `fself`, `bin2s`, `make_self_npdrm`, `package_finalize`, `cgcomp`) |
| portlibs | pinned per recipe | PPU | zlib, libpng, pixman, FreeType, cairo, libzip, tiny3d + libfont3d, and more; recipes in `scripts/portlibs/` |

Why these versions were chosen is in [toolchain-design.md](../toolchain-design.md).

## Where the binaries are

| Install | PPU tools | SPU tools | Host tools |
|---|---|---|---|
| Linux, from source (`source scripts/env.sh`) | `$PS3DEV/ppu/bin/powerpc64-ps3-elf-*` | `$PS3DEV/spu/bin/spu-elf-*` | `$PS3DEV/bin`, mirrored to `$PS3DK/bin` |
| Windows release zip (after `setup.cmd`) | `%PS3DK%\ppu\bin\powerpc64-ps3-elf-*.exe` | `%PS3DK%\spu\bin\spu-elf-*.exe` | `%PS3DK%\bin\*.exe` |

Every program also answers to a short name: `ppu-gcc`, `ppu-g++`, `ppu-ld`,
`ppu-objcopy`, ... and `spu-gcc`, `spu-ld`, ... . PSL1GHT-style Makefiles
(`ppu_rules`, `spu_rules`) call these names.

- On Linux the short names are symlinks made by the `symlinks` step of
  `build-ppu-toolchain.sh` / `build-spu-toolchain.sh`.
- On Windows a zip cannot hold symlinks, so each short name (`ppu-gcc.exe`, ...)
  is a copy of a small forwarder (`tools/tool-alias`). It maps
  `ppu-<tool>.exe` to `powerpc64-ps3-elf-<tool>.exe` and `spu-<tool>.exe` to
  `spu-elf-<tool>.exe` in its own directory, passes the command line through
  unchanged and exits with that program's exit code.

## Environment variables

| Variable | Meaning |
|---|---|
| `PS3DEV` | Toolchain install root. Linux: set by `scripts/env.sh` (default `<repo>/stage/ps3dev`, overridable). Windows: an in-session alias for `%PS3DK%`, set by `setup.cmd` with forward slashes so Makefile recipes run through `sh` keep it intact. |
| `PS3DK` | SDK root: headers, libraries, host tools. Linux: `$PS3DEV/ps3dk`. Windows: the directory the release zip was extracted to; set it once with `setx`. The PPU GCC driver reads it with `getenv()` in its link and include specs. |
| `PSL1GHT` | Alias for `PS3DK`, for code and Makefiles that still read the PSL1GHT name. |
| `PPU_PREFIX`, `SPU_PREFIX` | `$PS3DEV/ppu` and `$PS3DEV/spu` (set by `env.sh`). |
| `PS3_TOOLCHAIN_ROOT` | The repository checkout (set by `env.sh`). |
| `PS3_BUILD_ROOT` | Where the toolchain build scripts put build trees (default `$HOME/ps3tc/build`). |
| `ICON0` | Default `ICON0.PNG` for packaging (`sdk/assets/ICON0.PNG`). |
| `PS3DK_NO_DEFAULT_INCLUDE` | Set (and not `0`) to stop the PPU driver adding the SDK include directory by default. Used by the toolchain's and SDK's own builds. |

`scripts/env.sh` also puts `$PS3DEV/bin`, `$PPU_PREFIX/bin` and
`$SPU_PREFIX/bin` on `PATH` and sets `SOURCE_DATE_EPOCH` from the last commit.

### Windows: setup.cmd

`%PS3DK%\setup.cmd` activates the toolchain in the current `cmd` session:

- If `PS3DK` is not set it prints the `setx PS3DK "..."` line to run and exits
  with status 1. `setx` changes the registry, not the running shell, so open a
  new terminal afterwards.
- If `PS3DK` names a different directory than the one `setup.cmd` is in (for
  example after extracting a newer release beside an old one), it warns, prints
  the `setx` line for the new directory and activates the new directory for the
  session.
- It prepends `bin\`, `ppu\bin\` and `spu\bin\` to `PATH`, and sets `PS3DEV`
  and `PSL1GHT`.
- The PSL1GHT Makefile path needs `sh`, `sed`, `mkdir`, `rm`, `cp` and `uname`
  from Git for Windows. `setup.cmd` finds Git for Windows and appends its
  `usr\bin`; without it, it prints `winget install --id Git.Git -e` and exits
  with status 1.
- GNU make ships in `make\bin` and is appended to `PATH` only when no `make` is
  found already.
- It prints the GCC versions and an OK / MISSING line for `make`, `ppu-gcc`,
  `spu-gcc`, `sh`, `sed`, `cmake` and `ninja` (with the `winget` command for a
  missing CMake or Ninja). A missing CMake or Ninja is reported but does not
  fail.

The release zip layout under `%PS3DK%`: `bin\` host tools and `ICON0.PNG`;
`ppu\` and `spu\` (compilers, binutils, libraries, headers, target sysroot);
`cmake\` toolchain files and helpers; `ppu_rules`, `spu_rules`, `base_rules`,
`data_rules` Makefile fragments; `make\bin\`; `portlibs\`; `samples\`;
`setup.cmd`; `README.txt`; `CHANGELOG.md`.

## The PPU compiler

### Data model: ILP32 by default, LP64 with `-mlp64`

PPU objects are ELF64 with 32-bit C pointers by default (ILP32: `int`, `long`
and pointers are 32 bits), matching CellOS LV2. `-mlp64` selects the LP64 data
model. The two are separate multilibs: GCC is configured with
`MULTILIB_OPTIONS = mlp64`, so `-mlp64` links the `lp64/` variants of the start
files, `libgcc`, newlib and libstdc++, and the driver searches
`$PS3DK/ppu/lib/lp64` ahead of `$PS3DK/ppu/lib`. Do not mix ILP32 and LP64
objects in one link.

Plain `char` is signed in the default data model, and `wchar_t` is 16 bits in
both. The full contract (type sizes, calling convention, TOC, PRX layout) is in
[cellos-lv2-abi-spec.md](../abi/cellos-lv2-abi-spec.md).

Objects and archives built by a GCC 12.4 release must be rebuilt; so must SPU
objects built by an older SPU compiler. `build-ppu-toolchain.sh` refuses a
`$PS3DEV` that already holds a GCC 12.4 PPU compiler.

### What the driver adds for you

The `powerpc64-ps3-elf` driver carries the PS3 specs, so a plain
`powerpc64-ps3-elf-gcc main.c -o main.elf` builds a complete LV2 executable:

- Predefined macros: `__lv2ppu__`, `__CELLOS_LV2__`, `__STRICT_ALIGNED`, and in
  the ILP32 model `__LP32__`, `_LP32`, `__POINTER_32BIT__`.
- `-ffunction-sections -fdata-sections` are on by default.
- Include path: `-isystem $PS3DK/ppu/include` is added unless `-nostdinc` or
  `PS3DK_NO_DEFAULT_INCLUDE` is given. With `PS3DK` unset, the SDK installed
  beside the compiler is used.
- Start and end files: `lv2-crti.o`, `crtbegin.o`, `lv2-crt0.o`, `lv2-sprx.o`
  ... `crtend.o`, `lv2-crtn.o`.
- Linker script: `lv2.ld`.
- Libraries: `-L $PS3DK/ppu/lib --start-group -lsysbase -lc -lrt -llv2
  -llv2_stub -lc_stub --end-group` (plus `$PS3DK/ppu/lib/lp64` first under
  `-mlp64`). You only name the extra libraries your program uses.

### Language defaults

- C++: GNU C++17 by default; C++20 and C++23 are one `-std=` option away.
  libstdc++ is built with the POSIX thread model, so `std::thread`,
  `std::mutex` and friends work (backed by the pthread layer in `librt`; see
  [pthreads.md](../sdk/pthreads.md)).
- C: GNU C23 by default, and an implicit function declaration is an error.

### Recommended flags

The CMake toolchain file uses these, and the manual commands below match it:

| Use | Flags |
|---|---|
| Every compile and link | `-mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections` |
| Release | `-O2 -Wall` |
| Debug | `-O0 -g -Wall` |
| Assembler | `-mcpu=cell` |
| LP64 | add `-mlp64` to every compile, assemble and link |

## The SPU compiler

`spu-elf-gcc` / `spu-elf-g++` build SPU programs for the 256 KB local store.
The SPU toolchain installs a PS3 local-store linker script, `spu_ps3.ld`, under
`$PS3DEV/spu/share/ld` (the `linker-script` step of `build-spu-toolchain.sh`).

Recommended flags (from `cmake/ps3-spu-toolchain.cmake`):

| Use | Flags |
|---|---|
| C | `-Os -ffunction-sections -fdata-sections` |
| C++ | the C flags plus `-fno-exceptions -fno-rtti` |
| Link | `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections` |
| Release / Debug | `-Os -Wall` / `-Og -g -Wall` |

An SPU program reaches the PPU executable embedded as data; see
[spu-elf-to-ppu-obj.md](spu-elf-to-ppu-obj.md) and the `ps3_add_spu_image`
CMake helper. SPU vector types and intrinsics come from the SPU headers under
`$PS3DK/spu/include`.

## binutils

Both targets get the full binutils 2.42 set under their triple:
`powerpc64-ps3-elf-as`, `-ld`, `-ar`, `-ranlib`, `-objcopy`, `-objdump`, `-nm`,
`-strip`, `-readelf`, `-size`, `-strings`, `-addr2line`, `-c++filt`, and the
`spu-elf-` equivalents. They behave as upstream binutils does; the PS3 patches
are target support. Typical uses:

```sh
powerpc64-ps3-elf-objdump -d -r build/main.o        # disassemble with relocations
powerpc64-ps3-elf-readelf -S -s app.elf             # sections and symbols
powerpc64-ps3-elf-nm -C libfoo.a                    # demangled symbols in an archive
powerpc64-ps3-elf-strip app.elf -o app.stripped.elf
spu-elf-objdump -d worker.spu.elf
```

## GDB (PPU)

`powerpc64-ps3-elf-gdb` (GDB 14.2) attaches to RPCS3's GDB server. Load the
unstripped executable for symbols:

```sh
powerpc64-ps3-elf-gdb build/my_app
(gdb) target remote 127.0.0.1:2345
```

The CMake helper `ps3_add_debug_rpcs3(<target> [GDB_SERVER host:port]
[BREAK symbol])` generates a `.gdbinit` and a `debug-<target>` build target that
boots RPCS3 and attaches. Limits and setup: [debugging-rpcs3.md](../debugging-rpcs3.md).

## CMake

Requirements: CMake 3.20 or newer and a generator. Ninja is recommended; Unix
Makefiles and MinGW Makefiles also work. CMake and Ninja are not in the release
zip (install them with `winget`, `choco install cmake ninja`, the Visual Studio
installer's CMake/Ninja components, or your distribution's packages). On
Windows always pass `-G`: without it CMake picks the Visual Studio generator.

### Toolchain files

| File | Use |
|---|---|
| `cmake/ps3-ppu-toolchain.cmake` | PPU, ILP32 (the default) |
| `cmake/ps3-ppu-toolchain-lp64.cmake` | PPU, LP64: adds `-mlp64` to every compile and link, links from `$PS3DK/ppu/lib/lp64` |
| `cmake/ps3-spu-toolchain.cmake` | standalone SPU projects |

They live at `<repo>/cmake` in a checkout and `%PS3DK%\cmake` in the Windows
release. Each one:

- stops with an error if `PS3DEV` is not set (it tells you to `source
  scripts/env.sh` or run `setup.cmd`), and takes `PS3DK` from the environment or
  `$PS3DEV/ps3dk`;
- sets the compilers, `ar`, `ranlib`, `strip`, `objcopy` and `objdump` (with
  `.exe` on Windows hosts);
- sets the flags in the tables above, adds the SDK include directories
  (`$PS3DK/ppu/include` and `simdmath`, or `$PS3DK/spu/include`), and defaults
  `CMAKE_CXX_STANDARD` to 17;
- for the PPU, links `librt` into every target;
- adds the `cmake/` directory to `CMAKE_MODULE_PATH`, so `include(ps3-self)`
  works.

The PPU toolchain files also carry the profiler's opt-in cache options; they are
described in the [profiler guide](../profiler-guide.md).

### Building a sample

```sh
cd samples/toolchain/hello-ppu-c++17
cmake -S . -B cmake-build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$PS3DK/../cmake/ps3-ppu-toolchain.cmake
cmake --build cmake-build
```

```cmd
:: Windows, after %PS3DK%\setup.cmd
cd "%PS3DK%\samples\toolchain\hello-ppu-c++17"
cmake -S . -B cmake-build -G Ninja -DCMAKE_TOOLCHAIN_FILE="%PS3DK%\cmake\ps3-ppu-toolchain.cmake"
cmake --build cmake-build
```

`cmake --build cmake-build --verbose` prints every command, which is the
quickest way to see the exact flags a sample uses.

### The ps3-self helper module

`include(ps3-self)` loads `cmake/ps3-self.cmake`. At configure time it finds
`sprxlinker`, `make_self`, `make_self_npdrm` and `fself` in `$PS3DEV/bin` then
`$PS3DK/bin` (and stops if one is missing), and it checks that the installed
SDK is complete and current against `cmake/ps3-required-artifacts.txt` and the
install manifest, so a stale or partial install fails at configure time rather
than at link time.

| Function | What it does | Details |
|---|---|---|
| `ps3_add_self(<target> [OUTPUT_DIRECTORY dir])` | post-build: strip, `sprxlinker`, `make_self`, `fself` | below |
| `ps3_add_debug_rpcs3(<target> [GDB_SERVER ip:port] [BREAK sym])` | debug in RPCS3 with GDB | [debugging-rpcs3.md](../debugging-rpcs3.md) |
| `ps3_bin2s(<target> <file>)` | embed a binary file | below |
| `ps3_add_cg_shader_rsxcgc(<target> <file.vcg/.fcg>)` | compile a Cg shader with `rsx-cg-compiler` and embed it | [rsx-cg-compiler.md](rsx-cg-compiler.md) |
| `ps3_add_cg_shader(<target> <file>)` | legacy: compile with PSL1GHT's `cgcomp` and embed it | below |
| `ps3_add_spu_image(<target> NAME ... SOURCES ...)` | build an SPU program and link it into a PPU target | [spu-elf-to-ppu-obj.md](spu-elf-to-ppu-obj.md) |
| `ps3_add_prx`, `ps3_add_sprx`, `ps3_prx_stub_library`, `ps3_prx_export_roundtrip` | PRX modules and their importers | [prx-gen.md](prx-gen.md) |
| `ps3_make_sdata(<output> <input> ...)` | SDATA files for `cellFsSdataOpen` | [make-sdata.md](make-sdata.md) |
| `ps3_add_pkg(<target> CONTENTID ...)` | installable `.pkg` | [pkg.md](pkg.md) |

#### ps3_add_self

```cmake
include(ps3-self)
add_executable(my_app source/main.cpp)
target_link_libraries(my_app PRIVATE sysutil m)
ps3_add_self(my_app)
```

After the link it runs `strip`, `sprxlinker`, `make_self` and `fself` and
writes three files to the output directory:

| File | What it is | How to run it |
|---|---|---|
| `<target>.elf` | stripped ELF, rewritten by `sprxlinker` | `ps3load`, or GDB against RPCS3 with the unstripped build output |
| `<target>.self` | signed SELF | `rpcs3 --no-gui <target>.self`; signed hardware |
| `<target>.fake.self` | fake-signed SELF | RPCS3; `ps3load` / XMB install on CFW hardware |

The output directory is the target's build directory
(`CMAKE_CURRENT_BINARY_DIR`, so `cmake-build/` in the example above) unless you
pass `OUTPUT_DIRECTORY <dir>` or set `PS3_SELF_OUTPUT_DIRECTORY` for the whole
project (relative paths resolve against the build directory;
`set(PS3_SELF_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")` puts them next to
`CMakeLists.txt`). The raw CMake executable in the build directory is the
unstripped intermediate. `ps3_add_pkg` follows the same directory.

An LP64 target is passed `sprxlinker --lp64` (from the LP64 toolchain file, or
from `-mlp64` in the target's link options). Current `sprxlinker` accepts that
flag for older build rules and ignores it: it reads what it needs from the ELF
itself. Extra `sprxlinker` flags can be given in `PS3_SPRXLINKER_FLAGS`.

#### ps3_bin2s

`ps3_bin2s(<target> <file>)` runs `bin2s` on a file (relative to the source
directory, or an absolute or generated path), assembles the result into the
target and writes a header beside it in the build directory. The symbol stem is
the file name with dots turned into underscores and a leading digit prefixed
with `_` (`9.png` becomes `_9_png`), as PSL1GHT's `data_rules` do. The header
declares `<id>[]`, `<id>_end[]` and `<id>_size`:

```cmake
ps3_bin2s(my_app data/logo.png)     # #include "logo_png.h"; logo_png, logo_png_end, logo_png_size
```

#### Shaders

`ps3_add_cg_shader_rsxcgc(<target> <file>)` is the recommended path: `.vcg`
compiles with profile `sce_vp_rsx` to `<stem>.vpo`, `.fcg` with `sce_fp_rsx` to
`<stem>.fpo`, under `<build>/shaders/`, and the result is embedded with
`ps3_bin2s` (symbols `<stem>_vpo` / `<stem>_fpo`). Extra compiler options go in
the `PS3_RSXCGC_FLAGS` list. The output is a `CgBinaryProgram` blob, the layout
the `cellGcmCg*` helpers read.

`ps3_add_cg_shader(<target> <file>)` uses PSL1GHT's `cgcomp` instead (`-v` for
`.vcg`, `-f` for `.fcg`). Its blob layout is the one PSL1GHT's
`rsxLoadVertexProgram` / `rsxLoadFragmentProgramLocation` read, so code using
those calls must keep it; the two layouts are not interchangeable. `cgcomp` is
still bundled so older PSL1GHT Makefiles keep building; a future release will
install it only behind an explicit build flag.

## Building without CMake

The pipeline is **compile, link, strip, sprxlinker, sign**. These commands match
what the PPU toolchain file and `ps3-self.cmake` run. Replace
`powerpc64-ps3-elf-` with `spu-elf-` for SPU code.

**1. Compile** each source file:

```cmd
powerpc64-ps3-elf-gcc.exe -O2 -Wall -mcpu=cell ^
    -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections ^
    -I"%PS3DK%\ppu\include" -I"%PS3DK%\ppu\include\simdmath" ^
    -c source\main.c -o build\main.o
```

For C++ use `powerpc64-ps3-elf-g++.exe` and add a `-std=` if you need other
than C++17.

**2. Compile shaders** (`.vcg` / `.fcg`):

```cmd
rsx-cg-compiler.exe -p sce_vp_rsx --emit-container build\vpshader.vpo shaders\vpshader.vcg
rsx-cg-compiler.exe -p sce_fp_rsx --emit-container build\fpshader.fpo shaders\fpshader.fcg
```

**3. Embed binary data** (shaders, images, fonts):

```cmd
bin2s.exe -a 64 build\vpshader.vpo > build\vpshader_vpo.s
powerpc64-ps3-elf-as.exe -o build\vpshader_vpo.o build\vpshader_vpo.s
```

`ps3_bin2s` also writes a header with the `extern` declarations (see above);
by hand, write that header yourself.

**4. Link.** The driver adds the CRT objects, `lv2.ld` and the base libraries
(see [What the driver adds for you](#what-the-driver-adds-for-you)); name only
the extra libraries:

```cmd
powerpc64-ps3-elf-gcc.exe build\main.o build\vpshader_vpo.o build\fpshader_fpo.o ^
    -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections ^
    -Wl,-Map,sample.elf.map -L"%PS3DK%\ppu\lib" ^
    -lgcm_cmd -lgcm_sys -lio -lsysutil -lsysmodule -lrt -llv2 -lm ^
    -o sample.elf
```

**5. Strip, sprxlinker, sign** (host tools in `%PS3DK%\bin`):

```cmd
powerpc64-ps3-elf-strip.exe sample.elf -o build\sample.elf
sprxlinker.exe              build\sample.elf
make_self.exe               build\sample.elf sample.self
fself.exe                   build\sample.elf sample.fake.self
```

See [sprx-linker.md](sprx-linker.md) for what `sprxlinker` changes.

**6. Package** (optional): see [pkg.md](pkg.md) for the `.pkg` steps.

The extra libraries, include paths and standards each sample needs are in its
`CMakeLists.txt`.

## Building the toolchain from source

Build on native Linux (or WSL2 on Windows). Host packages, the build order, the
Windows `.exe` cross-build and the install checks are in
[building.md](../building.md). Both toolchain scripts take `--only <step>` and
`--clean`:

| Script | Steps |
|---|---|
| `scripts/build-ppu-toolchain.sh` | `binutils`, `gcc-newlib`, `gdb`, `symlinks` |
| `scripts/build-spu-toolchain.sh` | `binutils`, `gcc-newlib`, `symlinks`, `linker-script` |
