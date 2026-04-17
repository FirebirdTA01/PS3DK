#!/usr/bin/env bash
# PS3 Custom Toolchain — Phase 1: PPU toolchain (powerpc64-ps3-elf)
#
# Builds:
#   binutils 2.42  -> $PS3DEV/ppu
#   GCC 12.4.0     -> $PS3DEV/ppu (stage 1 + newlib + stage 2)
#   newlib 4.4.0   -> as part of combined GCC build
#   GDB 14.2       -> $PS3DEV/ppu
#
# Source extraction is via `git archive` from the mirrors cloned by bootstrap.sh.
# Patches are applied from patches/ppu/<component>/ in the order listed in
# patches/ppu/<component>/series (or, if no series file exists, sorted by name).
#
# Re-runnable: delete $PS3_BUILD_ROOT/ppu to force a clean rebuild, or run with
# --clean to do it for you.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[ppu] %s\n" "$*"; }
warn() { printf "[ppu] WARNING: %s\n" "$*" >&2; }
die() { printf "[ppu] ERROR: %s\n" "$*" >&2; exit 1; }

# Component versions — bump these if we re-target.
BINUTILS_TAG="binutils-2_42"
BINUTILS_VER="2.42"
GCC_TAG="releases/gcc-12.4.0"
GCC_VER="12.4.0"
NEWLIB_TAG="newlib-4.4.0"
NEWLIB_VER="4.4.0"
GDB_TAG="gdb-14.2-release"
GDB_VER="14.2"

TARGET="powerpc64-ps3-elf"
PREFIX="$PS3DEV/ppu"

UPSTREAM="$PS3_TOOLCHAIN_ROOT/src/upstream"
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/ppu"
BUILD="$PS3_BUILD_ROOT/ppu"

JOBS="$(nproc 2>/dev/null || echo 4)"
say "Using $JOBS parallel jobs."

# CLI: optional --clean wipes build dir; --only <step> runs only that step.
# Steps: binutils | gcc-newlib | gdb | symlinks (default: all in order).
ONLY=""
for arg in "$@"; do
    case "$arg" in
        --clean) say "Cleaning $BUILD"; rm -rf "$BUILD" ;;
        --only=*) ONLY="${arg#--only=}" ;;
        --only) shift; ONLY="${1:-}" ;;
        binutils|gcc-newlib|gdb|symlinks) ONLY="$arg" ;;
    esac
done

mkdir -p "$BUILD" "$PREFIX"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

# Extract a pinned tag from a git mirror into a fresh source tree.
extract_source() {
    local repo="$1" tag="$2" dest="$3"
    if [[ -d "$dest" ]]; then
        say "Source $dest already extracted (skipping — delete to re-extract)"
        return 0
    fi
    say "Extracting $tag from $repo -> $dest"
    mkdir -p "$dest"
    git -C "$repo" archive --format=tar "$tag" | tar -x -C "$dest"
}

# Apply patches from a directory. Honors a 'series' file if present (quilt-style).
apply_patches() {
    local src="$1" patches_dir="$2"
    if [[ ! -d "$patches_dir" ]]; then
        say "No patches directory at $patches_dir (skipping)"
        return 0
    fi

    local patch_list
    if [[ -f "$patches_dir/series" ]]; then
        mapfile -t patch_list < <(grep -v '^#' "$patches_dir/series" | grep -v '^\s*$')
    else
        mapfile -t patch_list < <(cd "$patches_dir" && ls *.patch 2>/dev/null | sort)
    fi

    if [[ ${#patch_list[@]} -eq 0 ]]; then
        say "No patches to apply in $patches_dir"
        return 0
    fi

    for p in "${patch_list[@]}"; do
        say "Applying $p to $src"
        (cd "$src" && patch -p1 --forward) < "$patches_dir/$p" \
            || die "Patch $p failed"
    done
}

# -----------------------------------------------------------------------------
# 1. Binutils 2.42
# -----------------------------------------------------------------------------
build_binutils() {
    local src="$BUILD/binutils-$BINUTILS_VER-src"
    local obj="$BUILD/binutils-$BINUTILS_VER-build"

    extract_source "$UPSTREAM/binutils-gdb" "$BINUTILS_TAG" "$src"

    # The binutils-gdb repo bundles readline, libdecnumber, and sim sources that
    # are only needed for GDB. We build GDB separately in build_gdb(), so remove
    # these to prevent the top-level Makefile from trying to compile them. The
    # bundled readline (circa 2023) fails to compile under GCC 14+ due to strict
    # -Wincompatible-pointer-types enforcement in signals.c.
    rm -rf "$src/readline" "$src/libdecnumber" "$src/sim" "$src/gdb" "$src/gdbserver" "$src/gdbsupport" "$src/gnulib"

    apply_patches "$src" "$PATCHES/binutils-2.42"

    if [[ -f "$obj/.installed" ]]; then
        say "Binutils already built and installed (skipping)"
        return 0
    fi

    mkdir -p "$obj"
    say "Configuring binutils -> $PREFIX (target=$TARGET)"
    (cd "$obj" && "$src/configure" \
        --prefix="$PREFIX" \
        --target="$TARGET" \
        --with-cpu=cell \
        --disable-nls \
        --disable-shared \
        --disable-werror \
        --disable-dependency-tracking \
        --disable-gdb \
        --disable-gdbserver \
        --disable-sim \
        --enable-64-bit-bfd \
        --enable-lto \
        --enable-plugins \
        --with-gcc \
        --with-gnu-as \
        --with-gnu-ld)

    say "Building binutils"
    (cd "$obj" && make -j"$JOBS")

    say "Installing binutils"
    (cd "$obj" && make install)
    touch "$obj/.installed"
}

# -----------------------------------------------------------------------------
# 2. GCC 12.4.0 + newlib 4.4.0 (combined-tree build)
#
#    The combined-tree pattern symlinks newlib/libgloss into the GCC source tree
#    so a single build produces libgcc, newlib, libstdc++ in one pass. This is
#    the canonical approach for embedded cross-toolchains.
# -----------------------------------------------------------------------------
build_gcc_newlib() {
    local gcc_src="$BUILD/gcc-$GCC_VER-src"
    local newlib_src="$BUILD/newlib-$NEWLIB_VER-src"
    local obj="$BUILD/gcc-$GCC_VER-build"

    extract_source "$UPSTREAM/gcc"            "$GCC_TAG"    "$gcc_src"
    extract_source "$UPSTREAM/newlib-cygwin"  "$NEWLIB_TAG" "$newlib_src"

    apply_patches "$gcc_src"    "$PATCHES/gcc-12.x"
    apply_patches "$newlib_src" "$PATCHES/newlib-4.x"

    # GCC prereqs: GMP, MPFR, MPC, ISL. Use contrib/download_prerequisites which
    # drops them as in-tree symlinks. Requires network access.
    if [[ ! -d "$gcc_src/gmp" ]]; then
        say "Downloading GCC prerequisites (GMP, MPFR, MPC, ISL)"
        (cd "$gcc_src" && ./contrib/download_prerequisites)
    fi

    # Symlink newlib and libgloss into the GCC tree for combined build.
    if [[ ! -e "$gcc_src/newlib" ]]; then
        ln -sf "$newlib_src/newlib"   "$gcc_src/newlib"
        ln -sf "$newlib_src/libgloss" "$gcc_src/libgloss"
    fi

    if [[ -f "$obj/.installed" ]]; then
        say "GCC+newlib already built and installed (skipping)"
        return 0
    fi

    mkdir -p "$obj"
    say "Configuring GCC+newlib -> $PREFIX (target=$TARGET, GCC $GCC_VER)"

    # Threading strategy: start with --enable-threads=single to unblock Phase 1d.
    # Phase 1e swaps in --enable-threads=posix after libsysbase gthr shim lands.
    local threads_mode="${PS3_GCC_THREADS:-single}"

    (cd "$obj" && "$gcc_src/configure" \
        --prefix="$PREFIX" \
        --target="$TARGET" \
        --with-cpu=cell \
        --with-newlib \
        --enable-languages=c,c++ \
        --enable-lto \
        --enable-threads="$threads_mode" \
        --enable-long-double-128 \
        --enable-newlib-io-long-long \
        --enable-newlib-io-c99-formats \
        --disable-dependency-tracking \
        --disable-libcc1 \
        --disable-libssp \
        --disable-libstdcxx-pch \
        --disable-libstdcxx-filesystem-ts \
        --disable-multilib \
        --disable-nls \
        --disable-shared \
        --disable-win32-registry \
        --with-system-zlib)

    say "Building GCC+newlib (this takes a while — ~30-90 minutes)"
    (cd "$obj" && make -j"$JOBS" all)

    say "Installing GCC+newlib"
    (cd "$obj" && make install)
    touch "$obj/.installed"
}

# -----------------------------------------------------------------------------
# 3. GDB 14.2
# -----------------------------------------------------------------------------
build_gdb() {
    local src="$BUILD/gdb-$GDB_VER-src"
    local obj="$BUILD/gdb-$GDB_VER-build"

    extract_source "$UPSTREAM/binutils-gdb" "$GDB_TAG" "$src"
    apply_patches "$src" "$PATCHES/gdb-14.x"

    if [[ -f "$obj/.installed" ]]; then
        say "GDB already built and installed (skipping)"
        return 0
    fi

    mkdir -p "$obj"
    say "Configuring GDB -> $PREFIX (target=$TARGET)"
    (cd "$obj" && "$src/configure" \
        --prefix="$PREFIX" \
        --target="$TARGET" \
        --disable-nls \
        --disable-werror \
        --disable-dependency-tracking \
        --with-python=yes \
        --with-system-readline)

    say "Building GDB"
    # Build only the gdb subdir — we already built binutils/ld/gas/bfd above.
    (cd "$obj" && make -j"$JOBS" all-gdb)

    say "Installing GDB"
    (cd "$obj" && make install-gdb)
    touch "$obj/.installed"
}

# -----------------------------------------------------------------------------
# 4. Short-name symlinks (ppu-gcc, ppu-g++, etc.)
# -----------------------------------------------------------------------------
create_symlinks() {
    local bin="$PREFIX/bin"
    if [[ ! -d "$bin" ]]; then
        warn "No $bin to symlink — skipping alias creation"
        return 0
    fi
    say "Creating ppu-* aliases in $bin"
    cd "$bin"
    # ppu -> powerpc64-ps3-elf directory alias
    if [[ ! -e "$PREFIX/ppu" && -d "$PREFIX/$TARGET" ]]; then
        (cd "$PREFIX" && ln -sf "$TARGET" ppu)
    fi
    for tool in "$TARGET"-*; do
        [[ -f "$tool" || -L "$tool" ]] || continue
        local short="ppu-${tool#"$TARGET"-}"
        [[ -e "$short" ]] && continue
        ln -sf "$tool" "$short"
    done
}

# -----------------------------------------------------------------------------
# Run
# -----------------------------------------------------------------------------
say "=== Phase 1: PPU toolchain ==="
say "binutils $BINUTILS_VER  gcc $GCC_VER  newlib $NEWLIB_VER  gdb $GDB_VER"
say "prefix=$PREFIX  build=$BUILD"
[[ -n "$ONLY" ]] && say "Running only: $ONLY"

run_step() {
    local step="$1"
    [[ -n "$ONLY" && "$ONLY" != "$step" ]] && return 0
    case "$step" in
        binutils)    build_binutils ;;
        gcc-newlib)  build_gcc_newlib ;;
        gdb)         build_gdb ;;
        symlinks)    create_symlinks ;;
    esac
}

run_step binutils
run_step gcc-newlib
run_step gdb
run_step symlinks

say "=== Done ==="
if [[ -x "$PREFIX/bin/$TARGET-gcc" ]]; then
    "$PREFIX/bin/$TARGET-gcc" --version | head -1
elif [[ -x "$PREFIX/bin/$TARGET-as" ]]; then
    "$PREFIX/bin/$TARGET-as" --version | head -1
fi
say "Next: source scripts/env.sh in a fresh shell and try samples/hello-ppu-c++17/."
