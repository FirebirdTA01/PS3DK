#!/usr/bin/env bash
# PS3 Custom Toolchain — PPU toolchain (powerpc64-ps3-elf)
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
#
# Cross-build mode (Windows-hosted toolchain):
#   --host=x86_64-w64-mingw32   produce .exe binaries that run on Windows.
#   --build=<triple>            (optional) override the build triple; default
#                               $(uname -m)-pc-linux-gnu.
# Cross-build requires the native PPU toolchain to already be installed
# (stage 0): target libraries (libgcc, libstdc++, newlib) are reused from
# the native tree rather than rebuilt, so the cross-compiler ships with
# host-agnostic PowerPC ELF libs sourced from the native build.
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
        binutils|gcc-newlib|gdb|symlinks) ONLY="$arg" ;;
    esac
done

# When cross-building, install into a parallel prefix and use a separate
# build directory so the native install tree is untouched and reusable.
NATIVE_PREFIX="$PREFIX"
if [[ -n "$HOST_TRIPLE" ]]; then
    PREFIX="${PS3DEV}.win/ppu"
    BUILD="$PS3_BUILD_ROOT/ppu-${HOST_TRIPLE}"
    BUILD_TRIPLE="${BUILD_TRIPLE:-$(uname -m)-pc-linux-gnu}"
    [[ -x "$NATIVE_PREFIX/bin/$TARGET-gcc" ]] \
        || die "Native PPU toolchain not present at $NATIVE_PREFIX/bin/$TARGET-gcc. Run without --host first to produce stage 0."
    say "Cross-build: host=$HOST_TRIPLE build=$BUILD_TRIPLE prefix=$PREFIX"
fi

# Cross-build env injection: passed to `env(1)` before configure so each
# `VAR=value` element survives wordsplitting (LDFLAGS contains spaces).
# Empty for native builds — don't clobber distro-supplied LDFLAGS in
# the user's env.
#
# For cross-builds we MUST override CC/CXX explicitly: when CI sets a
# global `CC=ccache gcc` (so native object compiles benefit from ccache),
# configure honors that env var and probes the *build* compiler as the
# *host* compiler — binutils' `checking for ${HOST}-gcc... ccache gcc`
# log line, followed by "C compiler cannot create executables".  Forcing
# CC/CXX to the cross-compiler restores the autoconf default
# (`AC_CHECK_TOOL(CC, ${host_alias}-gcc)`) while still letting ccache
# wrap the actual mingw invocations.  CC_FOR_BUILD points back at the
# build-machine native gcc so configure's own internal probes (e.g.
# the libsframe build-system compile tests) keep working.
#
# Static linkage: -static-libgcc / -static-libstdc++ removes the .exe's
# runtime DLL dependency on libgcc_s_seh-1.dll / libstdc++-6.dll.
# -static does the same for winpthread when GCC was built with
# --enable-threads=posix.
CROSS_LDFLAGS_ARGS=()
if [[ -n "$HOST_TRIPLE" ]]; then
    # ccache is optional — use it when on PATH, drop it cleanly otherwise.
    _ccache=""
    if command -v ccache >/dev/null 2>&1; then
        _ccache="ccache "
    fi
    CROSS_LDFLAGS_ARGS=(
        "CC=${_ccache}${HOST_TRIPLE}-gcc"
        "CXX=${_ccache}${HOST_TRIPLE}-g++"
        "CC_FOR_BUILD=${_ccache}gcc"
        "CXX_FOR_BUILD=${_ccache}g++"
        "LDFLAGS=-static -static-libgcc -static-libstdc++"
    )
    unset _ccache
fi

mkdir -p "$BUILD" "$PREFIX"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

# Extract a pinned tag from a git mirror into a fresh source tree.
# Retries on transient failure: `git archive` against a sometimes-online
# promisor remote can hit RPC blips ("curl 18 transfer closed") that
# truncate the tar stream and leave $dest half-populated.  Three
# attempts with `git fetch --refetch` between each give us coverage
# against momentary network glitches without the script just dying.
extract_source() {
    local repo="$1" tag="$2" dest="$3"
    if [[ -d "$dest" ]]; then
        say "Source $dest already extracted (skipping — delete to re-extract)"
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

# Apply patches from a directory. Honors a 'series' file if present (quilt-style).
# Idempotent via a per-patches-dir stamp: once all patches succeed, a stamp file
# is created at $src/.patches-applied-<patches_dir basename>; subsequent runs
# detect the stamp and short-circuit.  If a run is interrupted between patches,
# the source tree is left in a partial state — recover with `--clean` and re-run.
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

    extract_source "$UPSTREAM/gcc" "$GCC_TAG" "$gcc_src"
    apply_patches "$gcc_src" "$PATCHES/gcc-12.x"

    # Newlib is only built in the native (stage 0) flow.  Cross-builds reuse
    # the PowerPC ELF target libraries from the already-installed native
    # toolchain — they're host-agnostic.
    if [[ -z "$HOST_TRIPLE" ]]; then
        extract_source "$UPSTREAM/newlib-cygwin" "$NEWLIB_TAG" "$newlib_src"
        apply_patches "$newlib_src" "$PATCHES/newlib-4.x"

        # Regenerate newlib's configure + Makefile.in from the patched
        # configure.ac / acinclude.m4 / Makefile.am / Makefile.inc set.  Patch 0001
        # adds an `lv2` sys_dir conditional and patch 0004 ships the corresponding
        # libc/sys/lv2/ tree; neither takes effect unless autoreconf rewrites the
        # pregenerated 5k-line configure and 40k-line Makefile.in.  Patch 0008
        # relaxes override.m4's strict autoconf-2.69 pin so modern hosts can do this.
        if [[ ! -f "$newlib_src/newlib/.autoreconf-stamp" ]]; then
            say "Regenerating newlib autotools (autoreconf -fi)"
            (cd "$newlib_src/newlib" && autoreconf -fi)
            touch "$newlib_src/newlib/.autoreconf-stamp"
        fi
    fi

    # GCC prereqs: GMP, MPFR, MPC, ISL. Use contrib/download_prerequisites which
    # drops them as in-tree symlinks. Requires network access.
    if [[ ! -d "$gcc_src/gmp" ]]; then
        say "Downloading GCC prerequisites (GMP, MPFR, MPC, ISL)"
        (cd "$gcc_src" && ./contrib/download_prerequisites)
    fi

    # Symlink newlib + libgloss into the GCC tree for the native combined build.
    # Cross-build doesn't compile target libs, so the symlinks are irrelevant.
    if [[ -z "$HOST_TRIPLE" && ! -e "$gcc_src/newlib" ]]; then
        ln -sf "$newlib_src/newlib"   "$gcc_src/newlib"
        ln -sf "$newlib_src/libgloss" "$gcc_src/libgloss"
    fi

    # If any GCC or newlib patch is newer than $obj/.installed, target
    # libraries (libgcc / newlib / libstdc++ / libgloss) compiled before the
    # patch are silently ABI-stale.  Patch 0021 (rs6000 output_toc Pmode)
    # is the canonical instance: pre-patch libgloss had 8-byte LP64 TOC
    # slots, post-patch lwz @toc@l reads the wrong half.  Invalidate the
    # stamp so make rebuilds the target libs.
    if [[ -f "$obj/.installed" ]]; then
        local newer_patch
        newer_patch=$(find "$PATCHES/gcc-12.x" "$PATCHES/newlib-4.x" \
            -name "*.patch" -newer "$obj/.installed" 2>/dev/null | head -1)
        if [[ -n "$newer_patch" ]]; then
            say "Patch newer than .installed — invalidating to rebuild target libs ($newer_patch)"
            rm -f "$obj/.installed"
            # Also wipe target build trees so make actually re-compiles them.
            rm -rf "$obj/$TARGET/libgloss" "$obj/$TARGET/lp64/libgloss" \
                   "$obj/$TARGET/newlib"   "$obj/$TARGET/lp64/newlib"   \
                   "$obj/$TARGET/libstdc++-v3" "$obj/$TARGET/lp64/libstdc++-v3"
        fi
    fi

    if [[ -f "$obj/.installed" ]]; then
        say "GCC already built and installed (skipping)"
        return 0
    fi

    mkdir -p "$obj"
    say "Configuring GCC -> $PREFIX (target=$TARGET, GCC $GCC_VER${HOST_TRIPLE:+, host=$HOST_TRIPLE})"

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
        --enable-multilib
        --disable-nls
        --disable-shared
        --disable-win32-registry
    )

    local target_env=()
    if [[ -n "$HOST_TRIPLE" ]]; then
        # Cross-build: host binaries only, no target libs.  --disable-bootstrap
        # is mandatory when --host != --build — the standard 3-stage GCC
        # bootstrap can't run a Windows .exe on Linux.  Use GCC's bundled zlib
        # (drop --with-system-zlib) so we don't need mingw-w64 zlib headers.
        conf_args+=(
            --host="$HOST_TRIPLE"
            --build="$BUILD_TRIPLE"
            --disable-bootstrap
        )
        # CC_FOR_BUILD/CXX_FOR_BUILD identify the build-host compiler so
        # auxiliary tools like genmddeps compile with the local gcc, not the
        # mingw cross compiler.
        target_env=(
            CC_FOR_BUILD=gcc
            CXX_FOR_BUILD=g++
        )
    else
        conf_args+=(--with-system-zlib)
        # Native combined-tree: build libgcc, newlib, libstdc++ in one pass.
        # Threading strategy: start with --enable-threads=single.  A follow-up
        # swaps in --enable-threads=posix once the libsysbase gthr shim lands.
        local threads_mode="${PS3_GCC_THREADS:-single}"
        conf_args+=(
            --with-newlib
            --enable-threads="$threads_mode"
            --enable-newlib-io-long-long
            --enable-newlib-io-c99-formats
        )
    fi

    (cd "$obj" && \
        env "${target_env[@]}" "${CROSS_LDFLAGS_ARGS[@]}" \
        "$gcc_src/configure" "${conf_args[@]}")

    if [[ -n "$HOST_TRIPLE" ]]; then
        say "Building GCC host-only (~15-30 minutes)"
        (cd "$obj" && make -j"$JOBS" all-host)
        say "Installing GCC host-only"
        (cd "$obj" && make install-host)
        copy_target_libs_from_native
    else
        say "Building GCC+newlib (this takes a while — ~30-90 minutes)"
        (cd "$obj" && make -j"$JOBS" all)
        say "Installing GCC+newlib"
        (cd "$obj" && make install)
    fi
    touch "$obj/.installed"
}

# When cross-building, target-library trees (newlib, libgcc, libstdc++) come
# from the native install — they're host-agnostic PowerPC ELF.  Copy them
# into the Windows-hosted prefix so the resulting toolchain ships with a
# complete sysroot.
copy_target_libs_from_native() {
    [[ -n "$HOST_TRIPLE" ]] || return 0
    say "Copying target libraries: $NATIVE_PREFIX -> $PREFIX"

    # Target sysroot ($PREFIX/$TARGET/{lib,include,sys-include,...}).
    if [[ -d "$NATIVE_PREFIX/$TARGET" ]]; then
        mkdir -p "$PREFIX/$TARGET"
        cp -a "$NATIVE_PREFIX/$TARGET/." "$PREFIX/$TARGET/"
    fi

    # libgcc lives under lib/gcc/$TARGET/$GCC_VER/.
    if [[ -d "$NATIVE_PREFIX/lib/gcc/$TARGET" ]]; then
        mkdir -p "$PREFIX/lib/gcc/$TARGET"
        cp -a "$NATIVE_PREFIX/lib/gcc/$TARGET/." "$PREFIX/lib/gcc/$TARGET/"
    fi
}

# -----------------------------------------------------------------------------
# 3. GDB 14.2
# -----------------------------------------------------------------------------
build_gdb() {
    if [[ -n "$HOST_TRIPLE" ]]; then
        # GDB 14 requires GMP and MPFR for its expression evaluator.  Ubuntu
        # doesn't ship mingw-w64 builds of those, so we'd have to add a
        # separate prerequisites build for the cross GDB.  Defer until
        # there's demand: Windows users can debug via WSL-hosted gdb or
        # RPCS3's built-in debugger.
        say "Skipping GDB cross-build (host=$HOST_TRIPLE) — mingw-w64 GMP/MPFR not yet packaged; deferred."
        return 0
    fi

    local src="$BUILD/gdb-$GDB_VER-src"
    local obj="$BUILD/gdb-$GDB_VER-build"

    extract_source "$UPSTREAM/binutils-gdb" "$GDB_TAG" "$src"
    apply_patches "$src" "$PATCHES/gdb-14.x"

    if [[ -f "$obj/.installed" ]]; then
        say "GDB already built and installed (skipping)"
        return 0
    fi

    mkdir -p "$obj"
    say "Configuring GDB -> $PREFIX (target=$TARGET${HOST_TRIPLE:+, host=$HOST_TRIPLE})"

    local cross_args=()
    local python_arg="--with-python=yes"
    local readline_arg="--with-system-readline"
    if [[ -n "$HOST_TRIPLE" ]]; then
        cross_args+=(--host="$HOST_TRIPLE" --build="$BUILD_TRIPLE")
        # Cross-built GDB skips Python integration: configure runs python-config
        # against the build host, which produces Linux libpython refs the
        # Windows .exe can't link against.  Re-add via a Windows libpython
        # bundled in a follow-up.
        python_arg="--without-python"
        # mingw-w64 ships its own readline; let GDB use it via curses.
        readline_arg=""
    fi

    (cd "$obj" && \
        env "${CROSS_LDFLAGS_ARGS[@]}" \
        "$src/configure" \
        --prefix="$PREFIX" \
        --target="$TARGET" \
        "${cross_args[@]}" \
        --disable-nls \
        --disable-werror \
        --disable-dependency-tracking \
        --disable-sim \
        --disable-gdbserver \
        "$python_arg" \
        ${readline_arg:+"$readline_arg"})

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
    if [[ -n "$HOST_TRIPLE" ]]; then
        # Windows-hosted toolchain: NTFS symlinks need elevated perms and the
        # short ppu-* aliases are a Linux convenience.  Users invoke
        # powerpc64-ps3-elf-gcc.exe directly on Windows.
        say "Skipping ppu-* alias creation (cross-build target=$HOST_TRIPLE)"
        return 0
    fi
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
say "=== PPU toolchain ==="
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
