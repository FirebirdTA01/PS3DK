#!/usr/bin/env bash
# PS3 Custom Toolchain - PPU kernel-side toolchain (powerpc64-ps3-kernel-elf)
#
# Builds:
#   binutils 2.42  -> $PS3DEV/ppu-kernel
#   GCC 12.4.0     -> $PS3DEV/ppu-kernel (compiler only, no target runtime)
#
# libgcc is intentionally not wired yet. Invoking that item exits non-zero
# with a clear message until the kernel runtime work is gated.
#
# Source extraction is via `git archive` from the mirrors cloned by bootstrap.sh.
# Patches are applied from patches/ppu/<component>/ in the order listed in
# patches/ppu/<component>/series (or, if no series file exists, sorted by name).
#
# Re-runnable: delete $PS3_BUILD_ROOT/ppu-kernel to force a clean rebuild, or
# run with --clean to do it for you.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[ppu-kernel] %s\n" "$*"; }
warn() { printf "[ppu-kernel] WARNING: %s\n" "$*" >&2; }
die() { printf "[ppu-kernel] ERROR: %s\n" "$*" >&2; exit 1; }

BINUTILS_TAG="binutils-2_42"
BINUTILS_VER="2.42"
GCC_TAG="releases/gcc-12.4.0"
GCC_VER="12.4.0"

TARGET="powerpc64-ps3-kernel-elf"
PREFIX="$PS3DEV/ppu-kernel"

UPSTREAM="$PS3_TOOLCHAIN_ROOT/src/upstream"
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/ppu"
BUILD="$PS3_BUILD_ROOT/ppu-kernel"

JOBS="$(nproc 2>/dev/null || echo 4)"
say "Using $JOBS parallel jobs."

# CLI: optional --clean wipes build dir; --only <item> runs only that item.
# Items: binutils | gcc | libgcc (default: all in order; libgcc is stubbed).
ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            say "Cleaning $BUILD"
            rm -rf "$BUILD"
            shift
            ;;
        --only=*)
            ONLY="${1#--only=}"
            shift
            ;;
        --only)
            shift
            ONLY="${1:-}"
            [[ -n "$ONLY" ]] || die "--only requires an item: binutils, gcc, or libgcc"
            shift
            ;;
        binutils|gcc|libgcc)
            ONLY="$1"
            shift
            ;;
        *)
            die "Unknown argument: $1"
            ;;
    esac
done

case "$ONLY" in
    ""|binutils|gcc|libgcc) ;;
    *) die "Unknown --only item '$ONLY' (expected: binutils, gcc, or libgcc)" ;;
esac

mkdir -p "$BUILD" "$PREFIX"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

extract_source() {
    local repo="$1" tag="$2" dest="$3"
    if [[ -d "$dest" ]]; then
        say "Source $dest already extracted (skipping - delete to re-extract)"
        return 0
    fi

    local attempt
    for attempt in 1 2 3; do
        say "Extracting $tag from $repo -> $dest (attempt $attempt)"
        rm -rf "$dest"
        mkdir -p "$dest"
        if git -C "$repo" archive --format=tar "$tag" | tar -x -C "$dest"; then
            return 0
        fi
        warn "git archive failed; refetching tag and retrying"
        (cd "$repo" && git fetch --depth 1 origin "refs/tags/$tag:refs/tags/$tag" 2>/dev/null) || true
        sleep 5
    done
    die "Failed to extract $tag from $repo after 3 attempts"
}

apply_patches() {
    local src="$1" patches_dir="$2"
    if [[ ! -d "$patches_dir" ]]; then
        say "No patches directory at $patches_dir (skipping)"
        return 0
    fi

    local stamp="$src/.patches-applied-${patches_dir##*/}"
    if [[ -f "$stamp" ]]; then
        say "Patches from $patches_dir already applied (stamp present)"
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
            || die "Patch $p failed (consider --clean to wipe build root and re-extract)"
    done

    touch "$stamp"
}

# -----------------------------------------------------------------------------
# Binutils 2.42
# -----------------------------------------------------------------------------

build_binutils() {
    local src="$BUILD/binutils-$BINUTILS_VER-src"
    local obj="$BUILD/binutils-$BINUTILS_VER-build"

    extract_source "$UPSTREAM/binutils-gdb" "$BINUTILS_TAG" "$src"

    # GDB-only components are not needed for the binutils-only kernel target
    # gate and can trip modern host compiler warnings in bundled dependencies.
    rm -rf "$src/readline" "$src/libdecnumber" "$src/sim" "$src/gdb" "$src/gdbserver" "$src/gdbsupport" "$src/gnulib"

    apply_patches "$src" "$PATCHES/binutils-2.42"

    if [[ -f "$obj/.installed" ]]; then
        say "Binutils already built and installed (skipping)"
        return 0
    fi

    mkdir -p "$obj"
    say "Configuring binutils -> $PREFIX (target=$TARGET)"

    (cd "$obj" && \
        "$src/configure" \
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

build_gcc() {
    local src="$BUILD/gcc-$GCC_VER-src"
    local obj="$BUILD/gcc-$GCC_VER-build"

    extract_source "$UPSTREAM/gcc" "$GCC_TAG" "$src"
    apply_patches "$src" "$PATCHES/gcc-12.x"

    # GCC prereqs: GMP, MPFR, MPC, ISL. Use contrib/download_prerequisites
    # which drops them as in-tree symlinks.
    if [[ ! -d "$src/gmp" ]]; then
        say "Downloading GCC prerequisites (GMP, MPFR, MPC, ISL)"
        (cd "$src" && ./contrib/download_prerequisites)
    fi

    # If any GCC patch is newer than the install stamp, rebuild the compiler.
    if [[ -f "$obj/.installed" ]]; then
        local newer_patch
        newer_patch=$(find "$PATCHES/gcc-12.x" \
            -name "*.patch" -newer "$obj/.installed" 2>/dev/null | head -1)
        if [[ -n "$newer_patch" ]]; then
            say "Patch newer than .installed — invalidating GCC compiler build ($newer_patch)"
            rm -f "$obj/.installed"
        fi
    fi

    if [[ -f "$obj/.installed" ]]; then
        say "GCC already built and installed (skipping)"
        return 0
    fi

    mkdir -p "$obj"
    say "Configuring GCC compiler -> $PREFIX (target=$TARGET, GCC $GCC_VER)"

    local conf_args=(
        --prefix="$PREFIX"
        --target="$TARGET"
        --with-cpu=cell
        --enable-languages=c,c++
        --enable-lto
        --enable-long-double-128
        --disable-dependency-tracking
        --disable-libcc1
        --disable-libssp
        --disable-libstdcxx-pch
        --disable-libstdcxx-filesystem-ts
        --disable-nls
        --disable-shared
        --disable-win32-registry
        --with-system-zlib
        --without-headers
    )

    # GCC 12's host-side libcody requires exact C++11.  Newer rolling
    # hosts can default g++ to C++20-or-later semantics where u8 literals
    # become char8_t, which libcody does not accept.
    local target_env=(
        CXXFLAGS=-std=gnu++11
        CXXFLAGS_FOR_BUILD=-std=gnu++11
    )

    (cd "$obj" && \
        env "${target_env[@]}" \
        "$src/configure" "${conf_args[@]}")

    say "Building GCC compiler only"
    (cd "$obj" && make -j"$JOBS" all-gcc)

    say "Installing GCC compiler only"
    (cd "$obj" && make install-gcc)
    touch "$obj/.installed"
}

build_libgcc() {
    warn "workstream (4) kernel libgcc is not yet implemented; use --only=binutils for the current green path"
    die "libgcc item not implemented"
}

# -----------------------------------------------------------------------------
# Run
# -----------------------------------------------------------------------------

say "=== PPU kernel-side toolchain ==="
say "binutils $BINUTILS_VER  gcc $GCC_VER"
say "prefix=$PREFIX  build=$BUILD"
[[ -n "$ONLY" ]] && say "Running only: $ONLY"

run_item() {
    local item="$1"
    [[ -n "$ONLY" && "$ONLY" != "$item" ]] && return 0
    case "$item" in
        binutils) build_binutils ;;
        gcc)      build_gcc ;;
        libgcc)   build_libgcc ;;
    esac
}

run_item binutils
run_item gcc
run_item libgcc

say "=== Done ==="
if [[ -x "$PREFIX/bin/$TARGET-as" ]]; then
    "$PREFIX/bin/$TARGET-as" --version | head -1
fi
if [[ -x "$PREFIX/bin/$TARGET-gcc" ]]; then
    "$PREFIX/bin/$TARGET-gcc" --version | head -1
fi
