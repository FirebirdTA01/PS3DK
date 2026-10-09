#!/usr/bin/env bash
# portlibs recipe: tiny3d + libfont3d (wargio/tiny3d @ 9b02ae6, 2021-11-13)
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE,
#   plus PS3_TOOLCHAIN_ROOT / PS3DEV / PS3DK / PSL1GHT (scripts/env.sh).
# Current working directory is $PS3_BUILD_ROOT/portlibs.
#
# tiny3d is Hermes' small 3D/2D library over librsx; libfont3d draws bitmap
# and FreeType fonts with it.  A lot of PSL1GHT homebrew (AcidSampleV2 among
# them) links -ltiny3d -lfont3d.  Licence: the project README states it uses
# the PSL1GHT licence (MIT).
#
# Build system: its own PSL1GHT-style Makefiles (ppu_rules), run with the
# SDK's make; `make install` copies headers to $PORTLIBS/include and the
# archive to $PORTLIBS/lib.  The vertex shader ships pre-assembled
# (lib/source/vshader_text_normal.vcg.S), so no Cg compiler is involved.
# Depends on: the SDK runtime only (librsx, libgcm_sys); libfont3d's FreeType
# path needs freetype (030) at the application's link, not here.

set -euo pipefail

PKG=tiny3d
COMMIT=9b02ae6e9f21ff15185f8a3846bdca5304d7e0ae
TARBALL="$PKG-$COMMIT.tar.gz"
URLS=(
    "https://github.com/wargio/tiny3d/archive/$COMMIT.tar.gz"
)
SHA256="8f3005911ca974c00780bf431084100f064cf35236f7662bf52c61764ab36065"
SRC="$PKG-$COMMIT"

portlib_fetch "$TARBALL" "$SHA256" "${URLS[@]}" || exit 1

rm -rf "$SRC"
tar xf "$TARBALL"

# Its Makefiles read PS3DEV / PSL1GHT / PORTLIBS the PSL1GHT way and call
# the ppu-* tools; nothing else from the driver's CC/CFLAGS applies.
export PORTLIBS
mkdir -p "$PORTLIBS/include" "$PORTLIBS/lib"
make -C "$SRC/lib" install
make -C "$SRC/libfont" install

for f in "$PORTLIBS/lib/libtiny3d.a" "$PORTLIBS/lib/libfont3d.a" \
         "$PORTLIBS/include/tiny3d.h" "$PORTLIBS/include/libfont.h"; do
    [[ -s "$f" ]] || { echo "[portlibs] tiny3d: missing $f after install" >&2; exit 1; }
done
