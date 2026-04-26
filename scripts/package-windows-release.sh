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
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

say "Staging into $STAGE_DIR"

# 1. Toolchain (PPU + SPU).  Copy verbatim from the cross-built prefix.
say "Copying PPU toolchain from $WIN_PREFIX/ppu"
cp -a "$WIN_PREFIX/ppu" "$STAGE_DIR/ppu"
say "Copying SPU toolchain from $WIN_PREFIX/spu"
cp -a "$WIN_PREFIX/spu" "$STAGE_DIR/spu"

# 2. Host-agnostic runtime + SDK + portlibs from the native install.
for sub in psl1ght ps3dk portlibs; do
    if [[ -d "$PS3DEV/$sub" ]]; then
        say "Copying $sub from $PS3DEV/$sub"
        cp -a "$PS3DEV/$sub" "$STAGE_DIR/$sub"
    else
        warn "Native install has no $sub/ at $PS3DEV/$sub — skipping"
    fi
done

# 3. Umbrella bin/ — ICON0.PNG + Windows host tools.
mkdir -p "$STAGE_DIR/bin"
if [[ -f "$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG" ]]; then
    cp "$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG" "$STAGE_DIR/bin/ICON0.PNG"
fi

# 4. Host tools zip (from CI's build-host-tools-windows job).
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
else
    warn "No --tools-zip provided.  The release zip will ship without rsx-cg-compiler.exe / sprxlinker.exe / nidgen.exe / etc.  Pass --tools-zip=PATH to include them, or run release.yml's build-host-tools-windows job to produce one."
fi

# 5. setup.cmd: a one-shot environment activator for Windows users.
cat > "$STAGE_DIR/setup.cmd" <<'EOF'
@echo off
REM PS3 Custom Toolchain — Windows environment activator.
REM
REM First run: prints instructions if PS3DEV and PS3DK aren't set yet.
REM Once both are set (typically via "setx" at user scope), subsequent
REM runs prepend the toolchain bin dirs to %PATH% for the current shell.
setlocal enabledelayedexpansion

set "_missing="
if not defined PS3DEV set "_missing=!_missing! PS3DEV"
if not defined PS3DK  set "_missing=!_missing! PS3DK"

if defined _missing (
    echo.
    echo PS3 Custom Toolchain — environment not yet configured.
    echo The following variables are not set:!_missing!
    echo.
    echo Set them permanently for your user account.  Open a regular cmd
    echo window ^(NOT this one^) and run:
    echo.
    echo     setx PS3DEV "%~dp0."
    echo     setx PS3DK  "%%PS3DEV%%\ps3dk"
    echo.
    echo PS3DEV should point at the extracted release root ^(this directory^),
    echo and PS3DK at the SDK install root inside it ^(used by the PPU GCC
    echo driver to locate libsysbase, libc, librt, liblv2 at link time^).
    echo.
    echo "setx" writes to the registry but does NOT update the current
    echo shell.  Open a NEW terminal afterwards and re-run setup.cmd to
    echo prepend the toolchain bin dirs to PATH for that session.
    echo.
    exit /b 1
)

endlocal & set "PATH=%PS3DEV%\bin;%PS3DEV%\ppu\bin;%PS3DEV%\spu\bin;%PATH%"
echo PS3DEV: %PS3DEV%
echo PS3DK:  %PS3DK%
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

Layout:
  bin\\          Host tools (rsx-cg-compiler.exe, sprxlinker.exe, ...) + ICON0.PNG
  ppu\\bin\\     powerpc64-ps3-elf-{gcc,g++,as,ld,gdb,...}.exe
  ppu\\         libgcc, newlib libc, libstdc++ (PowerPC ELF target libs)
  spu\\bin\\     spu-elf-{gcc,g++,as,ld,...}.exe
  spu\\         libgcc, newlib libc, libstdc++ (SPU ELF target libs)
  psl1ght\\     PSL1GHT runtime + headers
  ps3dk\\       Our SDK headers + libraries (libgcm_cmd.a, libdbgfont.a, ...)
  portlibs\\    zlib, libpng, SDL2, libcurl, mbedTLS, ...
  setup.cmd     Environment activator (prepends toolchain bins to PATH)
  CHANGELOG.md  Release-by-release history

Quick start:
  1. Extract this archive somewhere (e.g. C:\\ps3sdk\\${STAGE_NAME}\\).
  2. Set the toolchain environment variables permanently for your user
     account.  In a regular cmd window run:
         setx PS3DEV "C:\\ps3sdk\\${STAGE_NAME}"
         setx PS3DK  "%PS3DEV%\\ps3dk"
     PS3DEV points at the extract root; PS3DK is the SDK install root
     inside it (used by the PPU GCC driver to find libsysbase, libc,
     librt, liblv2 at link time).
  3. Open a NEW terminal so the setx values take effect, then:
         cd C:\\ps3sdk\\${STAGE_NAME}
         setup.cmd
     This prepends bin\\, ppu\\bin\\ and spu\\bin\\ to PATH for the
     current shell session.
  4. powerpc64-ps3-elf-gcc.exe --version    (-> 12.4.0)
     spu-elf-gcc.exe            --version    (-> 9.5.0)

Sample builds use bash-style Makefiles; install MSYS2 or Git Bash if you
want to run "make" against the samples/ directory.  The toolchain itself
(gcc.exe etc.) does not require MSYS2.

See https://github.com/FirebirdTA01/PS3_Custom_Toolchain for docs.
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
