# Windows toolchain release — design

**Status:** draft, awaiting review
**Owner:** FirebirdTA01
**Target milestone:** v0.4.x ("Windows toolchain release") per `docs/VERSIONING.md`

## Goal

Ship `ps3-sdk-vX.Y.Z-windows-x86_64.zip`: a self-contained Windows-installable
PS3 toolchain release whose `bin/` contains real native Windows executables
(`powerpc64-ps3-elf-gcc.exe`, `spu-elf-gcc.exe`, `powerpc64-ps3-elf-gdb.exe`,
plus the binutils suite for both targets), with target libraries
(libgcc, newlib libc, libstdc++) and the SDK runtime (PSL1GHT, portlibs,
our own SDK) bundled in the same archive.

End-user flow: extract the zip somewhere, set `PS3DEV` to the extract
root, prepend `%PS3DEV%\bin;%PS3DEV%\ppu\bin;%PS3DEV%\spu\bin` to PATH,
and `make` inside `samples/` works.

## Non-goals

- Macros installer (`.msi`, `setup.exe`) — out of scope; a zip + `setup.cmd`
  per the v0.4.x roadmap is enough.
- Native MSYS2 toolchain build directly on Windows. We pick cross-build
  from Linux instead because (a) autotools and GCC's combined-tree build
  are more reliable on real Linux, (b) CI uses Linux runners, and (c) it
  matches the path already documented in README.
- macOS toolchain — separate milestone (v1.0.0).

## Build flow

We build the Windows-hosted toolchain by **cross-building from Linux**:
configure GCC/binutils/GDB with `--build=x86_64-pc-linux-gnu
--host=x86_64-w64-mingw32 --target={powerpc64-ps3-elf | spu-elf}`. The
resulting binaries are PE/COFF executables that link against
mingw-w64 runtime DLLs (or are statically linked) and produce
PowerPC / SPU ELF for the PS3.

This requires three logical stages on the build host (Linux / WSL Ubuntu):

1. **Stage 0 — native Linux toolchain.** Run the existing
   `build-ppu-toolchain.sh` and `build-spu-toolchain.sh` to produce
   Linux-hosted `powerpc64-ps3-elf-gcc` and `spu-elf-gcc`. This is the
   compiler that builds the *target libraries* (libgcc, newlib,
   libstdc++) — those libraries are PowerPC / SPU ELF and don't depend
   on the host, so they're produced once during stage 0 and reused for
   the Windows package.

2. **Stage 1 — Windows-hosted binutils + GCC + GDB.** Re-run the build
   scripts in `--host=x86_64-w64-mingw32` mode. binutils, GCC's driver
   and cc1/cc1plus/lto1, and GDB are recompiled as `.exe` files that
   run on Windows. Target-library compilation is **skipped** — we copy
   the libgcc / newlib / libstdc++ trees produced in stage 0 into the
   Windows install prefix.

3. **Stage 2 — package.** Combine: stage 1 `.exe` binaries + stage 0
   target libs + PSL1GHT runtime + portlibs + our SDK + the host tools
   already built natively for Windows by `release.yml` (Rust CLIs,
   sprx-linker, rsx-cg-compiler) into a single zip.

Stages 0 and 1 share `src/upstream/` (already extracted) and
`patches/`. They differ only in the build directory and install prefix.

## Prefix layout

The native Linux build installs into `$PS3DEV` (default
`stage/ps3dev/`). The Windows-hosted build installs into a parallel
prefix to avoid collisions:

```
stage/
  ps3dev/                 # Linux-hosted (existing — stage 0)
    bin/
    ppu/                  # powerpc64-ps3-elf-gcc, libgcc.a, libstdc++.a, ...
    spu/                  # spu-elf-gcc, ...
    psl1ght/
    ps3dk/
    portlibs/
  ps3dev-windows/         # Windows-hosted (new — stage 1 install root)
    bin/                  # .exe wrappers, ICON0.PNG, ...
    ppu/
      bin/
        powerpc64-ps3-elf-gcc.exe
        powerpc64-ps3-elf-as.exe
        ...
      lib/                # ← copied verbatim from stage/ps3dev/ppu/lib
      include/            # ← copied verbatim
      powerpc64-ps3-elf/  # ← copied verbatim (target sysroot)
    spu/
      bin/spu-elf-gcc.exe ...
      lib/                # ← copied from stage/ps3dev/spu/lib
      ...
    psl1ght/              # ← copied from stage/ps3dev/psl1ght (host-agnostic)
    ps3dk/                # ← copied from stage/ps3dev/ps3dk
    portlibs/ppu/         # ← copied from stage/ps3dev/portlibs/ppu
```

Why a parallel tree: stage 1 *configure* runs against the patched source
trees and writes build artifacts under a separate
`$PS3_BUILD_ROOT/ppu-windows/` and `$PS3_BUILD_ROOT/spu-windows/`. The
install prefix (`$PS3DEV.win/` or similar — see open question) just
holds the copy of the install tree we ship.

## Build-script changes

Both `build-ppu-toolchain.sh` and `build-spu-toolchain.sh` gain a host
selector. Default behavior is unchanged (native build). New mode
triggered by `PS3_HOST_TRIPLE=x86_64-w64-mingw32` in the environment, or
`--host=x86_64-w64-mingw32` on the command line.

When set:

- `BUILD` and `PREFIX` switch:
  - `BUILD=$PS3_BUILD_ROOT/ppu-windows`
  - `PREFIX=$PS3DEV.win/ppu` (or `$PS3DEV-windows/ppu` — see open question)
- Configure adds `--build=$(./config.guess) --host=x86_64-w64-mingw32`.
  The mingw-w64 toolchain on PATH provides `x86_64-w64-mingw32-gcc`
  etc., which configure picks up automatically when `--host` is set.
- For binutils + GDB: those are self-contained — no changes beyond
  `--host` and `--prefix`.
- For GCC: the combined-tree build with newlib **must skip target-lib
  compilation in stage 1**. We do this by configuring without
  `--with-newlib`, building only the host-side compiler bits
  (`make all-gcc all-binutils all-ld`), then installing only those.
  Target libs (libgcc, libstdc++, libsupc++) come from stage 0.
  Alternative: pass `MAKE_TARGET_DIRS=` or run a focused `make` target
  list. Whichever is simpler — see open question.
- After install, copy stage-0's target-library tree (`$PS3DEV/ppu/lib`,
  `$PS3DEV/ppu/include`, `$PS3DEV/ppu/powerpc64-ps3-elf/`) into
  the Windows prefix. Driver-relative path lookup (`-print-libgcc-file-name`
  etc.) works on Windows because GCC's driver computes paths relative
  to its own location, not absolute.

`create_symlinks` becomes a no-op or skipped in Windows mode (NTFS
symlinks need elevated perms; we ship `.cmd` shim files or just rely on
the `powerpc64-ps3-elf-` long names — the existing `ppu-gcc` short alias
is a Linux convenience and not strictly needed on Windows).

## Packaging — `scripts/package-windows-release.sh`

New script. Takes `$PS3DEV.win/` as input, produces
`ps3-sdk-vX.Y.Z-windows-x86_64.zip`.

```
ps3-sdk-vX.Y.Z-windows-x86_64/
  bin/                    # ← from stage 1 + host tools
    powerpc64-ps3-elf-*.exe
    spu-elf-*.exe
    nidgen.exe abi-verify.exe coverage-report.exe
    sprxlinker.exe rsx-cg-compiler.exe
    ICON0.PNG
  ppu/                    # ← from stage 1 + stage 0 target libs
  spu/                    # ← from stage 1 + stage 0 target libs
  psl1ght/                # ← from stage 0 (host-agnostic)
  ps3dk/                  # ← from stage 0
  portlibs/ppu/           # ← from stage 0
  setup.cmd               # ← sets %PS3DEV% to the extract root and prepends bin/
  README.txt
  CHANGELOG.md
ps3-sdk-vX.Y.Z-windows-x86_64.zip.sha256
```

`setup.cmd` is a one-liner that the user runs in their dev shell to
export `PS3DEV` and PATH for the current cmd / pwsh session.

The host tools (Rust CLIs, sprxlinker, rsx-cg-compiler) are not built
inside the Linux WSL flow — they're built natively on Windows by
`release.yml`'s existing `build-host-tools-windows` job. For local
verification we either fetch the latest CI artifact or invoke MSYS2
directly. **Initial implementation:** the package script accepts an
existing `ps3-sdk-tools-*-windows-x86_64.zip` (the existing CI artifact)
as an input flag and merges its `bin/` into the larger zip. CI
integration later wires the host-tools job's output as a dependency.

## CI integration (deferred to a follow-up)

`.github/workflows/release.yml` grows a new `build-toolchain-windows`
job that runs on `ubuntu-latest`, executes the WSL-validated flow
(`apt install …`, run the build scripts twice, run the package
script), and uploads `ps3-sdk-vX.Y.Z-windows-x86_64.zip` as an artifact.
The `publish` job's asset list grows to include it.

This is captured as task #9 and intentionally not in the initial
implementation — we want local verification first.

## Verification (task #8)

On Windows, in a fresh `cmd` / `pwsh`:

1. Extract `ps3-sdk-vX.Y.Z-windows-x86_64.zip` somewhere (e.g.
   `C:\ps3sdk\`).
2. `cd C:\ps3sdk\ps3-sdk-vX.Y.Z-windows-x86_64 && setup.cmd`.
3. `powerpc64-ps3-elf-gcc.exe --version` → reports 12.4.0.
4. `spu-elf-gcc.exe --version` → reports 9.5.0.
5. `cd samples\toolchain\hello-ppu-c++17 && make` (under MSYS2 or git-bash;
   the `Makefile`s are bash-makefile not nmake-style — see open question
   on whether to ship a Windows-native build path or document MSYS2 as a
   prerequisite).
6. Run the resulting `.self` in RPCS3 — expect `result: PASS`.

## Open questions

1. **Prefix naming.** `$PS3DEV.win/` (filesystem-friendly, conflicts with
   nothing) vs `$PS3DEV-windows/` (more explicit) vs reusing
   `$PS3DEV` with a `--host`-driven swap. Recommendation: `$PS3DEV.win/`
   — the `.win` suffix is unique enough that env.sh's PATH munging
   doesn't accidentally pick it up.

2. **Target-lib reuse mechanism.** Two viable approaches:
   - (a) Configure stage 1 without `--with-newlib` and skip target libs
         entirely; copy stage 0 libs into the install tree post-install.
   - (b) Configure stage 1 normally but stop the build with
         `make all-gcc all-binutils all-ld`, then `make install-gcc
         install-binutils install-ld`.
   Both work. (a) is cleaner because `--with-newlib` actually drives
   the build to compile newlib, which fails when the target compiler
   isn't on PATH at the build site. Recommendation: (a).

3. **Sample build on Windows.** Existing sample Makefiles use bash
   patterns (`shell`, `sed`, `find`). On Windows the user needs MSYS2 or
   Git Bash to drive `make`. Two options:
   - Ship MSYS2 in the zip (huge — out).
   - Document MSYS2 / Git Bash as a prerequisite for sample builds.
   Recommendation: document the prerequisite. The toolchain itself
   (gcc.exe etc.) does not require MSYS2 — only the `make`-driven
   sample workflow does.

4. **Source-tree location for WSL builds.** /mnt/c is unusably slow for
   GCC bootstraps. Recommendation: clone the repo into ext4 at
   `~/PS3_Custom_Toolchain` (with the Windows-side path added as a
   `windows` git remote for easy pulls). Builds run entirely in ext4;
   the Windows tree stays the editing target. The packaged release zip
   gets copied back to `/mnt/c/` for testing.

5. **mingw-w64 thread model.** Ubuntu's `x86_64-w64-mingw32-gcc` ships
   `GCC 13-win32` (Win32 threads). We never link a thread-using
   binary in the host toolchain (binutils/GCC/GDB are largely
   single-threaded for our use), so this shouldn't matter, but flag if
   GDB's Python / threading needs a `posix` model later.

## Risks

- **Stage 1 GCC libstdc++.** GCC's host toolchain links itself against
  libstdc++. Mingw-w64's libstdc++ comes with the cross compiler.
  Linking statically (`-static-libstdc++ -static-libgcc`) keeps the
  resulting `.exe` self-contained. Confirmed approach in
  `release.yml`'s rsx-cg-compiler step — same flags work here.
- **Path separators in driver specs.** GCC's driver hard-codes
  forward-slash paths in some specs; on Windows it generally normalises
  but historic bugs exist. Mitigation: rely on the well-trodden
  `--host=x86_64-w64-mingw32` build path that devkitPro / similar
  projects use; if problems surface, patch the spec at install time.
- **GDB Python integration.** `--with-python=yes` on the Linux build
  pulls in libpython. For Windows we either drop Python support
  (`--without-python`) or bundle a Windows Python — recommendation:
  drop for the initial Windows ship, re-add later.

## Plan handoff

After approval, this spec hands off to `superpowers:writing-plans`,
which produces a step-by-step implementation plan whose tasks map to
tasks #2 → #9 in the current task list.
