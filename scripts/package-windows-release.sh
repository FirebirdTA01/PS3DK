#!/usr/bin/env bash
# PS3 Custom Toolchain — Windows release packager
#
# Combines the cross-built Windows-hosted toolchain ($PS3DEV.win/) with the
# host-agnostic SDK runtime artifacts from $PS3DEV/ (PSL1GHT, portlibs,
# ps3dk) and pre-built Windows host tools (Rust CLIs + rsx-cg-compiler +
# sprx-linker) into a single zip:
#
#   ps3-sdk-vX.Y.Z-windows-x86_64.zip
#
# End users extract the zip, run setup.cmd to set %PS3DEV% and prepend
# bin/ on PATH, and the toolchain is usable on Windows.
#
# Prerequisites:
#   1. Stage 0 (native Linux) PPU+SPU+PSL1GHT+portlibs+SDK built into
#      $PS3DEV.
#   2. Stage 1 (cross to mingw32) PPU+SPU built into $PS3DEV.win.  Drive
#      this via:
#        ./scripts/build-ppu-toolchain.sh --host=x86_64-w64-mingw32
#        ./scripts/build-spu-toolchain.sh --host=x86_64-w64-mingw32
#   3. Windows host tools zip (ps3-sdk-tools-vX.Y.Z-windows-x86_64.zip)
#      produced by .github/workflows/release.yml's build-host-tools-windows
#      job.  Pass via --tools-zip=PATH or place at the default location
#      (see TOOLS_ZIP below).
#
# Output: $OUTPUT_DIR/ps3-sdk-vX.Y.Z-windows-x86_64.zip + .sha256.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say()  { printf "[pkg-win] %s\n" "$*"; }
warn() { printf "[pkg-win] WARNING: %s\n" "$*" >&2; }
die()  { printf "[pkg-win] ERROR: %s\n" "$*" >&2; exit 1; }

# Defaults.
HOST_TRIPLE="x86_64-w64-mingw32"
WIN_PREFIX="${PS3DEV}.win"
OUTPUT_DIR="$PS3_TOOLCHAIN_ROOT/stage/release"
TOOLS_ZIP=""

for arg in "$@"; do
    case "$arg" in
        --host=*)        HOST_TRIPLE="${arg#--host=}" ;;
        --win-prefix=*)  WIN_PREFIX="${arg#--win-prefix=}" ;;
        --output-dir=*)  OUTPUT_DIR="${arg#--output-dir=}" ;;
        --tools-zip=*)   TOOLS_ZIP="${arg#--tools-zip=}" ;;
        --help|-h)
            cat <<EOF
Usage: $0 [options]

Options:
  --host=TRIPLE        Target host triple (default: x86_64-w64-mingw32)
  --win-prefix=PATH    Path to cross-built toolchain prefix (default: \$PS3DEV.win)
  --output-dir=PATH    Where to write the release zip (default: stage/release)
  --tools-zip=PATH     Path to ps3-sdk-tools-*-windows-x86_64.zip from CI
                       (the Windows host tools archive built by release.yml)
  -h, --help           Show this help.
EOF
            exit 0
            ;;
        *) warn "Unknown argument: $arg" ;;
    esac
done

# Sanity checks.
[[ -d "$PS3DEV" ]]      || die "Native install missing: $PS3DEV (run stage 0 first)"
[[ -d "$WIN_PREFIX" ]]  || die "Cross-built prefix missing: $WIN_PREFIX (run stage 1 first)"
[[ -x "$WIN_PREFIX/ppu/bin/powerpc64-ps3-elf-gcc.exe" ]] \
    || die "$WIN_PREFIX/ppu/bin/powerpc64-ps3-elf-gcc.exe not found — run build-ppu-toolchain.sh --host=$HOST_TRIPLE"
[[ -x "$WIN_PREFIX/spu/bin/spu-elf-gcc.exe" ]] \
    || die "$WIN_PREFIX/spu/bin/spu-elf-gcc.exe not found — run build-spu-toolchain.sh --host=$HOST_TRIPLE"

# Version: from version.sh (matches the rest of the release pipeline).
VERSION="$("$script_dir/version.sh" --format=plain)"
[[ -n "$VERSION" ]] || die "scripts/version.sh produced no version string"
say "Version: $VERSION"

# Staging directory is wiped each run for reproducibility.
STAGE_NAME="ps3-sdk-${VERSION}-windows-x86_64"
STAGE_DIR="$OUTPUT_DIR/$STAGE_NAME"
ZIP_PATH="$OUTPUT_DIR/${STAGE_NAME}.zip"
mkdir -p "$OUTPUT_DIR"

# Defensive guard: refuse to wipe a path that doesn't match our expected
# staging-name pattern.  Catches accidental misuse like
# --output-dir=/ or --output-dir=$HOME.
case "$STAGE_DIR" in
    */ps3-sdk-*-windows-x86_64) ;;
    *) die "Refusing to rm -rf '$STAGE_DIR' — does not match the expected ps3-sdk-*-windows-x86_64 pattern" ;;
esac
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

say "Staging into $STAGE_DIR"

# Layout philosophy: the resulting zip extracts to a single directory the
# user sets %PS3DK% to.  PS3DK is the umbrella — toolchain binaries live
# under PS3DK/ppu/bin and PS3DK/spu/bin, runtime libraries (libsysbase,
# librt, liblv2, ...) live under PS3DK/ppu/lib, and the PSL1GHT-style
# Makefile rule fragments (ppu_rules, spu_rules, base_rules, data_rules)
# sit at PS3DK/.  The PPU GCC driver reads %PS3DK% via getenv() in
# LIB_LV2_SPEC and -L resolves directly against PS3DK/ppu/lib.

# 1. Cross-built toolchain (PPU + SPU host binaries + per-arch lib trees).
say "Copying PPU toolchain from $WIN_PREFIX/ppu"
cp -a "$WIN_PREFIX/ppu" "$STAGE_DIR/ppu"
say "Copying SPU toolchain from $WIN_PREFIX/spu"
cp -a "$WIN_PREFIX/spu" "$STAGE_DIR/spu"

# 1b. Refresh target sysroots from the native install.  The cross-build's
# target-lib snapshot was taken at cross-build time; PSL1GHT / SDK installs
# that landed afterwards (lv2-crt*.o, lv2.ld, additional .a's) live only
# in the native tree.  Overlay them onto the Windows package so the
# resulting toolchain sees the latest target-side artefacts.  Target libs
# are PowerPC / SPU ELF — host-agnostic.
for arch_pair in "ppu:powerpc64-ps3-elf" "spu:spu-elf"; do
    arch="${arch_pair%%:*}"
    target="${arch_pair##*:}"
    if [[ -d "$PS3DEV/$arch/$target" ]]; then
        say "Refreshing $arch/$target sysroot from native install"
        cp -a "$PS3DEV/$arch/$target/." "$STAGE_DIR/$arch/$target/"
    fi
done

# 2. Merge PSL1GHT runtime + SDK from $PS3DK into the install tree at the
# root level (no nested ps3dk/ subdir).  This puts:
#   $PS3DK/ppu/lib/*    -> $STAGE_DIR/ppu/lib/*    (alongside libgcc.a)
#   $PS3DK/spu/lib/*    -> $STAGE_DIR/spu/lib/*
#   $PS3DK/ppu/include  -> $STAGE_DIR/ppu/include
#   $PS3DK/spu/include  -> $STAGE_DIR/spu/include
#   $PS3DK/{ppu,spu,base,data}_rules -> $STAGE_DIR/{ppu,spu,base,data}_rules
if [[ -d "$PS3DK" && -n "$(ls -A "$PS3DK" 2>/dev/null)" ]]; then
    say "Merging PSL1GHT runtime + SDK from $PS3DK"
    cp -a "$PS3DK/." "$STAGE_DIR/"
    # PSL1GHT (and our SDK) install host tools at $PS3DK/bin/ as native
    # Linux ELFs (sprxlinker, sometimes others).  Those have no business
    # in a Windows release zip — Windows host-tool .exe's land here via
    # --tools-zip below.  Strip any ELF that snuck in from the merge.
    if [[ -d "$STAGE_DIR/bin" ]]; then
        while IFS= read -r f; do
            [[ -n "$f" ]] || continue
            if file -b "$f" 2>/dev/null | grep -q '^ELF '; then
                say "Removing leaked Linux binary from bin/: ${f#"$STAGE_DIR/"}"
                rm -f "$f"
            fi
        done < <(find "$STAGE_DIR/bin" -maxdepth 1 -type f)
    fi
else
    warn "$PS3DK is empty / unbuilt — release will ship without runtime libs (libsysbase, librt, liblv2). Run build-psl1ght.sh + build-portlibs.sh + build-sdk.sh on a native install first."
fi

# 3. portlibs (zlib, libpng, SDL2, libcurl, mbedTLS, ...).
if [[ -d "$PS3DEV/portlibs" && -n "$(ls -A "$PS3DEV/portlibs" 2>/dev/null)" ]]; then
    say "Copying portlibs from $PS3DEV/portlibs"
    cp -a "$PS3DEV/portlibs" "$STAGE_DIR/portlibs"
fi

# 4. Sample sources.  Useful enough to ship alongside the toolchain so
# users have something to build straight after install.
if [[ -d "$PS3_TOOLCHAIN_ROOT/samples" ]]; then
    say "Copying samples from $PS3_TOOLCHAIN_ROOT/samples"
    cp -a "$PS3_TOOLCHAIN_ROOT/samples" "$STAGE_DIR/samples"
fi

# 5. Umbrella bin/ — ICON0.PNG + Windows host tools.
mkdir -p "$STAGE_DIR/bin"
if [[ -f "$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG" ]]; then
    cp "$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG" "$STAGE_DIR/bin/ICON0.PNG"
    # Also stage at sdk/assets/ICON0.PNG so cmake/ps3-self.cmake's
    # ps3_add_pkg() default ICON path (${_PS3_TOOLCHAIN_ROOT}/sdk/assets/ICON0.PNG,
    # which resolves to cmake/.. = $STAGE_DIR) finds it.
    mkdir -p "$STAGE_DIR/sdk/assets"
    cp "$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG" "$STAGE_DIR/sdk/assets/ICON0.PNG"
fi

# 5b. CMake toolchain files + helper modules.  Samples build with
#     -DCMAKE_TOOLCHAIN_FILE="%PS3DK%\cmake\ps3-ppu-toolchain.cmake"
#     and the helpers (ps3-self.cmake, ps3-bin2s-impl.cmake) plus
#     templates/sfo.xml are pulled in via include(ps3-self) and
#     ps3_add_pkg().  Without this directory the README's quickstart
#     fails immediately with "could not load cache file ps3-ppu-toolchain.cmake".
if [[ -d "$PS3_TOOLCHAIN_ROOT/cmake" ]]; then
    say "Copying cmake/ toolchain files + helpers"
    cp -a "$PS3_TOOLCHAIN_ROOT/cmake" "$STAGE_DIR/cmake"
else
    warn "$PS3_TOOLCHAIN_ROOT/cmake missing — release will ship without CMake toolchain files; samples won't build out of the box."
fi

# 4. Host tools zip (from CI's build-host-tools-windows job) OR locally
#    cross-built tools staged at $STAGE_HOST_TOOLS_BIN by
#    scripts/build-host-tools-windows.sh.  CI takes the zip path; local
#    runs typically use the staged dir.
STAGE_HOST_TOOLS_BIN="$PS3_TOOLCHAIN_ROOT/stage/host-tools-windows/bin"
if [[ -n "$TOOLS_ZIP" ]]; then
    [[ -f "$TOOLS_ZIP" ]] || die "Tools zip not found: $TOOLS_ZIP"
    say "Merging Windows host tools from $TOOLS_ZIP"
    local_tmp="$(mktemp -d)"
    trap '[[ -n "${local_tmp:-}" ]] && rm -rf "$local_tmp"' EXIT
    unzip -q "$TOOLS_ZIP" -d "$local_tmp"
    # The CI tools zip stages a top-level "ps3-sdk-tools-VERSION-windows-x86_64/bin/"
    # — find any nested bin/ directory and merge its contents.
    local_bin="$(find "$local_tmp" -type d -name bin | head -1)"
    if [[ -n "$local_bin" ]]; then
        cp -a "$local_bin/." "$STAGE_DIR/bin/"
    else
        warn "Tools zip $TOOLS_ZIP has no bin/ subdir — skipping merge"
    fi
elif [[ -d "$STAGE_HOST_TOOLS_BIN" && -n "$(ls -A "$STAGE_HOST_TOOLS_BIN" 2>/dev/null)" ]]; then
    say "Merging Windows host tools from $STAGE_HOST_TOOLS_BIN (local cross-build)"
    cp -a "$STAGE_HOST_TOOLS_BIN/." "$STAGE_DIR/bin/"
else
    warn "No --tools-zip provided and $STAGE_HOST_TOOLS_BIN is empty.  The release zip will ship without rsx-cg-compiler.exe / make_self.exe / nidgen.exe / etc.  Either run scripts/build-host-tools-windows.sh first (to populate the staged dir), or pass --tools-zip=PATH pointing at release.yml's build-host-tools-windows artifact."
fi

validate_windows_release_payload() {
    say "Validating Windows release payload"

    local missing=0
    local required_bins=(
        abi-verify.exe
        bin2s.exe
        cgcomp.exe
        coverage-report.exe
        fself.exe
        make_self.exe
        make_self_npdrm.exe
        nidgen.exe
        package_finalize.exe
        rsx-cg-compiler.exe
        sprxlinker.exe
    )
    local required_ppu_libs=(
        libaudio_stub.a
        libc_stub.a
        libfs_stub.a
        libgcm_sys.a
        libio.a
        libjpgdec_stub.a
        libl10n_stub.a
        liblv2_stub.a
        libpngdec_stub.a
        librtc_stub.a
        libspurs_jq_stub.a
        libspurs_stub.a
        libsync2_stub.a
        libsync_stub.a
        libsysmodule_stub.a
        libsysutil_ap_stub.a
        libsysutil_audio_out_stub.a
        libsysutil_imejp_stub.a
        libsysutil_music_decode_stub.a
        libsysutil_music_export_stub.a
        libsysutil_music_stub.a
        libsysutil_savedata_extra_stub.a
        libsysutil_savedata_stub.a
        libsysutil_screenshot_stub.a
        libsysutil_stub.a
        libsysutil_subdisplay_stub.a
    )

    for exe in "${required_bins[@]}"; do
        if [[ ! -f "$STAGE_DIR/bin/$exe" ]]; then
            warn "Missing required Windows host tool: bin/$exe"
            missing=1
        fi
    done
    if [[ ! -f "$STAGE_DIR/bin/cg.dll" ]]; then
        warn "Optional legacy Cg runtime not staged: bin/cg.dll. cgcomp.exe may not run, but rsx-cg-compiler.exe is packaged and is the supported shader compiler."
    fi

    for lib in "${required_ppu_libs[@]}"; do
        if [[ ! -f "$STAGE_DIR/ppu/lib/$lib" ]]; then
            warn "Missing required PPU stub/archive: ppu/lib/$lib"
            missing=1
        fi
    done

    if command -v file >/dev/null 2>&1; then
        for exe in "${required_bins[@]}"; do
            [[ -f "$STAGE_DIR/bin/$exe" ]] || continue
            if file -b "$STAGE_DIR/bin/$exe" | grep -q '^ELF '; then
                warn "Windows release contains Linux ELF in bin/$exe"
                missing=1
            fi
        done
    fi

    [[ "$missing" -eq 0 ]] || die "Windows release payload is incomplete. Run scripts/build-cell-stub-archives.sh and provide the complete Windows host-tools artifact via --tools-zip, or run scripts/build-host-tools-windows.sh before packaging."
}

validate_windows_release_payload

# 5. setup.cmd: a one-shot environment activator for Windows users.
cat > "$STAGE_DIR/setup.cmd" <<'EOF'
@echo off
REM PS3 Custom Toolchain — Windows environment activator.
REM
REM First run: prints instructions if PS3DK isn't set yet.
REM Once PS3DK is set (typically via "setx" at user scope), subsequent
REM runs prepend the toolchain bin dirs to %PATH% for the current shell
REM and export PS3DEV / PSL1GHT as in-session aliases for code that
REM still reads those names.
setlocal enabledelayedexpansion

if not defined PS3DK (
    echo.
    echo PS3 Custom Toolchain — environment not yet configured.
    echo PS3DK is not set.
    echo.
    echo Set it permanently for your user account.  Open a regular cmd
    echo window ^(NOT this one^) and run:
    echo.
    echo     setx PS3DK "%~dp0."
    echo.
    echo PS3DK should point at the extracted release root ^(this directory^).
    echo The PPU GCC driver reads it via getenv^(^) in its link spec, and
    echo -L resolves against %%PS3DK%%\ppu\lib at link time.
    echo.
    echo "setx" writes to the registry but does NOT update the current
    echo shell.  Open a NEW terminal afterwards and re-run setup.cmd to
    echo prepend the toolchain bin dirs to PATH for that session.
    echo.
    exit /b 1
)

REM PS3DEV and PSL1GHT are back-compat aliases — code that still reads
REM them sees the same install root.  We export at the parent shell
REM scope only for this session; permanent setx is optional.  Windows
REM tolerates double-separator paths (...\\ppu\\bin) so we don't bother
REM trimming a trailing backslash off PS3DK.
endlocal & set "PATH=%PS3DK%\bin;%PS3DK%\ppu\bin;%PS3DK%\spu\bin;%PATH%" & set "PS3DEV=%PS3DK%" & set "PSL1GHT=%PS3DK%"
echo PS3DK:   %PS3DK%
echo PS3DEV:  %PS3DEV%   ^(in-session alias^)
echo PSL1GHT: %PSL1GHT%   ^(in-session alias^)
echo Toolchain bin dirs prepended to PATH for this session.
echo.
where powerpc64-ps3-elf-gcc.exe 1>nul 2>nul && powerpc64-ps3-elf-gcc.exe --version
where spu-elf-gcc.exe            1>nul 2>nul && spu-elf-gcc.exe            --version
EOF
# cmd.exe parses .cmd/.bat files line by line and is line-ending-sensitive
# — Unix LF endings cause "REM" to be read as a partial token and
# enabledelayedexpansion doesn't kick in.  Convert to CRLF.
sed -i 's/$/\r/' "$STAGE_DIR/setup.cmd"

# 6. README + bundled changelog.
cat > "$STAGE_DIR/README.txt" <<EOF
PS3 Custom Toolchain — ${VERSION}, windows-x86_64

This archive bundles a ready-to-use PS3 toolchain for Windows hosts,
cross-built from Linux to ${HOST_TRIPLE}.

Layout (everything below is anchored at %PS3DK% once installed):
  bin\\               Host tools (rsx-cg-compiler.exe, sprxlinker.exe,
                      nidgen.exe, ...) + ICON0.PNG
  ppu\\bin\\          powerpc64-ps3-elf-{gcc,g++,as,ld,...}.exe
  ppu\\lib\\          libsysbase, libc, librt, liblv2, libgcm, libio,
                      libsysutil*, libfont, ... (PSL1GHT + SDK runtime)
                      + libgcc.a (toolchain) under lib\\gcc\\powerpc64-ps3-elf\\
  ppu\\powerpc64-ps3-elf\\
                      Target sysroot — newlib libc/libm, libstdc++,
                      lv2-crt0.o, lv2.ld linker script
  ppu\\include\\      PSL1GHT + SDK PPU headers (cell/*.h, sys/*.h, etc.)
  spu\\               Same shape as ppu\\, for spu-elf
  cmake\\             CMake toolchain files + helpers
                      (ps3-ppu-toolchain.cmake, ps3-spu-toolchain.cmake,
                      ps3-self.cmake, ps3-bin2s-impl.cmake, templates\\)
  ppu_rules           PSL1GHT-style Makefile fragments (legacy build path)
  spu_rules
  base_rules
  data_rules
  portlibs\\          zlib, libpng, SDL2, libcurl, mbedTLS, ...
  samples\\           Source for hello-ppu, hello-spu, cellGcm,
                      Spurs, sysutil, etc.  Build with CMake — see below.
  setup.cmd           Environment activator (prepends toolchain bins to PATH)
  README.txt
  CHANGELOG.md

Quick start:
  1. Extract this archive somewhere stable.  The path you pick is the
     value of %PS3DK% — pick whatever you like (e.g. C:\\PS3DK\\,
     D:\\dev\\ps3dk\\, %USERPROFILE%\\ps3dk\\, ...).
  2. Set PS3DK permanently for your user account.  In a regular cmd
     window run (substituting the path you extracted to):
         setx PS3DK "C:\\path\\to\\extracted\\PS3DK"
     The PPU GCC driver reads this via getenv() at link time and
     resolves -L against %PS3DK%\\ppu\\lib.
  3. Open a NEW terminal so the setx value takes effect, then:
         cd "%PS3DK%"
         setup.cmd
     This prepends bin\\, ppu\\bin\\ and spu\\bin\\ to PATH and exports
     PS3DEV / PSL1GHT as in-session aliases (some legacy code paths
     still read those names).
  4. powerpc64-ps3-elf-gcc.exe --version    (-> 12.4.0)
     spu-elf-gcc.exe            --version    (-> 9.5.0)

Building the samples (CMake — recommended):
  Samples build with CMake.  You need cmake (3.20+) and a generator on
  PATH.  Common picks: Ninja, "MinGW Makefiles", or "Unix Makefiles"
  (under MSYS2 / Git Bash).  None of these ship in this archive; install
  via "choco install cmake ninja", the Visual Studio installer's
  optional CMake/Ninja components, MSYS2 (https://www.msys2.org/), or
  any equivalent.

  Each sample is a standalone CMake project.  From a cmd shell after
  running %PS3DK%\\setup.cmd (replace -G with whichever generator you
  installed):

      cd "%PS3DK%\\samples\\toolchain\\hello-ppu-c++17"
      cmake -S . -B cmake-build ^
            -DCMAKE_TOOLCHAIN_FILE="%PS3DK%\\cmake\\ps3-ppu-toolchain.cmake" ^
            -G Ninja
      cmake --build cmake-build

  Each sample produces three artefacts at the sample directory next to
  CMakeLists.txt: <name>.elf (raw PPU ELF), <name>.self (CEX-signed for
  RPCS3 + signed hardware), and <name>.fake.self (fake-signed for CFW
  hardware).  Run on RPCS3 with:
      rpcs3 --no-gui <name>.self

  For standalone SPU work, swap in cmake\\ps3-spu-toolchain.cmake.

Direct toolchain invocation (no CMake):
  The toolchain itself (gcc.exe / g++.exe / ld.exe etc.) does NOT
  require any extra tools.  Standalone compile + link from cmd or
  PowerShell works fine:
      powerpc64-ps3-elf-gcc.exe -O2 hello.c -o hello.elf

  The full strip + sprxlinker + make_self / fself signing chain is
  documented in the project README's "Manual / direct toolchain
  invocation (no CMake)" section.

See https://github.com/FirebirdTA01/PS3_Custom_Toolchain for full docs.
EOF

if [[ -f "$PS3_TOOLCHAIN_ROOT/CHANGELOG.md" ]]; then
    cp "$PS3_TOOLCHAIN_ROOT/CHANGELOG.md" "$STAGE_DIR/CHANGELOG.md"
fi

# 7. Zip + sha256.
say "Creating $ZIP_PATH"
rm -f "$ZIP_PATH" "$ZIP_PATH.sha256"
(cd "$OUTPUT_DIR" && zip -r -q -9 "$STAGE_NAME.zip" "$STAGE_NAME")
(cd "$OUTPUT_DIR" && sha256sum "$STAGE_NAME.zip" > "$STAGE_NAME.zip.sha256")

say "=== Done ==="
ls -lh "$ZIP_PATH" "$ZIP_PATH.sha256"
say "Stage tree: $STAGE_DIR"
say "Archive:    $ZIP_PATH"
