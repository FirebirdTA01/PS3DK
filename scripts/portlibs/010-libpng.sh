#!/usr/bin/env bash
# portlibs recipe: libpng 1.6.58
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE.
# Current working directory is $PS3_BUILD_ROOT/portlibs.
#
# Build system: autotools (configure + make).
# Depends on: zlib (001) — already installed in $PORTLIBS.

set -euo pipefail

PKG=libpng
VER=1.6.58
TARBALL="$PKG-$VER.tar.xz"
# Canonical home is SourceForge (project libpng); GitHub mirror under
# pnggroup/libpng carries the same release assets as a fallback.
URLS=(
    "https://downloads.sourceforge.net/project/libpng/libpng16/$VER/$TARBALL"
    "https://download.sourceforge.net/libpng/$TARBALL"
    "https://github.com/pnggroup/libpng/releases/download/v$VER/$TARBALL"
)
# sha256 verified against Buildroot package/libpng/libpng.hash (master),
# which records it as the upstream SourceForge release hash for 1.6.58.
SHA256="28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775"
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

# CC/CXX/AR/RANLIB/CFLAGS come from the driver — do not hardcode them.
# zlib lives in $PORTLIBS; point libpng's configure (its AC_CHECK_LIB
# zlibVersion link test) at it explicitly.
export CPPFLAGS="-I$PORTLIBS/include${CPPFLAGS:+ $CPPFLAGS}"
export LDFLAGS="-L$PORTLIBS/lib${LDFLAGS:+ $LDFLAGS}"

# --host triggers cross mode; static-only since the PS3 SELF link is static.
# The PPU has no ARM/x86 SIMD, so libpng's hardware-accel autodetect is a
# no-op here; no --enable-hardware-optimizations override is needed.
./configure \
    --host="$HOST_TRIPLE" \
    --prefix="$PORTLIBS" \
    --disable-shared \
    --enable-static

make -j"$(nproc 2>/dev/null || echo 4)"
make install
