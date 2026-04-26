#!/usr/bin/env bash
# portlibs recipe: zlib 1.3.1
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE.
# Current working directory is $PS3_BUILD_ROOT/portlibs.

set -euo pipefail

PKG=zlib
VER=1.3.1
TARBALL="$PKG-$VER.tar.gz"
# zlib.net rotates current vs. archived releases between /<file> and
# /fossils/<file>.  Try the unversioned URL first, then the archive
# path, then GitHub mirror so the recipe survives either shuffle.
URLS=(
    "https://zlib.net/$TARBALL"
    "https://zlib.net/fossils/$TARBALL"
    "https://github.com/madler/zlib/releases/download/v$VER/$TARBALL"
)
SHA256="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
SRC="$PKG-$VER"

if [[ ! -f "$TARBALL" ]]; then
    for url in "${URLS[@]}"; do
        wget --continue -O "$TARBALL" "$url" && break
        rm -f "$TARBALL"
    done
    [[ -s "$TARBALL" ]] || { echo "All zlib mirrors failed" >&2; exit 1; }
fi
echo "$SHA256  $TARBALL" | sha256sum -c - \
    || { echo "checksum mismatch for $TARBALL" >&2; exit 1; }

if [[ ! -d "$SRC" ]]; then
    tar xf "$TARBALL"
fi

cd "$SRC"

# zlib's configure isn't autotools, but accepts host-style env vars.
./configure --prefix="$PORTLIBS" --static

make -j"$(nproc 2>/dev/null || echo 4)"
make install
