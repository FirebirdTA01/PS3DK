#!/usr/bin/env bash
# PS3 Custom Toolchain — PPU toolchain (powerpc64-ps3-elf)
#
# Builds:
#   binutils 2.42  -> $PS3DEV/ppu
#   GCC 16.1.0     -> $PS3DEV/ppu (stage 1 + newlib + stage 2; patches/ppu/gcc-16.x)
#   newlib 4.4.0   -> as part of combined GCC build
#   GDB 14.2       -> $PS3DEV/ppu
#
# Source extraction is via `git archive` from the mirrors cloned by bootstrap.sh.
# Patches are applied from patches/ppu/<component>/ in the order listed in
# patches/ppu/<component>/series (or, if no series file exists, sorted by name).
#
# Re-runnable: delete $PS3_BUILD_ROOT/ppu-gcc16 to force a clean rebuild, or run with
# --clean to do it for you.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"
# shellcheck disable=SC1091
source "$script_dir/ppu-thread-model.sh"
# The compiler adds the SDK include directory by default (gcc-16.x patch 0068); building the
# toolchain, the runtime or the SDK itself must never see an installed SDK's headers.
export PS3DK_NO_DEFAULT_INCLUDE=1

say() { printf "[ppu] %s\n" "$*"; }
warn() { printf "[ppu] WARNING: %s\n" "$*" >&2; }
die() { printf "[ppu] ERROR: %s\n" "$*" >&2; exit 1; }

# Component versions — bump these if we re-target.
BINUTILS_TAG="binutils-2_42"
BINUTILS_VER="2.42"
GCC_TAG="releases/gcc-16.1.0"
GCC_VER="16.1.0"
GCC_PATCH_DIR="gcc-16.x"
NEWLIB_TAG="newlib-4.4.0"
NEWLIB_VER="4.4.0"
GDB_TAG="gdb-14.2-release"
GDB_VER="14.2"

TARGET="powerpc64-ps3-elf"
PREFIX="$PS3DEV/ppu"

UPSTREAM="$PS3_TOOLCHAIN_ROOT/src/upstream"
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/ppu"
# ppu-gcc16, not ppu: a build tree left by an earlier GCC 12.4 toolchain is never reused.
BUILD="$PS3_BUILD_ROOT/ppu-gcc16"

JOBS="$(nproc 2>/dev/null || echo 4)"
say "Using $JOBS parallel jobs."

# CLI: optional --clean wipes build dir; --only <step> runs only that step.
# Steps: binutils | gcc-newlib | gdb | symlinks (default: all in order).
#
# GCC 16.1.0 is the only PPU compiler.  A prefix that already holds another
# GCC major version (an earlier 12.4 install) is refused, so point PS3DEV at a
# fresh prefix.  --gcc=16 is still accepted and changes nothing; any other
# --gcc value is refused.  --print-config prints the resolved configuration
# and exits without building.
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
CLEAN=""
PRINT_CONFIG=""
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --gcc=16) ;;
        --gcc=*) die "$arg: GCC 16.1.0 is the only PPU compiler (the GCC 12.4 path was removed)" ;;
        --print-config) PRINT_CONFIG=1 ;;
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
    BUILD="${BUILD}-${HOST_TRIPLE}"
    BUILD_TRIPLE="${BUILD_TRIPLE:-$(uname -m)-pc-linux-gnu}"
    [[ -x "$NATIVE_PREFIX/bin/$TARGET-gcc" ]] \
        || die "Native PPU toolchain not present at $NATIVE_PREFIX/bin/$TARGET-gcc. Run without --host first to produce stage 0."
    say "Cross-build: host=$HOST_TRIPLE build=$BUILD_TRIPLE prefix=$PREFIX"
fi

# require_gcc_major <prefix> <role>: an installed $TARGET-gcc under <prefix>
# must report the selected major version.  A compiler that cannot be run or
# answers with something other than a version is refused too: an existing
# installation of unknown version is not an empty prefix.
require_gcc_major() {
    local gcc="$1/bin/$TARGET-gcc" ver rc
    [[ -e "$gcc" || -L "$gcc" ]] || return 0
    rc=0
    ver="$("$gcc" -dumpfullversion 2>/dev/null)" || rc=$?
    [[ $rc -eq 0 && "$ver" =~ ^[0-9]+(\.[0-9]+)*$ ]] \
        || die "$2 $gcc did not report a GCC version (exit $rc, output '$ver'); refusing to build GCC $GCC_VER there."
    [[ "${ver%%.*}" == "${GCC_VER%%.*}" ]] \
        || die "$2 $1 holds GCC $ver; refusing to build GCC $GCC_VER with it. Point PS3DEV at a separate prefix."
}

# require_cross_prefix_major <prefix>: a Windows-hosted (--host) prefix holds
# .exe files this Linux build host cannot run, so its GCC major comes from the
# install metadata this script writes there (<prefix>/.ps3dk-gcc-version,
# before installing the compiler).  Fail closed: a prefix holding a compiler
# (gcc or gcc.exe) without readable, well-formed metadata is of unknown version
# and refused, as is one whose metadata names another major.  A prefix with no
# compiler (empty, or binutils only) is accepted.
require_cross_prefix_major() {
    local p=$1 meta="$1/.ps3dk-gcc-version" gcc="$1/bin/$TARGET-gcc" ver=""
    if [[ ! -e "$gcc" && ! -L "$gcc" && ! -e "$gcc.exe" && ! -L "$gcc.exe" ]]; then
        return 0
    fi
    [[ -e "$meta" ]] \
        || die "cross prefix $p holds a compiler but no $meta (unknown GCC version); refusing to build GCC $GCC_VER there. Remove that prefix or point PS3DEV elsewhere."
    { [[ -r "$meta" ]] && ver=$(head -n 1 "$meta"); } 2>/dev/null \
        || die "cross prefix $p: $meta cannot be read; refusing to build GCC $GCC_VER there."
    [[ "$ver" =~ ^[0-9]+(\.[0-9]+)*$ ]] \
        || die "cross prefix $p: $meta holds '$ver', not a GCC version; refusing to build GCC $GCC_VER there."
    [[ "${ver%%.*}" == "${GCC_VER%%.*}" ]] \
        || die "cross prefix $p holds GCC $ver (per $meta); refusing to build GCC $GCC_VER with it. Point PS3DEV at a separate prefix."
}

# Never build GCC 16 over another major version (an earlier 12.4 install).  A cross build also copies the native prefix's
# target libraries into its package, so the native compiler must match too.
if [[ -n "$HOST_TRIPLE" ]]; then
    require_cross_prefix_major "$PREFIX"
    require_gcc_major "$NATIVE_PREFIX" "native prefix"
else
    require_gcc_major "$PREFIX" "prefix"
fi

if [[ -n "$PRINT_CONFIG" ]]; then
    for kv in "gcc_tag=$GCC_TAG" "gcc_ver=$GCC_VER" "gcc_patches=$PATCHES/$GCC_PATCH_DIR" \
              "prefix=$PREFIX" "build=$BUILD"; do
        echo "$kv"
    done
    exit 0
fi

if [[ -n "$CLEAN" ]]; then
    say "Cleaning $BUILD"
    rm -rf "$BUILD"
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
        # -Wl,--no-insert-timestamp zeroes the PE TimeDateStamp so the .exe/.dll
        # we ship are byte-identical run to run.  -ffile-prefix-map strips the CI
        # build tree out of the host binaries' DWARF (__FILE__ / debug info);
        # -g -O2 are kept because setting the flags otherwise replaces the
        # build's default flags.  CXX carries no -std here: only -std=gnu++11
        # (the libcody guard) must stay out of this shared array, since binutils
        # and GDB build their C++ without it.  This reaches the binutils, GCC
        # and GDB host links alike.
        "CFLAGS=-g -O2 -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
        "CXXFLAGS=-g -O2 -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
        "LDFLAGS=-static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp"
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
        (cd "$src" && patch -p1 --forward --fuzz=0) < "$patches_dir/$p" \
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

    # --enable-deterministic-archives: ar and ranlib default to D mode
    # (member mtime, uid and gid 0, mode 644; symbol table dated 0), so two
    # builds of the same commit give byte-identical .a files.  Every archive
    # the SDK installs is written by this ar (GCC/newlib target libraries,
    # the sdk/ Makefiles, nidgen, CMake, portlibs), so none needs its own D.
    # tests/sdk/deterministic-archives-test.sh judges the installed tree.
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
        --enable-deterministic-archives \
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
# 2. GCC 16.1.0 + newlib 4.4.0 (combined-tree build)
#
#    The combined-tree pattern symlinks newlib/libgloss into the GCC source tree
#    so a single build produces libgcc, newlib, libstdc++ in one pass. This is
#    the canonical approach for embedded cross-toolchains.
# -----------------------------------------------------------------------------
build_gcc_newlib() {
    local gcc_src="$BUILD/gcc-$GCC_VER-src"
    local newlib_src="$BUILD/newlib-$NEWLIB_VER-src"
    local obj="$BUILD/gcc-$GCC_VER-build"

    # Thread model: libstdc++/libgcc are built with the posix gthread layer
    # over librt's pthreads (newlib patch 0018 declares them for the
    # toolchain build; gcc-16.x 0069-0071).  PS3_GCC_THREADS=single
    # overrides it.  The model changes
    # libstdc++'s ABI (shared_ptr's lock policy, guards), so a build tree or a
    # stage made for another model is never reused: the tree is rebuilt from
    # scratch, and the model is stamped into both.
    local threads_mode="${PS3_GCC_THREADS:-$(ppu_threads_default "${GCC_VER%%.*}")}"
    local native_for_model=""
    [[ -n "$HOST_TRIPLE" ]] && native_for_model="$NATIVE_PREFIX"
    check_gcc_thread_model() {
        case "$(ppu_stage_model_decision "$threads_mode" "$obj" "$PREFIX" "$native_for_model")" in
            native)
                die "native stage $NATIVE_PREFIX has thread model '$(ppu_stamp "$NATIVE_PREFIX/.ps3dk-thread-model")', this build wants '$threads_mode': rebuild the native toolchain first" ;;
            rebuild)
                say "GCC build tree is for thread model '$(ppu_stamp "$obj/.threads-model")', this build wants '$threads_mode': removing it"
                rm -rf "$obj" ;;
            reinstall)
                say "installed prefix $PREFIX has thread model '$(ppu_stamp "$PREFIX/.ps3dk-thread-model")', this build wants '$threads_mode': building and installing again"
                rm -f "$obj/.installed" ;;
        esac
    }
    check_gcc_thread_model

    extract_source "$UPSTREAM/gcc" "$GCC_TAG" "$gcc_src"
    apply_patches "$gcc_src" "$PATCHES/$GCC_PATCH_DIR"

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
        newer_patch=$(find "$PATCHES/$GCC_PATCH_DIR" "$PATCHES/newlib-4.x" \
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

    # The skip trusts the build tree only if the installed prefix (and a --host build's native source) still
    # carries the same thread model: a prefix replaced since must not be skipped over.
    check_gcc_thread_model
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
        "$(ppu_gcc_threads_arg "$threads_mode")"
    )

    # No host -std pin: GCC 16's own code needs C++14 while its libcody
    # still requires exactly C++11, and each configure adds the -std it
    # needs unless CXXFLAGS overrides it, so forcing one breaks either.
    local target_env=()
    local target_make_env=()
    # Extra host flags for the Windows cross build only.  Set LAST on the
    # configure env line so it wins over target_env's CXXFLAGS and the shared
    # CROSS_LDFLAGS_ARGS CFLAGS, without disturbing their order.
    local gcc_host_cxx=()

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
        target_env+=(
            CC_FOR_BUILD=gcc
            CXX_FOR_BUILD=g++
        )
        # The prefix map without a -std (as in the native build).  Appended
        # LAST on the env line so it wins over CROSS_LDFLAGS_ARGS' CXXFLAGS,
        # without touching their order.
        gcc_host_cxx=(
            "CXXFLAGS=-g -O2 -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
        )
    else
        conf_args+=(--with-system-zlib)
        # Native combined-tree: build libgcc, newlib, libstdc++ in one pass,
        # with the thread model chosen above.
        conf_args+=(
            --with-newlib
            --enable-newlib-io-long-long
            --enable-newlib-io-c99-formats
        )
        # The in-tree target libs (libgcc / libstdc++ / newlib) bake the CI
        # host build tree into their DWARF, so map it out.  Setting FOR_TARGET
        # otherwise drops GCC's default -g -O2 target flags, so reset them and
        # append the prefix map.
        target_env+=(
            "CFLAGS_FOR_TARGET=-g -O2 -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
            "CXXFLAGS_FOR_TARGET=-g -O2 -ffile-prefix-map=$PS3_BUILD_ROOT=/ps3dk-build"
        )
        if [[ "$threads_mode" == posix ]]; then
            # librt has real rwlocks, but std::shared_mutex starts on libstdc++'s
            # mutex + condition_variable implementation; answering its
            # pthread_rwlock_t check "no" selects that (switching needs its own
            # rows).
            # The target libraries are configured during `make`, so the answer
            # goes into make's environment, not only the top-level configure's.
            target_make_env+=("ac_cv_type_pthread_rwlock_t=no")
        fi
    fi

    (cd "$obj" && \
        env "${target_env[@]}" "${CROSS_LDFLAGS_ARGS[@]}" "${gcc_host_cxx[@]}" \
        "$gcc_src/configure" "${conf_args[@]}")
    [[ "$(ppu_configured_threads "$obj")" == "$threads_mode" ]]         || die "GCC was configured with thread model '$(ppu_configured_threads "$obj")', this build wants '$threads_mode'"

    if [[ -n "$HOST_TRIPLE" ]]; then
        say "Building GCC host-only (~15-30 minutes)"
        (cd "$obj" && make -j"$JOBS" all-host)
        say "Installing GCC host-only"
        # The cross prefix's version metadata (require_cross_prefix_major) is
        # written first, so an interrupted install still names its major.
        mkdir -p "$PREFIX"
        echo "$GCC_VER" > "$PREFIX/.ps3dk-gcc-version"
        (cd "$obj" && make install-host)
        copy_target_libs_from_native "$threads_mode"
    else
        say "Building GCC+newlib (this takes a while — ~30-90 minutes)"
        (cd "$obj" && env "${target_make_env[@]}" make -j"$JOBS" all)
        say "Installing GCC+newlib"
        (cd "$obj" && make install)
        local driver_model
        driver_model=$("$PREFIX/bin/$TARGET-gcc" -v 2>&1 | sed -n 's/^Thread model: //p')
        [[ "$driver_model" == "$threads_mode" ]]             || die "installed driver reports thread model '$driver_model', this build wants '$threads_mode'"
        echo "$threads_mode" > "$PREFIX/.ps3dk-thread-model"
    fi
    echo "$threads_mode" > "$obj/.threads-model"
    touch "$obj/.installed"
}

# When cross-building, target-library trees (newlib, libgcc, libstdc++) come
# from the native install — they're host-agnostic PowerPC ELF.  Copy them
# into the Windows-hosted prefix so the resulting toolchain ships with a
# complete sysroot.
copy_target_libs_from_native() {
    [[ -n "$HOST_TRIPLE" ]] || return 0
    say "Copying target libraries: $NATIVE_PREFIX -> $PREFIX"

    # The target libraries carry the native build's thread model; refuse a
    # native stage made for a different one (or not stamped at all).
    local want_model=$1
    local native_model
    native_model=$(ppu_stamp "$NATIVE_PREFIX/.ps3dk-thread-model")
    [[ "$native_model" == "$want_model" ]] \
        || die "native stage $NATIVE_PREFIX has thread model '$native_model', this build wants '$want_model': rebuild the native toolchain first"
    cp "$NATIVE_PREFIX/.ps3dk-thread-model" "$PREFIX/.ps3dk-thread-model"

    # Target sysroot ($PREFIX/$TARGET/{lib,include,sys-include,...}), but
    # not $TARGET/bin: binutils installs the native (Linux) as/ld/ar/...
    # there, and the cross build installs its own .exe files beside them.
    if [[ -d "$NATIVE_PREFIX/$TARGET" ]]; then
        mkdir -p "$PREFIX/$TARGET"
        tar -C "$NATIVE_PREFIX/$TARGET" --exclude=./bin -cf - . | tar -C "$PREFIX/$TARGET" -xf -
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
