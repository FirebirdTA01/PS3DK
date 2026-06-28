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

if [[ ! -f "$TARBALL" ]]; then
    for url in "${URLS[@]}"; do
        wget --continue -O "$TARBALL" "$url" && break
        rm -f "$TARBALL"
    done
    [[ -s "$TARBALL" ]] || { echo "All $PKG mirrors failed" >&2; exit 1; }
fi
echo "$SHA256  $TARBALL" | sha256sum -c - \
    || { echo "checksum mismatch for $TARBALL" >&2; exit 1; }

if [[ ! -d "$SRC" ]]; then
    tar xf "$TARBALL"
fi

cd "$SRC"

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
cmake -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_INSTALL_PREFIX="$PORTLIBS" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_FIND_ROOT_PATH="$PS3DEV/ppu;$PS3DK/ppu;$PORTLIBS" \
    -DZLIB_INCLUDE_DIR="$PORTLIBS/include" \
    -DZLIB_LIBRARY="$PORTLIBS/lib/libz.a" \
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
