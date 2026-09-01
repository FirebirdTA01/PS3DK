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
# /fossils/<file>, and serves an HTML page (200 OK) under the tarball's name
# for a version it has rotated away.  A release-TAGGED artifact does not move,
# so it goes first; the two zlib.net paths remain as fallbacks and are now
# content-verified per URL by portlib_fetch rather than trusted on status.
URLS=(
    "https://github.com/madler/zlib/releases/download/v$VER/$TARBALL"
    "https://zlib.net/fossils/$TARBALL"
    "https://zlib.net/$TARBALL"
)
SHA256="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
SRC="$PKG-$VER"

portlib_fetch "$TARBALL" "$SHA256" "${URLS[@]}" || exit 1

if [[ ! -d "$SRC" ]]; then
    tar xf "$TARBALL"
fi

cd "$SRC"

# zlib's configure isn't autotools, but accepts host-style env vars.
./configure --prefix="$PORTLIBS" --static

make -j"$(nproc 2>/dev/null || echo 4)"
make install
