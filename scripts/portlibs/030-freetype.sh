#!/usr/bin/env bash
# portlibs recipe: freetype 2.13.2
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE.
# Current working directory is $PS3_BUILD_ROOT/portlibs.
#
# Build system: autotools (configure + make).
# Depends on: zlib (001) and libpng (010) — both already in $PORTLIBS,
# found via the zlib.pc / libpng16.pc that those recipes installed into
# $PORTLIBS/lib/pkgconfig (PKG_CONFIG_PATH is exported by the driver).
#
# --without-harfbuzz / --without-brotli / --without-bzip2 keep the
# dependency graph acyclic (harfbuzz itself depends on freetype) and avoid
# pulling extra unported codecs.

set -euo pipefail

PKG=freetype
VER=2.13.2
TARBALL="$PKG-$VER.tar.xz"
URLS=(
    "https://download.savannah.gnu.org/releases/freetype/$TARBALL"
    "https://downloads.sourceforge.net/project/freetype/freetype2/$VER/$TARBALL"
    "https://download.savannah.nongnu.org/releases/freetype/$TARBALL"
)
# sha256 verified against Buildroot 2024.11 package/freetype/freetype.hash
# (upstream Savannah/SourceForge release hash for 2.13.2).
SHA256="12991c4e55c506dd7f9b765933e62fd2be2e06d421505d7950a132e4f1bb484d"
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

# zlib/libpng resolve through pkg-config (PKG_CONFIG_PATH=$PORTLIBS/...);
# pass CPPFLAGS/LDFLAGS as a belt-and-braces fallback for the configure
# probes that test-link against -lz / -lpng directly.
export CPPFLAGS="-I$PORTLIBS/include${CPPFLAGS:+ $CPPFLAGS}"
export LDFLAGS="-L$PORTLIBS/lib${LDFLAGS:+ $LDFLAGS}"

# Top-level ./configure delegates to builds/unix and honours --host.
./configure \
    --host="$HOST_TRIPLE" \
    --prefix="$PORTLIBS" \
    --disable-shared \
    --enable-static \
    --with-zlib=yes \
    --with-png=yes \
    --without-harfbuzz \
    --without-brotli \
    --without-bzip2

make -j"$(nproc 2>/dev/null || echo 4)"
make install
