#!/usr/bin/env bash
# PS3 Custom Toolchain — SPU toolchain (spu-elf)
#
# Builds:
#   binutils 2.42  -> $PS3DEV/spu  (spu-elf target; upstream still intact)
#   GCC 16.1.0     -> $PS3DEV/spu  (the SPU backend forward-ported from 9.5.0;
#                                   --gcc=9 builds 9.5.0, the last upstream SPU GCC)
#   newlib 4.4.0   -> combined build (libgloss/spu upstream + PS3 extras patch)
#
# Why a forward port:
#   - GCC 9 was the last version with an intact config/spu/ backend; GCC 10
#     removed ~34k lines of it.
#   - The default build (--gcc=16) is GCC 16.1.0 with the 9.5.0 backend
#     restored from the releases/gcc-9.5.0 tag and the patches/spu/gcc-16.x
#     series on top (docs/design/spu-gcc16-port.md), the same GCC release as
#     the PPU compiler.  --gcc=9 still builds GCC 9.5.0 with patches/spu/gcc-9.5.0.
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
GCC_PATCH_DIR="gcc-9.5.0"
# --gcc=16: the forward-ported SPU backend (see the header).
SPU_BACKEND_TAG="releases/gcc-9.5.0"
NEWLIB_TAG="newlib-4.4.0"
NEWLIB_VER="4.4.0"

TARGET="spu-elf"
PREFIX="$PS3DEV/spu"

UPSTREAM="$PS3_TOOLCHAIN_ROOT/src/upstream"
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/spu"
BUILD="$PS3_BUILD_ROOT/spu"
SPU_GCC=16
CLEAN=0
PRINT_CONFIG=0

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
say "Using $JOBS parallel jobs."

ONLY=""
HOST_TRIPLE=""
BUILD_TRIPLE=""
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --print-config) PRINT_CONFIG=1 ;;
        --only=*) ONLY="${arg#--only=}" ;;
        --only) shift; ONLY="${1:-}" ;;
        --host=*) HOST_TRIPLE="${arg#--host=}" ;;
        --build=*) BUILD_TRIPLE="${arg#--build=}" ;;
        --gcc=16) SPU_GCC=16 ;;
        --gcc=9|--gcc=9.5) SPU_GCC=9 ;;
        --gcc=*) die "unsupported ${arg}: the SPU compiler is 16 (16.1.0 forward port, default) or 9 (9.5.0)" ;;
        binutils|gcc-newlib|symlinks|linker-script) ONLY="$arg" ;;
    esac
done

# Cross-build mode (Windows-hosted toolchain): see build-ppu-toolchain.sh's
# header comment for the same option semantics.
if [[ "$SPU_GCC" == 16 ]]; then
    GCC_TAG="releases/gcc-16.1.0"
    GCC_VER="16.1.0"
    GCC_PATCH_DIR="gcc-16.x"
    # A separate build root, so a 9.5 tree is never reused for 16.
    BUILD="${BUILD}-gcc16"
fi

NATIVE_PREFIX="$PREFIX"
if [[ -n "$HOST_TRIPLE" ]]; then
    PREFIX="${PS3DEV}.win/spu"
    # Keep the compiler identity in the cross root too (spu-gcc16-<host>).
    BUILD="${BUILD}-${HOST_TRIPLE}"
    BUILD_TRIPLE="${BUILD_TRIPLE:-$(uname -m)-pc-linux-gnu}"
    [[ -x "$NATIVE_PREFIX/bin/$TARGET-gcc" ]] \
        || die "Native SPU toolchain not present at $NATIVE_PREFIX/bin/$TARGET-gcc. Run without --host first to produce stage 0."
    say "Cross-build: host=$HOST_TRIPLE build=$BUILD_TRIPLE prefix=$PREFIX"
fi

# --clean acts on the build root the compiler and host options selected
# (spu, spu-gcc16, spu[-gcc16]-<host>), so it runs only after both, whatever
# the option order.
# --print-config: the selection only, for tests; nothing is removed or built.
if [[ "$PRINT_CONFIG" == 1 ]]; then
    printf 'spu_gcc=%s
gcc_ver=%s
patch_dir=%s
build=%s
prefix=%s
clean=%s
'         "$SPU_GCC" "$GCC_VER" "$GCC_PATCH_DIR" "$BUILD" "$PREFIX" "$CLEAN"
    exit 0
fi
if [[ "$CLEAN" == 1 ]]; then
    say "Cleaning $BUILD"; rm -rf "$BUILD"
fi

# See build-ppu-toolchain.sh for the rationale on the env-array form vs.
# plain CC / CXX / LDFLAGS variables.  Forcing CC/CXX is mandatory under
# CI: the workflow exports CC=ccache gcc globally so native compiles get
# cached, and configure would otherwise pick that up as the host
# compiler when --host=mingw.
CROSS_LDFLAGS_ARGS=()
if [[ -n "$HOST_TRIPLE" ]]; then
    _ccache=""
    if command -v ccache >/dev/null 2>&1; then
        _ccache="ccache "
    fi
    CROSS_LDFLAGS_ARGS=(
        "CC=${_ccache}${HOST_TRIPLE}-gcc"
        "CXX=${_ccache}${HOST_TRIPLE}-g++"
        "CC_FOR_BUILD=${_ccache}gcc"
        "CXX_FOR_BUILD=${_ccache}g++"
        # -Wl,--no-insert-timestamp zeroes the PE TimeDateStamp so the .exe/.dll
        # we ship are byte-identical run to run.  -ffile-prefix-map strips the CI
        # build tree out of the host binaries' DWARF (__FILE__ / debug info);
        # -g -O2 are kept because setting the flags otherwise replaces the
        # build's defaults.  This reaches the SPU host (mingw) toolchain links.
        "CFLAGS=-g -O2 -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
        "CXXFLAGS=-g -O2 -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
        "LDFLAGS=-static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp"
    )
    unset _ccache
fi

mkdir -p "$BUILD" "$PREFIX"

extract_source() {
    # See build-ppu-toolchain.sh's extract_source for the retry-on-RPC
    # rationale.  `git archive` can hit promisor-remote curl blips
    # mid-stream and corrupt the tar pipe; three attempts with a tag
    # refetch between each rides those out without failing CI.
    local repo="$1" tag="$2" dest="$3"
    if [[ -d "$dest" ]]; then
        say "Source $dest already extracted (skipping)"
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
    [[ ${#patch_list[@]} -eq 0 ]] && { say "No patches in $patches_dir"; return 0; }
    for p in "${patch_list[@]}"; do
        say "Applying $p to $src"
        (cd "$src" && patch -p1 --forward --fuzz=0) < "$patches_dir/$p" \
            || die "Patch $p failed (consider --clean to wipe build root and re-extract)"
    done
    touch "$stamp"
}

# GCC 10 removed the SPU backend.  --gcc=16 restores it unchanged from the
# 9.5.0 release tag (the upstream mirror bootstrap.sh fetches) before the
# patches/spu/gcc-16.x series ports it; the series assumes exactly that tree.
restore_spu_backend() {
    local src="$1" stamp="$1/.spu-backend-restored"
    [[ -f "$stamp" ]] && { say "SPU backend already restored (stamp present)"; return 0; }
    git -C "$UPSTREAM/gcc" rev-parse -q --verify "$SPU_BACKEND_TAG^{commit}" >/dev/null         || die "$SPU_BACKEND_TAG not in $UPSTREAM/gcc (run scripts/bootstrap.sh)"
    say "Restoring the SPU backend from $SPU_BACKEND_TAG into $src"
    git -C "$UPSTREAM/gcc" archive "$SPU_BACKEND_TAG"         gcc/config/spu gcc/common/config/spu libgcc/config/spu | tar -x -C "$src"         || die "could not restore the SPU backend from $SPU_BACKEND_TAG"
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

    # --enable-deterministic-archives: ar and ranlib default to D mode, so
    # rebuilds give byte-identical .a files (see build-ppu-toolchain.sh).
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
        --enable-deterministic-archives \
        --with-gcc \
        --with-gnu-as \
        --with-gnu-ld)

    say "Building binutils"
    (cd "$obj" && make -j"$JOBS")
    say "Installing binutils"
    (cd "$obj" && make install)
    touch "$obj/.installed"
}

verify_binutils() {
    if [[ -n "$HOST_TRIPLE" ]]; then
        say "Skipping assembler execution check for cross-host binaries ($HOST_TRIPLE)"
        return 0
    fi
    say "Checking installed SPU nop opcode/directive dispatch"
    python3 "$PS3_TOOLCHAIN_ROOT/tests/sdk/spu-nop-dispatch-test.py" \
        --as "$PREFIX/bin/$TARGET-as" \
        --output "$BUILD/spu-nop-dispatch-evidence"
}

build_gcc_newlib() {
    local gcc_src="$BUILD/gcc-$GCC_VER-src"
    local newlib_src="$BUILD/newlib-$NEWLIB_VER-src"
    local obj="$BUILD/gcc-$GCC_VER-build"

    extract_source "$UPSTREAM/gcc" "$GCC_TAG" "$gcc_src"
    if [[ "$SPU_GCC" == 16 ]]; then
        restore_spu_backend "$gcc_src"
    fi
    apply_patches "$gcc_src" "$PATCHES/$GCC_PATCH_DIR"

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
        --enable-multilib
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
        if [[ "$SPU_GCC" == 16 ]]; then
            # newlib 4.4 still has two implicit function declarations
            # (signal.c getpid, __dprintf.c write), errors since GCC 14;
            # keep 9.5's behaviour for building newlib itself (newlib 4.6
            # removes them).  std::stacktrace needs mmap, which SPU lacks.
            cflags_target="$cflags_target -Wno-error=implicit-function-declaration"
            conf_args+=(--disable-libstdcxx-backtrace)
        fi
        # SPU's libm/machine/spu fenv files use #include "headers/fefpscr.h"
        # relative to their source directory.  libc/machine/spu/memcpy.c pulls
        # vec_literal.h the same way — add -I so the compiler finds them.
        local target_includes="-I$newlib_src/newlib/libm/machine/spu -I$newlib_src/newlib/libc/machine/spu"
        # CXXFLAGS_FOR_TARGET mirrors CFLAGS_FOR_TARGET: libstdc++ is large
        # enough that without -mno-branch-hints the hbrr instruction's 16-bit
        # displacement overflows in functexcept.cc / system_error.cc and
        # binutils aborts.
        # Append the host-build prefix map so libgcc/newlib/libstdc++ DWARF
        # drops the CI tree.  Keeps $cflags_target and $target_includes verbatim.
        target_env=(
            CFLAGS_FOR_TARGET="$cflags_target $target_includes -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
            CXXFLAGS_FOR_TARGET="$cflags_target $target_includes -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
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
    # Not $TARGET/bin: binutils installs the native (Linux) as/ld/ar/...
    # there, and the cross build installs its own .exe files beside them.
    if [[ -d "$NATIVE_PREFIX/$TARGET" ]]; then
        mkdir -p "$PREFIX/$TARGET"
        tar -C "$NATIVE_PREFIX/$TARGET" --exclude=./bin -cf - . | tar -C "$PREFIX/$TARGET" -xf -
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
        binutils)       build_binutils; verify_binutils ;;
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
