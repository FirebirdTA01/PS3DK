#!/usr/bin/env bash
# PS3 Custom Toolchain — portlibs
#
# Drives per-library recipes under scripts/portlibs/*.sh. Each recipe is
# self-contained: it fetches its tarball, verifies checksum, applies patches
# from patches/portlibs/<libname>/, configures and builds for powerpc64-ps3-elf,
# installs into $PS3DEV/portlibs/ppu.
#
# Ordering is critical (libpng needs zlib, freetype needs libpng, etc.), so
# recipes are executed in filename order.
#
# Usage:
#   build-portlibs.sh                # build everything in order
#   build-portlibs.sh zlib libpng    # build specific recipes by name

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[portlibs] %s\n" "$*"; }
warn() { printf "[portlibs] WARNING: %s\n" "$*" >&2; }
die() { printf "[portlibs] ERROR: %s\n" "$*" >&2; exit 1; }

RECIPES_DIR="$PS3_TOOLCHAIN_ROOT/scripts/portlibs"
PORTLIBS_INSTALL="$PS3DEV/portlibs/ppu"
BUILD="$PS3_BUILD_ROOT/portlibs"

[[ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]] \
    || die "PPU toolchain not installed. Run scripts/build-ppu-toolchain.sh first."

mkdir -p "$BUILD" "$PORTLIBS_INSTALL"

export PORTLIBS="$PORTLIBS_INSTALL"
export PATH="$PS3DEV/ppu/bin:$PORTLIBS_INSTALL/bin:$PATH"
export PKG_CONFIG_PATH="$PORTLIBS_INSTALL/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# Cross-compile defaults consumed by all recipes.
export HOST_TRIPLE="powerpc64-ps3-elf"
export CC="$HOST_TRIPLE-gcc"
export CXX="$HOST_TRIPLE-g++"
export AR="$HOST_TRIPLE-ar"
export RANLIB="$HOST_TRIPLE-ranlib"
export STRIP="$HOST_TRIPLE-strip"
export CFLAGS="-O2 -mcpu=cell -fPIC"
export CXXFLAGS="$CFLAGS"

discover_recipes() {
    # Recipes are *.sh files in scripts/portlibs/, executed in filename order.
    # User may pass specific names (without .sh) to filter.
    if [[ $# -eq 0 ]]; then
        find "$RECIPES_DIR" -maxdepth 1 -type f -name '*.sh' | sort
    else
        for name in "$@"; do
            local file="$RECIPES_DIR/$name.sh"
            [[ -f "$file" ]] || die "No recipe: $file"
            echo "$file"
        done
    fi
}

run_recipe() {
    local recipe="$1"
    say "=== $(basename "$recipe") ==="
    (
        cd "$BUILD"
        # shellcheck disable=SC1090
        source "$recipe"
    )
}

mapfile -t RECIPES < <(discover_recipes "$@")
if [[ ${#RECIPES[@]} -eq 0 ]]; then
    warn "No recipes found in $RECIPES_DIR"
    exit 0
fi

for r in "${RECIPES[@]}"; do
    run_recipe "$r"
done

say "=== Done ==="
say "portlibs installed at: $PORTLIBS_INSTALL"
