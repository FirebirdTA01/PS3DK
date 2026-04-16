#!/usr/bin/env bash
# PS3 Custom Toolchain — Phase 3: PSL1GHT
#
# Builds the PSL1GHT runtime (PPU + SPU libraries, tools, samples) against our
# new toolchain. Applies the v3 RFC patch series from patches/psl1ght/ to:
#   - Migrate symbol/type/constant naming to Sony-style (cellXxx, CellXxx,
#     CELL_XXX_*).
#   - Ship psl1ght-compat.h aliasing legacy names for 1-2 releases.
#   - Add the NV40-FP fragment shader assembler.
#   - Complete networking (getaddrinfo, select/poll).
#
# Installs into $PS3DEV/psl1ght (also used as $PSL1GHT by sample Makefiles).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[psl1ght] %s\n" "$*"; }
warn() { printf "[psl1ght] WARNING: %s\n" "$*" >&2; }
die() { printf "[psl1ght] ERROR: %s\n" "$*" >&2; exit 1; }

SRC="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT"
PATCHES="$PS3_TOOLCHAIN_ROOT/patches/psl1ght"
INSTALL="$PS3DEV/psl1ght"

# Verify toolchains are built.
[[ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]] \
    || die "PPU toolchain not installed. Run scripts/build-ppu-toolchain.sh first."
[[ -x "$PS3DEV/spu/bin/spu-elf-gcc" ]] \
    || die "SPU toolchain not installed. Run scripts/build-spu-toolchain.sh first."

# Verify PSL1GHT source is present (cloned by bootstrap.sh).
[[ -d "$SRC" ]] \
    || die "PSL1GHT source not at $SRC. Run scripts/bootstrap.sh first."

cd "$SRC"

# Apply patch series from patches/psl1ght/ if present. Idempotent via --forward.
if [[ -d "$PATCHES" ]]; then
    if [[ -f "$PATCHES/series" ]]; then
        patch_list=( $(grep -v '^#' "$PATCHES/series" | grep -v '^\s*$') )
    else
        patch_list=( $(cd "$PATCHES" && ls *.patch 2>/dev/null | sort) )
    fi
    for p in "${patch_list[@]}"; do
        say "Applying $p"
        patch -p1 --forward --silent < "$PATCHES/$p" \
            || warn "Patch $p did not apply cleanly (may already be applied)"
    done
else
    say "No psl1ght patches (patches/psl1ght/ absent — building vanilla)"
fi

# PSL1GHT's Makefile expects PSL1GHT to point at the install root.
export PSL1GHT="$INSTALL"

say "install-ctrl (copies ppu_rules / spu_rules / base_rules to \$PSL1GHT)"
make install-ctrl

say "make (builds ppu + spu + common + tools)"
make -j"$(nproc 2>/dev/null || echo 4)"

say "make install"
make install

say "=== Done ==="
say "PSL1GHT installed at: $INSTALL"
say "Try: cd samples/hello-ppu-c++17 && make"
