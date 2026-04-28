#!/usr/bin/env bash
# PS3 Custom Toolchain — cross-compile Windows host tools.
#
# Cross-compiles the host-side tools the sample-build pipeline needs into
# x86_64-w64-mingw32 PE32+ binaries, plus the support libraries (zlib, GMP,
# OpenSSL) those tools link against.  Output stages under
# $PS3_TOOLCHAIN_ROOT/stage/host-tools-windows/bin/, ready to be merged into
# the Windows release zip by package-windows-release.sh.
#
# Tools cross-compiled here:
#   * Rust workspace      — nidgen.exe, abi-verify.exe, coverage-report.exe
#   * rsx-cg-compiler.exe — clean-room Cg→NV40 shader compiler (CMake)
#   * make_self.exe       — CEX-signed SELF builder (gmp + openssl + zlib)
#   * make_self_npdrm.exe — NPDRM-signed SELF builder (same libs)
#   * package_finalize.exe — npdrm pkg finalisation (same libs)
#   * fself.exe           — fake-signed SELF builder (zlib only)
#   * bin2s.exe           — PSL1GHT binary-to-assembly helper
#   * cgcomp.exe          — legacy PSL1GHT Cg shader compiler
#
# Tools we skip locally:
#   * sprxlinker.exe — needs libelf cross.  Mingw-w64 doesn't ship libelf
#     in upstream Arch / Ubuntu repos, and bringing libelf along clean-room
#     is non-trivial.  CI's build-host-tools-windows job builds sprxlinker
#     under MSYS2 (which has mingw-w64-libelf as a package) and we merge
#     its output via package-windows-release.sh's --tools-zip flag.  Local
#     runs of this script omit sprxlinker; the resulting Windows zip will
#     warn about a missing sprxlinker.exe at package time.
#
# Cached intermediates live under $PS3_BUILD_ROOT/host-tools-windows/.
# Re-runs are incremental: each lib has a stamp file and skips when present.
# Pass --clean to wipe the cache.
#
# Usage:
#   source ./scripts/env.sh
#   ./scripts/build-host-tools-windows.sh

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say()  { printf "[host-tools-win] %s\n" "$*"; }
warn() { printf "[host-tools-win] WARNING: %s\n" "$*" >&2; }
die()  { printf "[host-tools-win] ERROR: %s\n" "$*" >&2; exit 1; }

HOST_TRIPLE="x86_64-w64-mingw32"
JOBS="$(nproc 2>/dev/null || echo 4)"

# Cross-compile root: where mingw libraries land.  Kept under the build root
# (gitignored) so it survives across `git clean -fdx` of the source tree.
DEPS_ROOT="$PS3_BUILD_ROOT/host-tools-windows/deps-${HOST_TRIPLE}"
SRC_ROOT="$PS3_BUILD_ROOT/host-tools-windows/src"

# Final stage: where the .exe + .py + asset files for the Windows release land.
STAGE_ROOT="$PS3_TOOLCHAIN_ROOT/stage/host-tools-windows"
STAGE_BIN="$STAGE_ROOT/bin"

CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --help|-h)
            sed -n '2,40p' "$0"
            exit 0
            ;;
        *) die "Unknown argument: $arg (use --help)" ;;
    esac
done

if [[ "$CLEAN" -eq 1 ]]; then
    say "Cleaning $DEPS_ROOT and $STAGE_ROOT"
    rm -rf "$DEPS_ROOT" "$STAGE_ROOT"
fi

mkdir -p "$DEPS_ROOT" "$SRC_ROOT" "$STAGE_BIN"

# Verify the cross compiler resolves before doing anything.
command -v "$HOST_TRIPLE-gcc" >/dev/null \
    || die "$HOST_TRIPLE-gcc not on PATH — install mingw-w64-gcc (see README 'Optional: cross-build the Windows-hosted toolchain release')"
say "Using $HOST_TRIPLE-gcc: $(command -v "$HOST_TRIPLE-gcc")"

# -----------------------------------------------------------------------------
# Source tarballs.  Pinned versions; SHA256s verified after download.
# -----------------------------------------------------------------------------
ZLIB_VER="1.3.1"
ZLIB_SHA256="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
ZLIB_URLS=(
    "https://zlib.net/zlib-${ZLIB_VER}.tar.gz"
    "https://zlib.net/fossils/zlib-${ZLIB_VER}.tar.gz"
    "https://github.com/madler/zlib/releases/download/v${ZLIB_VER}/zlib-${ZLIB_VER}.tar.gz"
)

GMP_VER="6.3.0"
GMP_SHA256="a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898"
GMP_URLS=(
    "https://gmplib.org/download/gmp/gmp-${GMP_VER}.tar.xz"
    "https://ftp.gnu.org/gnu/gmp/gmp-${GMP_VER}.tar.xz"
)

# OpenSSL 3.0.x is a long-term support line and the lowest-friction line to
# cross-compile to mingw — newer 3.x branches occasionally ship configure
# changes that break mingw cross-builds.  3.0.x receives security backports
# until 2026-09 per the OpenSSL release strategy.
OPENSSL_VER="3.0.16"
OPENSSL_SHA256="57e03c50feab5d31b152af2b764f10379aecd8ee92f16c985983ce4a99f7ef86"
OPENSSL_URLS=(
    "https://www.openssl.org/source/openssl-${OPENSSL_VER}.tar.gz"
    "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"
)

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

fetch() {
    local tarball="$1"; shift
    if [[ -f "$SRC_ROOT/$tarball" ]]; then
        return 0
    fi
    for url in "$@"; do
        say "Fetching $url"
        if wget --quiet --continue -O "$SRC_ROOT/$tarball" "$url"; then
            return 0
        fi
        rm -f "$SRC_ROOT/$tarball"
    done
    die "All mirrors failed for $tarball"
}

verify_sha256() {
    local tarball="$1" expected="$2"
    local actual
    actual="$(sha256sum "$SRC_ROOT/$tarball" | awk '{print $1}')"
    [[ "$actual" == "$expected" ]] \
        || die "SHA256 mismatch for $tarball: expected $expected, got $actual"
}

extract_once() {
    local tarball="$1" dirname="$2"
    if [[ -d "$SRC_ROOT/$dirname" ]]; then
        return 0
    fi
    say "Extracting $tarball -> $SRC_ROOT/$dirname"
    tar -xf "$SRC_ROOT/$tarball" -C "$SRC_ROOT"
}

ensure_psl1ght_source() {
    local psl1ght_src="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT"
    if [[ -f "$psl1ght_src/tools/generic/bin2s.c" && -d "$psl1ght_src/tools/cgcomp/source" ]]; then
        return 0
    fi

    if [[ -e "$psl1ght_src" && ! -d "$psl1ght_src/.git" ]]; then
        die "PSL1GHT source at $psl1ght_src is incomplete and is not a git checkout. Run scripts/bootstrap.sh or remove that directory and re-run."
    fi

    command -v git >/dev/null \
        || die "git not on PATH and PSL1GHT source is missing. Run scripts/bootstrap.sh first."

    mkdir -p "$(dirname "$psl1ght_src")"
    if [[ -d "$psl1ght_src/.git" ]]; then
        say "Updating PSL1GHT source for legacy host tools"
        git -C "$psl1ght_src" pull --ff-only --depth 1
    else
        say "Fetching PSL1GHT source for legacy host tools"
        git clone --depth 1 https://github.com/ps3dev/PSL1GHT.git "$psl1ght_src"
    fi

    [[ -f "$psl1ght_src/tools/generic/bin2s.c" && -d "$psl1ght_src/tools/cgcomp/source" ]] \
        || die "PSL1GHT legacy host-tool sources missing after fetch"
}

# -----------------------------------------------------------------------------
# 1. zlib (static, mingw)
# -----------------------------------------------------------------------------
build_zlib() {
    local stamp="$DEPS_ROOT/.zlib-${ZLIB_VER}-stamp"
    [[ -f "$stamp" ]] && { say "zlib already built"; return 0; }

    fetch "zlib-${ZLIB_VER}.tar.gz" "${ZLIB_URLS[@]}"
    verify_sha256 "zlib-${ZLIB_VER}.tar.gz" "$ZLIB_SHA256"
    extract_once "zlib-${ZLIB_VER}.tar.gz" "zlib-${ZLIB_VER}"

    say "Building zlib for $HOST_TRIPLE"
    (cd "$SRC_ROOT/zlib-${ZLIB_VER}" && \
        CC="$HOST_TRIPLE-gcc" AR="$HOST_TRIPLE-ar" RANLIB="$HOST_TRIPLE-ranlib" \
        ./configure --prefix="$DEPS_ROOT" --static && \
        make -j"$JOBS" libz.a && \
        make install)
    touch "$stamp"
}

# -----------------------------------------------------------------------------
# 2. GMP (static, mingw)
# -----------------------------------------------------------------------------
build_gmp() {
    local stamp="$DEPS_ROOT/.gmp-${GMP_VER}-stamp"
    [[ -f "$stamp" ]] && { say "GMP already built"; return 0; }

    fetch "gmp-${GMP_VER}.tar.xz" "${GMP_URLS[@]}"
    verify_sha256 "gmp-${GMP_VER}.tar.xz" "$GMP_SHA256"
    extract_once "gmp-${GMP_VER}.tar.xz" "gmp-${GMP_VER}"

    say "Building GMP for $HOST_TRIPLE"
    local obj="$PS3_BUILD_ROOT/host-tools-windows/build/gmp-${GMP_VER}"
    rm -rf "$obj"
    mkdir -p "$obj"
    local build_triple
    build_triple="$(uname -m)-pc-linux-gnu"
    # GMP 6.3.0's "long long reliability test 1" includes a K&R-style
    # function definition `void g(){}` then calls `g(...)` with multiple
    # arguments.  GCC 15 (default C2x) treats `()` as "no args" and rejects
    # the call.  Pinning to C99 makes `()` mean "unspecified args" again,
    # which is what the test was written for.  -fpermissive is unnecessary
    # under the C99 dialect — the offending semantics simply don't apply.
    (cd "$obj" && \
        "$SRC_ROOT/gmp-${GMP_VER}/configure" \
            --prefix="$DEPS_ROOT" \
            --build="$build_triple" \
            --host="$HOST_TRIPLE" \
            --disable-shared \
            --enable-static \
            --disable-assembly \
            CC="$HOST_TRIPLE-gcc -std=gnu99" \
            CC_FOR_BUILD="gcc -std=gnu99" && \
        make -j"$JOBS" && \
        make install)
    touch "$stamp"
}

# -----------------------------------------------------------------------------
# 3. OpenSSL (static, mingw — only need libcrypto for our tools)
# -----------------------------------------------------------------------------
build_openssl() {
    local stamp="$DEPS_ROOT/.openssl-${OPENSSL_VER}-stamp"
    [[ -f "$stamp" ]] && { say "OpenSSL already built"; return 0; }

    fetch "openssl-${OPENSSL_VER}.tar.gz" "${OPENSSL_URLS[@]}"
    verify_sha256 "openssl-${OPENSSL_VER}.tar.gz" "$OPENSSL_SHA256"
    extract_once "openssl-${OPENSSL_VER}.tar.gz" "openssl-${OPENSSL_VER}"

    say "Building OpenSSL for $HOST_TRIPLE"
    # 3.0.x doesn't accept no-docs / no-apps / no-stdio (those landed in
    # 3.2+).  We trim to no-shared + no-tests + no-engine and run only
    # `make build_libs` to skip the apps/ subdir entirely (apps/openssl.exe
    # fails to link in 3.0.16 with no-engine: srp_lib.obj references
    # ossl_bn_group_1024 etc. that the engine path used to provide).
    #
    # We DO NOT pass no-deprecated: PSL1GHT's make_self.c uses
    # AES_encrypt() and the legacy CRYPTO_* surface, both pre-3.x APIs.
    # Stripping deprecated symbols would force a clean-room rewrite.
    #
    # `make install_sw` is the canonical static-lib install target but it
    # transitively builds apps/openssl.exe (broken above) — we just copy
    # libs + headers + pkg-config files manually, which is what
    # install_sw does anyway on a successful build.
    (cd "$SRC_ROOT/openssl-${OPENSSL_VER}" && \
        ./Configure mingw64 \
            --prefix="$DEPS_ROOT" \
            --cross-compile-prefix="$HOST_TRIPLE-" \
            no-shared no-tests no-engine && \
        make -j"$JOBS" build_libs)

    # Stage libs + headers + pkg-config bypassing OpenSSL's install_sw.
    install -d "$DEPS_ROOT/lib" "$DEPS_ROOT/lib/pkgconfig" "$DEPS_ROOT/include/openssl"
    install -m 0644 "$SRC_ROOT/openssl-${OPENSSL_VER}/libcrypto.a"  "$DEPS_ROOT/lib/"
    install -m 0644 "$SRC_ROOT/openssl-${OPENSSL_VER}/libssl.a"     "$DEPS_ROOT/lib/"
    for pc in libcrypto.pc libssl.pc openssl.pc; do
        if [[ -f "$SRC_ROOT/openssl-${OPENSSL_VER}/$pc" ]]; then
            install -m 0644 "$SRC_ROOT/openssl-${OPENSSL_VER}/$pc" "$DEPS_ROOT/lib/pkgconfig/"
        fi
    done
    cp -r "$SRC_ROOT/openssl-${OPENSSL_VER}/include/openssl/." "$DEPS_ROOT/include/openssl/"

    touch "$stamp"
}

# -----------------------------------------------------------------------------
# 4. Rust workspace -> .exe
#
# The Rust cross-compile needs the x86_64-pc-windows-gnu std lib.  rustup
# manages this via `rustup target add`; distro Rust packages (Arch / Fedora /
# Debian system rust) typically ship only the host target's std lib.  When
# rustup is not present and the target's std lib isn't installed, we skip
# this step rather than fail — the resulting Windows zip will warn about
# missing nidgen.exe / abi-verify.exe / coverage-report.exe at package time.
# Two recovery paths:
#   * Install rustup, then re-run: `pacman -R rust && pacman -S rustup &&
#     rustup default stable && rustup target add x86_64-pc-windows-gnu`
#   * Pull the .exe binaries from CI's build-host-tools-windows artifact
#     (uploaded by .github/workflows/release.yml) and pass the zip to
#     package-windows-release.sh via --tools-zip.
# -----------------------------------------------------------------------------
build_rust_tools() {
    say "Cross-compiling Rust workspace for x86_64-pc-windows-gnu"

    # Try to add the target via rustup if we have rustup.  No-op on distro
    # Rust where rustup isn't installed.
    if command -v rustup >/dev/null 2>&1; then
        rustup target add x86_64-pc-windows-gnu >/dev/null 2>&1 || true
    fi

    # Probe whether the target's std lib is actually installed before
    # invoking cargo — cargo's failure mode for a missing target is a wall
    # of "can't find crate for `core`" errors that's noisy and slow to
    # produce.  rustc --print target-libdir reports the path the std lib
    # *would* live at; we just check if it exists.
    local libdir
    libdir="$(rustc --print target-libdir --target x86_64-pc-windows-gnu 2>/dev/null || true)"
    if [[ -z "$libdir" || ! -d "$libdir" ]]; then
        warn "Rust x86_64-pc-windows-gnu std lib not installed — skipping nidgen/abi-verify/coverage-report .exe build."
        warn "  To enable: install rustup ('pacman -S rustup' on Arch / 'curl https://sh.rustup.rs | sh' elsewhere),"
        warn "             then 'rustup default stable && rustup target add x86_64-pc-windows-gnu'."
        warn "  Or merge CI's host-tools-windows artifact via package-windows-release.sh --tools-zip=PATH."
        return 0
    fi

    # Tell cargo which linker to use for the windows-gnu target.  The
    # mingw-w64 host gcc is the right pick — same ABI as the cross libs we
    # built above.  Cargo reads CARGO_TARGET_<TRIPLE>_LINKER (uppercased,
    # dashes -> underscores).
    export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="$HOST_TRIPLE-gcc"

    (cd "$PS3_TOOLCHAIN_ROOT/tools" && \
        cargo build --release --workspace --target x86_64-pc-windows-gnu)

    local rust_out="$PS3_TOOLCHAIN_ROOT/tools/target/x86_64-pc-windows-gnu/release"
    for bin in nidgen abi-verify coverage-report; do
        if [[ -f "$rust_out/${bin}.exe" ]]; then
            install -m 0755 "$rust_out/${bin}.exe" "$STAGE_BIN/"
            say "  staged ${bin}.exe"
        else
            warn "${bin}.exe not produced — check cargo log"
        fi
    done
}

# -----------------------------------------------------------------------------
# 5. rsx-cg-compiler (CMake C++17 — clean-room Cg→NV40 compiler)
# -----------------------------------------------------------------------------
build_rsx_cg_compiler() {
    say "Cross-compiling rsx-cg-compiler for $HOST_TRIPLE"

    local src="$PS3_TOOLCHAIN_ROOT/tools/rsx-cg-compiler"
    local obj="$PS3_BUILD_ROOT/host-tools-windows/build/rsx-cg-compiler"

    # Inline mingw toolchain file — keeps the cross-compile self-contained and
    # avoids a top-level cmake/ toolchain file that's unrelated to the
    # Windows-toolchain CMake migration coming later (samples/CMakeLists.txt).
    local tc="$obj/mingw-toolchain.cmake"
    rm -rf "$obj"
    mkdir -p "$obj"
    cat >"$tc" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   $HOST_TRIPLE-gcc)
set(CMAKE_CXX_COMPILER $HOST_TRIPLE-g++)
set(CMAKE_RC_COMPILER  $HOST_TRIPLE-windres)
set(CMAKE_FIND_ROOT_PATH /usr/$HOST_TRIPLE $DEPS_ROOT)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
EOF

    cmake -S "$src" -B "$obj" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$tc" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$obj" --target rsx-cg-compiler

    if [[ -f "$obj/rsx-cg-compiler.exe" ]]; then
        install -m 0755 "$obj/rsx-cg-compiler.exe" "$STAGE_BIN/"
        say "  staged rsx-cg-compiler.exe"
    else
        warn "rsx-cg-compiler.exe not produced"
    fi
}

# -----------------------------------------------------------------------------
# 6. PSL1GHT host tools — make_self / make_self_npdrm / package_finalize / fself
#
#    These live in src/ps3dev/PSL1GHT/tools/ and link against gmp + libcrypto
#    (openssl) + libz (or just libz for fself).  We compile them directly
#    here against the static mingw libs we built above, bypassing PSL1GHT's
#    Linux-flavoured Makefile entirely (the Makefile picks UNAME=Linux on
#    this host and emits Linux-flavoured link flags).
# -----------------------------------------------------------------------------
build_psl1ght_tools() {
    say "Cross-compiling PSL1GHT host tools (geohot + fself)"

    local geohot_src="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/tools/geohot"
    local fself_src="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/tools/fself"
    [[ -f "$geohot_src/make_self.c" ]] \
        || die "PSL1GHT geohot tools source missing — run scripts/bootstrap.sh"
    [[ -d "$fself_src/source" ]] \
        || die "PSL1GHT fself source missing — run scripts/bootstrap.sh"

    local cflags="-O2 -I$DEPS_ROOT/include"
    # -static-libgcc keeps libgcc_s_seh-1.dll out of the dependency list.
    # -lws2_32 is a Windows networking lib that openssl's older libs reference
    # via getaddrinfo etc.; harmless to add unconditionally.
    local ldflags="-static -static-libgcc -L$DEPS_ROOT/lib -lgmp -lcrypto -lz -lws2_32 -lcrypt32"

    local cc="$HOST_TRIPLE-gcc"
    "$cc" $cflags "$geohot_src/make_self.c"        $ldflags -o "$STAGE_BIN/make_self.exe"
    "$cc" $cflags -DNPDRM "$geohot_src/make_self.c" $ldflags -o "$STAGE_BIN/make_self_npdrm.exe"
    "$cc" $cflags "$geohot_src/package_finalize.c" $ldflags -o "$STAGE_BIN/package_finalize.exe"
    say "  staged make_self.exe make_self_npdrm.exe package_finalize.exe"

    # fself is a small fself.elf builder; ships under tools/fself/source/.
    # It only needs zlib.  Build the .c sources directly — its own Makefile
    # is heavily indirected and isn't worth re-using cross.
    local fself_cflags="-O2 -I$fself_src/include -I$DEPS_ROOT/include"
    local fself_ldflags="-static -static-libgcc -L$DEPS_ROOT/lib -lz"
    local fself_srcs=()
    mapfile -t fself_srcs < <(find "$fself_src/source" -maxdepth 1 -name '*.c' -print)
    [[ ${#fself_srcs[@]} -gt 0 ]] || die "no .c sources under $fself_src/source"
    "$cc" $fself_cflags "${fself_srcs[@]}" $fself_ldflags -o "$STAGE_BIN/fself.exe"
    say "  staged fself.exe"
}

# -----------------------------------------------------------------------------
# 7. PSL1GHT legacy sample helpers: bin2s and cgcomp.
#
#    CMake sample helpers call these from $PS3DK/bin on Windows.  They are
#    small host-side tools with no dependency on the target SDK build.
# -----------------------------------------------------------------------------
build_psl1ght_legacy_tools() {
    say "Cross-compiling PSL1GHT legacy host tools (bin2s + cgcomp)"

    ensure_psl1ght_source

    local psl1ght_src="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT"
    local bin2s_src="$psl1ght_src/tools/generic/bin2s.c"
    local cgcomp_src="$psl1ght_src/tools/cgcomp"
    local cgcomp_srcs=()

    [[ -f "$bin2s_src" ]] || die "bin2s source missing: $bin2s_src"
    "$HOST_TRIPLE-gcc" -O2 -Wall -static -static-libgcc \
        "$bin2s_src" -o "$STAGE_BIN/bin2s.exe"
    say "  staged bin2s.exe"

    mapfile -t cgcomp_srcs < <(find "$cgcomp_src/source" -maxdepth 1 -name '*.cpp' -print | sort)
    [[ ${#cgcomp_srcs[@]} -gt 0 ]] || die "no cgcomp .cpp sources under $cgcomp_src/source"
    "$HOST_TRIPLE-g++" -std=c++11 -O2 -Wall -DWIN32 \
        -static -static-libgcc -static-libstdc++ \
        -I"$cgcomp_src/include" \
        "${cgcomp_srcs[@]}" \
        -o "$STAGE_BIN/cgcomp.exe"
    say "  staged cgcomp.exe"

    if [[ -f "$cgcomp_src/cg.dll" ]]; then
        install -m 0644 "$cgcomp_src/cg.dll" "$STAGE_BIN/cg.dll"
        say "  staged cg.dll"
    else
        warn "cg.dll not found next to cgcomp.exe source output. cgcomp.exe will start but legacy Cg shader samples need cg.dll beside it in the final Windows SDK."
    fi
}

# -----------------------------------------------------------------------------
# 8. Host-side Python + assets that the .self / .pkg pipeline needs.
# -----------------------------------------------------------------------------
stage_python_and_assets() {
    say "Staging host Python tools + assets"

    # PSL1GHT installs these to $PS3DEV/bin during the Linux native build.
    # They're host-agnostic Python and small data files — copy them into the
    # Windows release bin/ verbatim.
    local src="$PS3DEV/bin"
    for f in pkg.py sfo.py fself.py Struct.py sfo.xml ICON0.PNG; do
        if [[ -f "$src/$f" ]]; then
            install -m 0644 "$src/$f" "$STAGE_BIN/$f"
        fi
    done
    # Make the Python entry-point scripts executable so they can be invoked
    # directly when Python is on PATH (Windows still respects the +x bit).
    for f in pkg.py sfo.py fself.py; do
        [[ -f "$STAGE_BIN/$f" ]] && chmod +x "$STAGE_BIN/$f"
    done
}

# -----------------------------------------------------------------------------
# Run.
# -----------------------------------------------------------------------------
say "=== Windows host-tools cross-build ==="
say "deps prefix: $DEPS_ROOT"
say "stage:       $STAGE_BIN"

build_zlib
build_gmp
build_openssl
build_rust_tools
build_rsx_cg_compiler
build_psl1ght_tools
build_psl1ght_legacy_tools
stage_python_and_assets

say "=== Done ==="
ls -lh "$STAGE_BIN" 2>/dev/null || true
say "Run package-windows-release.sh — it picks $STAGE_BIN up automatically."
