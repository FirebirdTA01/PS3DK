#!/usr/bin/env bash
# portlibs recipe: cairo 1.16.0
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE.
# Current working directory is $PS3_BUILD_ROOT/portlibs.
#
# Build system: autotools (configure + make).  1.16.0 is the last
# autotools cairo; 1.18+ is meson-only, hence the pin.
# Depends on: pixman-1 (020), freetype (030), libpng (010), zlib (001) —
# all in $PORTLIBS, located via their .pc files on PKG_CONFIG_PATH.
#
# Only the software "image" surface backend is wanted: the PS3 has no X11,
# Wayland, GL/EGL or fontconfig in this toolchain, so every windowing /
# display / font-config backend is disabled.  FreeType stays ENABLED so
# cairo can rasterise glyphs (cairo/videoTest/save samples need it).

set -euo pipefail

PKG=cairo
VER=1.16.0
TARBALL="$PKG-$VER.tar.xz"
URLS=(
    "https://www.cairographics.org/releases/$TARBALL"
    "https://cairographics.org/releases/$TARBALL"
    "https://ftp2.osuosl.org/pub/blfs/conglomeration/cairo/$TARBALL"
)
# sha256 verified against Buildroot 2023.02 package/cairo/cairo.hash
# (upstream cairographics.org 1.16.0 release hash).
SHA256="5e7b29b3f113ef870d1e3ecf8adf21f923396401604bda16d44be45e66052331"
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

# pixman/freetype/png/zlib come in through pkg-config; keep the -I/-L
# fallback for the handful of configure link probes that bypass it.
export CPPFLAGS="-I$PORTLIBS/include${CPPFLAGS:+ $CPPFLAGS}"
export LDFLAGS="-L$PORTLIBS/lib${LDFLAGS:+ $LDFLAGS}"

# LIKELY NEEDS PATCH: cairo 1.16.0's configure runs AC_RUN_IFELSE probes
# (atomic-primitive detection, float word-order / endianness) that abort
# under cross-compilation with "cannot run test program while cross
# compiling".  Embedded ports (Buildroot/OE) work around this by seeding
# the ac_cv_* cache.  If configure stops, add a config.cache or
# patches/portlibs/cairo/*.patch rather than guessing values here.
#
# NOTE: a plain top-level `make` also descends into test/ perf/ util/,
# some of which fail to cross-compile (they assume a hosted environment).
# If that bites, build only the library with `make -C src` and install
# with `make -C src install` plus `make install-pkgconfigDATA`.
./configure \
    --host="$HOST_TRIPLE" \
    --prefix="$PORTLIBS" \
    --disable-shared \
    --enable-static \
    --enable-ft \
    --enable-png=yes \
    --disable-fc \
    --disable-xlib \
    --disable-xcb \
    --disable-gl \
    --disable-egl \
    --disable-glx \
    --disable-script \
    --disable-trace \
    --disable-interpreter \
    --disable-valgrind \
    --disable-gtk-doc

make -j"$(nproc 2>/dev/null || echo 4)"
make install
