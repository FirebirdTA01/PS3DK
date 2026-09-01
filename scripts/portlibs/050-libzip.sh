#!/usr/bin/env bash
# portlibs recipe: libzip 1.11.4
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE,
#   plus PS3_TOOLCHAIN_ROOT / PS3DEV / PS3DK (exported by scripts/env.sh).
# Current working directory is $PS3_BUILD_ROOT/portlibs.
#
# Build system: CMAKE ONLY — libzip has no autotools build.  We reuse the
# repo's PPU toolchain file so the cross compiler / flags / find-root
# policy match the samples.
# Depends on: zlib (001) — the only mandatory dependency.  Every optional
# codec (bzip2/lzma/zstd) and crypto backend (openssl/mbedtls/etc.) is off
# because none are ported yet.  Needed by the ps3load sample.

set -euo pipefail

PKG=libzip
VER=1.11.4
TARBALL="$PKG-$VER.tar.xz"
URLS=(
    "https://libzip.org/download/$TARBALL"
    "https://github.com/nih-at/libzip/releases/download/v$VER/$TARBALL"
)
# sha256 verified against the official libzip.org download page checksum
# for libzip-1.11.4.tar.xz (also matches Buildroot package/libzip).
SHA256="8a247f57d1e3e6f6d11413b12a6f28a9d388de110adc0ec608d893180ed7097b"
SRC="$PKG-$VER"

portlib_fetch "$TARBALL" "$SHA256" "${URLS[@]}" || exit 1

# Always start from a pristine extract: the patch step below is fatal on a
# hunk that does not apply, and an already-patched tree from a previous run
# would trip it ("Reversed (or previously applied) patch detected").
rm -rf "$SRC"
tar xf "$TARBALL"

cd "$SRC"

# Port patches (patches/portlibs/libzip/*.patch, -p1).  A hunk that does not
# apply is fatal: the source is not what the patch expects, and building on
# regardless is how header/recipe drift went unnoticed for months.
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/portlibs/$PKG"
if [[ -d "$PATCHES" ]]; then
    for p in "$PATCHES"/*.patch; do
        [[ -f "$p" ]] || continue
        echo "[$PKG] applying $(basename "$p")"
        patch -p1 --forward --silent < "$p" \
            || { echo "[$PKG] patch $(basename "$p") did not apply cleanly" >&2; exit 1; }
    done
fi

TOOLCHAIN_FILE="$PS3_TOOLCHAIN_ROOT/cmake/ps3-ppu-toolchain.cmake"
[[ -f "$TOOLCHAIN_FILE" ]] || { echo "missing $TOOLCHAIN_FILE" >&2; exit 1; }

# Fresh out-of-tree build dir (CMake caches the toolchain on first run).
rm -rf build
mkdir build
cd build

# The toolchain file restricts CMAKE_FIND_ROOT_PATH to $PS3DEV/ppu and
# $PS3DK/ppu, but zlib was installed under $PORTLIBS — append it so
# find_package(ZLIB) (FIND_ROOT_PATH_MODE_*=ONLY) can see libz.a + zlib.h.
# Pin ZLIB_* explicitly too, belt-and-braces.
#
# Use the default Makefiles generator (Ninja may be absent in the build
# env); libzip is happy with either.
# The PS3 toolchain file probes with CMAKE_TRY_COMPILE_TARGET_TYPE
# STATIC_LIBRARY, so check_function_exists() never links and answers
# yes for every name.  Pre-seed the ones this target cannot link
# (verified by linking each against the cross toolchain), or config.h
# selects code paths for clonefile, arc4random, MSVC _*() etc.
cmake -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_INSTALL_PREFIX="$PORTLIBS" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_FIND_ROOT_PATH="$PS3DEV/ppu;$PS3DK/ppu;$PORTLIBS" \
    -DZLIB_INCLUDE_DIR="$PORTLIBS/include" \
    -DZLIB_LIBRARY="$PORTLIBS/lib/libz.a" \
    -DHAVE__CLOSE=OFF \
    -DHAVE__DUP=OFF \
    -DHAVE__FDOPEN=OFF \
    -DHAVE__FILENO=OFF \
    -DHAVE__FSEEKI64=OFF \
    -DHAVE__FSTAT64=OFF \
    -DHAVE__SETMODE=OFF \
    -DHAVE__STAT64=OFF \
    -DHAVE__STRDUP=OFF \
    -DHAVE__STRTOI64=OFF \
    -DHAVE__STRTOUI64=OFF \
    -DHAVE__UNLINK=OFF \
    -DHAVE_ARC4RANDOM=OFF \
    -DHAVE_CLONEFILE=OFF \
    -DHAVE_EXPLICIT_MEMSET=OFF \
    -DHAVE_FCHMOD=OFF \
    -DHAVE_GETPROGNAME=OFF \
    -DHAVE_MEMCPY_S=OFF \
    -DHAVE_SETMODE=OFF \
    -DHAVE_STRERROR_S=OFF \
    -DHAVE_STRERRORLEN_S=OFF \
    -DHAVE_STRICMP=OFF \
    -DHAVE_STRNCPY_S=OFF \
    -DHAVE_FTS_OPEN=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_BZIP2=OFF \
    -DENABLE_LZMA=OFF \
    -DENABLE_ZSTD=OFF \
    -DENABLE_OPENSSL=OFF \
    -DENABLE_GNUTLS=OFF \
    -DENABLE_MBEDTLS=OFF \
    -DENABLE_COMMONCRYPTO=OFF \
    -DENABLE_WINDOWS_CRYPTO=OFF \
    -DBUILD_TOOLS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_DOC=OFF \
    -DBUILD_REGRESS=OFF \
    ..

cmake --build . -j"$(nproc 2>/dev/null || echo 4)"
cmake --install .
