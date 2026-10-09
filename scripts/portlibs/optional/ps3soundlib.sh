#!/usr/bin/env bash
# OPT-IN portlibs recipe: ps3soundlib (wargio/ps3soundlib @ 262ea20)
#
# Not part of the default build and not shipped in the SDK packages: it is
# GPLv3, and anything that links it must be distributed under the GPLv3
# too.  Build it only when you want that:
#
#     scripts/build-portlibs.sh ps3soundlib
#
# Sourced by scripts/build-portlibs.sh with the usual staged environment
# (PORTLIBS, PS3DEV, PS3DK, PSL1GHT, ...).  Current working directory is
# $PS3_BUILD_ROOT/portlibs.
#
# Installs into $PORTLIBS: libspu_sound (the SPU sound library and its PPU
# side), libaudioplayer / liboggplayer, libmpg123, libogg (1.2.1, its own
# copy) and libmodplay, plus spu_soundmodule.bin under
# $PORTLIBS/modules.  Its own PSL1GHT-style Makefiles do the work.

set -euo pipefail

cat >&2 <<'EOF'
[portlibs] ps3soundlib is GPLv3.  Homebrew that links any of its libraries
[portlibs] (spu_sound, audioplayer, oggplayer, mpg123, ogg, modplay from this
[portlibs] build) must be distributed under the GPLv3.
EOF

PKG=ps3soundlib
COMMIT=262ea20d4e4e3fb5d0aca8a60d09d631c0e17fb8
TARBALL="$PKG-$COMMIT.tar.gz"
URLS=(
    "https://github.com/wargio/ps3soundlib/archive/$COMMIT.tar.gz"
)
SHA256="73b75ccf07a1ad2fad61d92fb7cfa4b6844150edb127b60d0ed57d5b5c06dc92"
SRC="$PKG-$COMMIT"

portlib_fetch "$TARBALL" "$SHA256" "${URLS[@]}" || exit 1

rm -rf "$SRC"
mkdir -p "$SRC"
tar xf "$TARBALL" -C "$SRC" --strip-components=1

export PORTLIBS
mkdir -p "$PORTLIBS/include" "$PORTLIBS/lib"
make -C "$SRC"

for f in libspu_sound.a libaudioplayer.a libmpg123.a; do
    [[ -s "$PORTLIBS/lib/$f" ]] || { echo "[portlibs] ps3soundlib: missing $PORTLIBS/lib/$f after install" >&2; exit 1; }
done
