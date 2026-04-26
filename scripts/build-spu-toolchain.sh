#!/usr/bin/env bash
# PS3 Custom Toolchain — SPU toolchain (spu-elf)
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
#     a long-lead workstream, kept in a separate branch.
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
NEWLIB_TAG="newlib-4.4.0"
NEWLIB_VER="4.4.0.20231231"

TARGET="spu-elf"
PREFIX="$PS3DEV/spu"

UPSTREAM="$PS3_TOOLCHAIN_ROOT/src/upstream"
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/spu"
BUILD="$PS3_BUILD_ROOT/spu"

JOBS="$(nproc 2>/dev/null || echo 4)"
say "Using $JOBS parallel jobs."

ONLY=""
HOST_TRIPLE=""
BUILD_TRIPLE=""
for arg in "$@"; do
    case "$arg" in
        --clean) say "Cleaning $BUILD"; rm -rf "$BUILD" ;;
        --only=*) ONLY="${arg#--only=}" ;;
        --only) shift; ONLY="${1:-}" ;;
        --host=*) HOST_TRIPLE="${arg#--host=}" ;;
        --build=*) BUILD_TRIPLE="${arg#--build=}" ;;
        binutils|gcc-newlib|symlinks|linker-script) ONLY="$arg" ;;
    esac
done

# Cross-build mode (Windows-hosted toolchain): see build-ppu-toolchain.sh's
# header comment for the same option semantics.
NATIVE_PREFIX="$PREFIX"
if [[ -n "$HOST_TRIPLE" ]]; then
    PREFIX="${PS3DEV}.win/spu"
    BUILD="$PS3_BUILD_ROOT/spu-${HOST_TRIPLE}"
    BUILD_TRIPLE="${BUILD_TRIPLE:-$(uname -m)-pc-linux-gnu}"
    [[ -x "$NATIVE_PREFIX/bin/$TARGET-gcc" ]] \
        || die "Native SPU toolchain not present at $NATIVE_PREFIX/bin/$TARGET-gcc. Run without --host first to produce stage 0."
    say "Cross-build: host=$HOST_TRIPLE build=$BUILD_TRIPLE prefix=$PREFIX"
fi

# See build-ppu-toolchain.sh for the rationale on this array form vs.
# a plain LDFLAGS variable.
CROSS_LDFLAGS_ARGS=()
if [[ -n "$HOST_TRIPLE" ]]; then
    CROSS_LDFLAGS_ARGS=("LDFLAGS=-static -static-libgcc -static-libstdc++")
fi

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
    [[ ${#patch_list[@]} -eq 0 ]] && { say "No patches in $patches_dir"; return 0; }
    for p in "${patch_list[@]}"; do
        say "Applying $p to $src"
        (cd "$src" && patch -p1 --forward) < "$patches_dir/$p" \
            || die "Patch $p failed (consider --clean to wipe build root and re-extract)"
    done
    touch "$stamp"
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
    say "Configuring binutils -> $PREFIX (target=$TARGET${HOST_TRIPLE:+, host=$HOST_TRIPLE})"

    local cross_args=()
    if [[ -n "$HOST_TRIPLE" ]]; then
        cross_args+=(--host="$HOST_TRIPLE" --build="$BUILD_TRIPLE")
    fi

    (cd "$obj" && \
        env "${CROSS_LDFLAGS_ARGS[@]}" \
        "$src/configure" \
        --prefix="$PREFIX" \
        --target="$TARGET" \
        "${cross_args[@]}" \
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

    extract_source "$UPSTREAM/gcc" "$GCC_TAG" "$gcc_src"
    apply_patches "$gcc_src" "$PATCHES/gcc-9.5.0"

    # Newlib only built in the native (stage 0) flow.  Cross-builds reuse
    # the SPU ELF target libs from the native install.
    if [[ -z "$HOST_TRIPLE" ]]; then
        extract_source "$UPSTREAM/newlib-cygwin" "$NEWLIB_TAG" "$newlib_src"
        apply_patches "$newlib_src" "$PATCHES/newlib-4.x"
    fi

    if [[ ! -d "$gcc_src/gmp" ]]; then
        say "Downloading GCC prerequisites"
        (cd "$gcc_src" && ./contrib/download_prerequisites)
    fi

    if [[ -z "$HOST_TRIPLE" && ! -e "$gcc_src/newlib" ]]; then
        ln -sf "$newlib_src/newlib"   "$gcc_src/newlib"
        ln -sf "$newlib_src/libgloss" "$gcc_src/libgloss"
    fi

    [[ -f "$obj/.installed" ]] && { say "GCC already built (skipping)"; return 0; }

    mkdir -p "$obj"
    say "Configuring GCC -> $PREFIX (target=$TARGET, GCC $GCC_VER${HOST_TRIPLE:+, host=$HOST_TRIPLE})"

    local conf_args=(
        --prefix="$PREFIX"
        --target="$TARGET"
        --enable-languages=c,c++
        --enable-lto
        --enable-obsolete
        --disable-dependency-tracking
        --disable-libcc1
        --disable-libssp
        --disable-libstdcxx-pch
        --disable-libstdcxx-filesystem-ts
        --disable-multilib
        --disable-nls
        --disable-shared
        --disable-threads
        --disable-win32-registry
        --with-pic
    )

    local target_env=()
    if [[ -n "$HOST_TRIPLE" ]]; then
        # Cross-build: host binaries only, no target libs (reused from native).
        conf_args+=(
            --host="$HOST_TRIPLE"
            --build="$BUILD_TRIPLE"
            --disable-bootstrap
        )
        # Export enable_obsolete=yes so gcc/config.gcc accepts the obsolete
        # spu-elf target during the gcc/ sub-configure that `make all-host`
        # spawns — the top-level --enable-obsolete flag doesn't propagate
        # to that sub-configure under cross-build.  CC_FOR_BUILD / CXX_FOR_BUILD
        # ensure build-machine auxiliary tools (genmddeps etc.) compile with
        # the local Linux gcc, not the mingw cross compiler.
        export enable_obsolete=yes
        target_env=(
            CC_FOR_BUILD=gcc
            CXX_FOR_BUILD=g++
        )
    else
        conf_args+=(--with-newlib)
        # SPU optimization flags for libgcc/newlib.  Note: -fpic is omitted
        # because it causes relocation overflows in libgcc's cachemgr.c with
        # binutils 2.42.  User code should add -fpic as needed in their own
        # CFLAGS.  -mno-branch-hints: prevents hbrr instruction relocation
        # overflows with binutils 2.42 for large functions (cachemgr, etc.)
        local cflags_target="-Os -ffast-math -ftree-vectorize -funroll-loops -fschedule-insns -mdual-nops -mno-branch-hints"
        # SPU's libm/machine/spu fenv files use #include "headers/fefpscr.h"
        # relative to their source directory.  libc/machine/spu/memcpy.c pulls
        # vec_literal.h the same way — add -I so the compiler finds them.
        local target_includes="-I$newlib_src/newlib/libm/machine/spu -I$newlib_src/newlib/libc/machine/spu"
        # CXXFLAGS_FOR_TARGET mirrors CFLAGS_FOR_TARGET: libstdc++ is large
        # enough that without -mno-branch-hints the hbrr instruction's 16-bit
        # displacement overflows in functexcept.cc / system_error.cc and
        # binutils aborts.
        target_env=(
            CFLAGS_FOR_TARGET="$cflags_target $target_includes"
            CXXFLAGS_FOR_TARGET="$cflags_target $target_includes"
        )
    fi

    (cd "$obj" && \
        env "${target_env[@]}" "${CROSS_LDFLAGS_ARGS[@]}" \
        "$gcc_src/configure" "${conf_args[@]}")

    if [[ -n "$HOST_TRIPLE" ]]; then
        say "Building GCC host-only (~15-25 minutes)"
        (cd "$obj" && make -j"$JOBS" all-host)
        say "Installing GCC host-only"
        (cd "$obj" && make install-host)
        copy_target_libs_from_native
    else
        say "Building GCC+newlib (SPU; ~20-40 minutes)"
        (cd "$obj" && make -j"$JOBS" all)
        say "Installing GCC+newlib"
        (cd "$obj" && make install)
    fi
    touch "$obj/.installed"
}

# Cross-build helper: reuse target libs from the native install.
copy_target_libs_from_native() {
    [[ -n "$HOST_TRIPLE" ]] || return 0
    say "Copying target libraries: $NATIVE_PREFIX -> $PREFIX"
    if [[ -d "$NATIVE_PREFIX/$TARGET" ]]; then
        mkdir -p "$PREFIX/$TARGET"
        cp -a "$NATIVE_PREFIX/$TARGET/." "$PREFIX/$TARGET/"
    fi
    if [[ -d "$NATIVE_PREFIX/lib/gcc/$TARGET" ]]; then
        mkdir -p "$PREFIX/lib/gcc/$TARGET"
        cp -a "$NATIVE_PREFIX/lib/gcc/$TARGET/." "$PREFIX/lib/gcc/$TARGET/"
    fi
}

create_symlinks() {
    if [[ -n "$HOST_TRIPLE" ]]; then
        say "Skipping spu-* alias creation (cross-build target=$HOST_TRIPLE)"
        return 0
    fi
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

say "=== SPU toolchain ==="
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
