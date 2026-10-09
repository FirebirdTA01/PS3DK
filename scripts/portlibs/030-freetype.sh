#!/usr/bin/env bash
# portlibs recipe: freetype 2.13.2
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE.
# Current working directory is $PS3_BUILD_ROOT/portlibs.
#
# Build system: autotools (configure + make).
# Depends on: zlib (001), already in $PORTLIBS (libpng is not used, see below),
# found via the zlib.pc that recipe installed into
# $PORTLIBS/lib/pkgconfig (PKG_CONFIG_PATH is exported by the driver).
#
# --without-png: FreeType uses libpng only for colour bitmap glyphs (emoji
# fonts).  With it, -lfreetype also needs -lpng, which Makefiles written for
# the old ps3libraries portlibs do not list (AcidSampleV2 stopped on
# png_error / png_get_error_ptr from sfnt.c).  Linking -lpng as well still
# works.
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

portlib_fetch "$TARBALL" "$SHA256" "${URLS[@]}" || exit 1

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
    --without-png \
    --without-harfbuzz \
    --without-brotli \
    --without-bzip2

make -j"$(nproc 2>/dev/null || echo 4)"
make install

# Legacy header layout.  FreeType 2.5.1 and older installed ft2build.h and
# freetype/ directly in include/; 2.5.2 and later put both under
# include/freetype2/.  PS3 homebrew written against the old ps3libraries
# portlibs (tiny3d's libfont3d users, AcidSampleV2, ...) includes
# <ft2build.h> with only -I$(PORTLIBS)/include and fails with "ft2build.h:
# No such file or directory".  Install the old layout as well, as copies:
# the Windows package cannot carry symlinks.  The modern layout stays the
# primary one (pkg-config's freetype2.pc points there).
cp -f "$PORTLIBS/include/freetype2/ft2build.h" "$PORTLIBS/include/ft2build.h"
rm -rf "$PORTLIBS/include/freetype"
cp -R "$PORTLIBS/include/freetype2/freetype" "$PORTLIBS/include/freetype"
