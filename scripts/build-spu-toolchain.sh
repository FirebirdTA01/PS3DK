#!/usr/bin/env bash
# PS3 Custom Toolchain — Phase 2a: SPU toolchain (spu-elf)
#
# Builds:
#   binutils 2.42  -> $PS3DEV/spu  (spu-elf target; upstream still intact)
#   GCC 9.5.0      -> $PS3DEV/spu  (last GCC with a working SPU backend)
#   newlib 4.4.0   -> combined build (libgloss/spu upstream + PS3 extras patch)
#
# Why GCC 9.5.0 here and 12.4.0 for PPU:
#   - GCC 9 was the last version with an intact config/spu/ backend.
#   - GCC 9.5.0 is the newest 9.x patch release; includes C++17 complete and
#     partial C++20.
#   - GCC 10 removed ~34k lines of SPU backend. Forward-porting to 12.4.0 is
#     Phase 2b (long-lead), kept in a separate branch.
#
# SPU C++ constraints (256 KB local store budget):
#   - Default spec file adds -fno-exceptions -fno-rtti -Os for size.
#   - libstdc++ builds without filesystem TS / PCH.
#   - libspu_mini.a (shipped by PSL1GHT) offers tighter printf/memcpy/memset
#     variants for projects that can't afford newlib's full surface.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[spu] %s\n" "$*"; }
warn() { printf "[spu] WARNING: %s\n" "$*" >&2; }
die() { printf "[spu] ERROR: %s\n" "$*" >&2; exit 1; }

BINUTILS_TAG="binutils-2_42"
BINUTILS_VER="2.42"
GCC_TAG="releases/gcc-9.5.0"
GCC_VER="9.5.0"
NEWLIB_TAG="newlib-4.4.0.20231231"
NEWLIB_VER="4.4.0.20231231"

TARGET="spu-elf"
PREFIX="$PS3DEV/spu"

UPSTREAM="$PS3_TOOLCHAIN_ROOT/src/upstream"
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/spu"
BUILD="$PS3_BUILD_ROOT/spu"

JOBS="$(nproc 2>/dev/null || echo 4)"
say "Using $JOBS parallel jobs."

ONLY=""
for arg in "$@"; do
    case "$arg" in
        --clean) say "Cleaning $BUILD"; rm -rf "$BUILD" ;;
        --only=*) ONLY="${arg#--only=}" ;;
        --only) shift; ONLY="${1:-}" ;;
        binutils|gcc-newlib|symlinks|linker-script) ONLY="$arg" ;;
    esac
done

mkdir -p "$BUILD" "$PREFIX"

extract_source() {
    local repo="$1" tag="$2" dest="$3"
    if [[ -d "$dest" ]]; then
        say "Source $dest already extracted (skipping)"
        return 0
    fi
    say "Extracting $tag from $repo -> $dest"
    mkdir -p "$dest"
    git -C "$repo" archive --format=tar "$tag" | tar -x -C "$dest"
}

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
    [[ ${#patch_list[@]} -eq 0 ]] && { say "No patches in $patches_dir"; return 0; }
    for p in "${patch_list[@]}"; do
        say "Applying $p to $src"
        (cd "$src" && patch -p1 --forward) < "$patches_dir/$p" || die "Patch $p failed"
    done
}

build_binutils() {
    local src="$BUILD/binutils-$BINUTILS_VER-src"
    local obj="$BUILD/binutils-$BINUTILS_VER-build"

    extract_source "$UPSTREAM/binutils-gdb" "$BINUTILS_TAG" "$src"

    # Remove GDB-only components from the combined binutils-gdb source tree.
    # The bundled readline fails to compile under GCC 14+ (strict type checking
    # in signals.c). We build GDB separately, so these aren't needed here.
    rm -rf "$src/readline" "$src/libdecnumber" "$src/sim" "$src/gdb" "$src/gdbserver" "$src/gdbsupport" "$src/gnulib"

    apply_patches "$src" "$PATCHES/binutils-2.42"

    [[ -f "$obj/.installed" ]] && { say "Binutils already built (skipping)"; return 0; }

    mkdir -p "$obj"
    say "Configuring binutils -> $PREFIX (target=$TARGET)"
    (cd "$obj" && "$src/configure" \
        --prefix="$PREFIX" \
        --target="$TARGET" \
        --disable-nls \
        --disable-shared \
        --disable-werror \
        --disable-dependency-tracking \
        --disable-gdb \
        --disable-gdbserver \
        --disable-sim \
        --with-gcc \
        --with-gnu-as \
        --with-gnu-ld)

    say "Building binutils"
    (cd "$obj" && make -j"$JOBS")
    say "Installing binutils"
    (cd "$obj" && make install)
    touch "$obj/.installed"
}

build_gcc_newlib() {
    local gcc_src="$BUILD/gcc-$GCC_VER-src"
    local newlib_src="$BUILD/newlib-$NEWLIB_VER-src"
    local obj="$BUILD/gcc-$GCC_VER-build"

    extract_source "$UPSTREAM/gcc"           "$GCC_TAG"    "$gcc_src"
    extract_source "$UPSTREAM/newlib-cygwin" "$NEWLIB_TAG" "$newlib_src"

    apply_patches "$gcc_src"    "$PATCHES/gcc-9.5.0"
    apply_patches "$newlib_src" "$PATCHES/newlib-4.x"

    if [[ ! -d "$gcc_src/gmp" ]]; then
        say "Downloading GCC prerequisites"
        (cd "$gcc_src" && ./contrib/download_prerequisites)
    fi

    if [[ ! -e "$gcc_src/newlib" ]]; then
        ln -sf "$newlib_src/newlib"   "$gcc_src/newlib"
        ln -sf "$newlib_src/libgloss" "$gcc_src/libgloss"
    fi

    [[ -f "$obj/.installed" ]] && { say "GCC+newlib already built (skipping)"; return 0; }

    mkdir -p "$obj"
    # SPU optimization flags matching ps3dev's proven settings for libgcc/newlib.
    local cflags_target="-Os -fpic -ffast-math -ftree-vectorize -funroll-loops -fschedule-insns -mdual-nops -mwarn-reloc"

    say "Configuring GCC+newlib -> $PREFIX (target=$TARGET, GCC $GCC_VER)"
    (cd "$obj" && CFLAGS_FOR_TARGET="$cflags_target" "$gcc_src/configure" \
        --prefix="$PREFIX" \
        --target="$TARGET" \
        --with-newlib \
        --enable-languages=c,c++ \
        --enable-lto \
        --disable-dependency-tracking \
        --disable-libcc1 \
        --disable-libssp \
        --disable-libstdcxx-pch \
        --disable-libstdcxx-filesystem-ts \
        --disable-multilib \
        --disable-nls \
        --disable-shared \
        --disable-threads \
        --disable-win32-registry \
        --with-pic)

    say "Building GCC+newlib (SPU; ~20-40 minutes)"
    (cd "$obj" && make -j"$JOBS" all)
    say "Installing GCC+newlib"
    (cd "$obj" && make install)
    touch "$obj/.installed"
}

create_symlinks() {
    local bin="$PREFIX/bin"
    [[ -d "$bin" ]] || { warn "No $bin to symlink"; return 0; }
    say "Creating spu-* aliases in $bin"
    if [[ ! -e "$PREFIX/spu" && -d "$PREFIX/$TARGET" ]]; then
        (cd "$PREFIX" && ln -sf "$TARGET" spu)
    fi
    cd "$bin"
    for tool in "$TARGET"-*; do
        [[ -f "$tool" || -L "$tool" ]] || continue
        local short="spu-${tool#"$TARGET"-}"
        [[ -e "$short" ]] && continue
        ln -sf "$tool" "$short"
    done
}

install_linker_script() {
    local dst="$PREFIX/share/ld"
    mkdir -p "$dst"
    local src="$PS3_TOOLCHAIN_ROOT/patches/spu/spu_ps3.ld"
    if [[ -f "$src" ]]; then
        say "Installing spu_ps3.ld -> $dst/"
        cp "$src" "$dst/spu_ps3.ld"
    else
        warn "spu_ps3.ld not found at $src (LS linker script not shipped yet)"
    fi
}

say "=== Phase 2a: SPU toolchain ==="
say "binutils $BINUTILS_VER  gcc $GCC_VER  newlib $NEWLIB_VER"
say "prefix=$PREFIX  build=$BUILD"
[[ -n "$ONLY" ]] && say "Running only: $ONLY"

run_step() {
    local step="$1"
    [[ -n "$ONLY" && "$ONLY" != "$step" ]] && return 0
    case "$step" in
        binutils)       build_binutils ;;
        gcc-newlib)     build_gcc_newlib ;;
        symlinks)       create_symlinks ;;
        linker-script)  install_linker_script ;;
    esac
}

run_step binutils
run_step gcc-newlib
run_step symlinks
run_step linker-script

say "=== Done ==="
if [[ -x "$PREFIX/bin/$TARGET-gcc" ]]; then
    "$PREFIX/bin/$TARGET-gcc" --version | head -1
elif [[ -x "$PREFIX/bin/$TARGET-as" ]]; then
    "$PREFIX/bin/$TARGET-as" --version | head -1
fi
say "Next: try samples/hello-spu/."
