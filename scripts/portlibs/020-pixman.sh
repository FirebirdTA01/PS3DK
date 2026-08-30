#!/usr/bin/env bash
# portlibs recipe: pixman 0.42.2
#
# Sourced by scripts/build-portlibs.sh with a staged environment:
#   CC, CXX, AR, RANLIB, STRIP, CFLAGS, CXXFLAGS, PORTLIBS, HOST_TRIPLE.
# Current working directory is $PS3_BUILD_ROOT/portlibs.
#
# Build system: autotools (configure + make).
# 0.42.2 is the LAST pixman release that ships an autotools build; 0.44+
# is meson-only, so this version is pinned deliberately.  Installs the
# pixman-1 library + pixman-1.pc that cairo (040) links against.
# Depends on: nothing in portlibs (only libc).

set -euo pipefail

PKG=pixman
VER=0.42.2
TARBALL="$PKG-$VER.tar.xz"
URLS=(
    "https://www.cairographics.org/releases/$TARBALL"
    "https://www.x.org/releases/individual/lib/$TARBALL"
    "https://xorg.freedesktop.org/archive/individual/lib/$TARBALL"
)
# sha256 verified against Buildroot 2023.02 package/pixman/pixman.hash
# (sourced from the X.org pixman-0.42.2 announcement, Oct 2022).
SHA256="5747d2ec498ad0f1594878cc897ef5eb6c29e91c53b899f7f71b506785fc1376"
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

# The Cell PPE has AltiVec/VMX, but pixman's VMX fast-path autodetect
# leans on getauxval()/__builtin_cpu_supports, neither of which exists in
# the newlib bare-metal runtime — leaving it enabled risks an unresolved
# reference at link time.  Disable the SIMD backends; the C fast paths are
# enough for cairo's image surface.  (If you later port a runtime CPU
# probe, drop --disable-vmx and add the AltiVec build flags.)
#
# LIKELY NEEDS PATCH: pixman's configure runs a couple of AC_RUN_IFELSE
# probes (e.g. the TLS / __thread support test) that abort under cross
# compilation.  If configure stops with "cannot run test program while
# cross compiling", add patches/portlibs/pixman/*.patch (or seed a
# config.cache with pixman_cv_* / ac_cv_tls vars) rather than editing here.
# Thread safety: pixman keeps its fast-path cache in a static __thread
# struct and detects `TLS __thread` with a compile-only probe.  That is the
# path we want — local-exec TLS works at runtime on this target (regression
# row TLS_OK), and the portlibs CFLAGS carry no -fPIC (the one combination
# that ICEs GCC 12.4 on __thread under ILP32).  The former
# -DPIXMAN_NO_TLS fallback left the cache an unguarded global: a data race
# the moment two threads composite.  Do not reintroduce it.
if [[ -f Makefile ]]; then
    # A tree configured by an older recipe revision holds objects built
    # with different CPPFLAGS; make does not track that. Start clean.
    make distclean >/dev/null 2>&1 || true
fi
./configure \
    --host="$HOST_TRIPLE" \
    --prefix="$PORTLIBS" \
    --disable-shared \
    --enable-static \
    --disable-vmx \
    --disable-arm-simd \
    --disable-arm-neon \
    --disable-mmx \
    --disable-sse2 \
    --disable-gtk \
    --disable-libpng

# The probe result the whole recipe exists to secure: silent fallback to
# a slower/racier path is exactly what we must not ship.
grep -q '^#define TLS __thread' config.h \
    || { echo "pixman configure did not detect TLS __thread (see config.log)" >&2; exit 1; }

make -j"$(nproc 2>/dev/null || echo 4)"
make install
