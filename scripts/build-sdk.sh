#!/usr/bin/env bash
# PS3 Custom Toolchain — build + install our SDK.
#
# Single entry point for the entire sdk/ tree: libraries plus header-
# only subsystems.  Runs after the PSL1GHT runtime is in place
# (build-psl1ght.sh), since today's SDK headers forward to PSL1GHT
# symbols.  As we wean off PSL1GHT, the forwarding details change
# behind this command but the interface does not.
#
#   source ./scripts/env.sh
#   ./scripts/build-sdk.sh

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[sdk] %s\n" "$*"; }
die() { printf "[sdk] ERROR: %s\n" "$*" >&2; exit 1; }

[[ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]] \
    || die "PPU toolchain not installed. Run scripts/build-ppu-toolchain.sh first."
[[ -d "$PS3DEV/psl1ght/ppu/include" ]] \
    || die "PSL1GHT runtime not installed. Run scripts/build-psl1ght.sh first."

say "building + installing SDK"
make -C "$PS3_TOOLCHAIN_ROOT/sdk" install

# Replace PSL1GHT's default ICON0.PNG with our PS3DK-branded one.
# Samples that don't override ICON0 in their Makefile pick up whatever
# lives at $PS3DEV/bin/ICON0.PNG; this install makes that our icon
# instead of the PSL1GHT logo PSL1GHT itself drops there during its
# own tool install.  Re-run this after any build-psl1ght.sh since that
# restores the PSL1GHT default on top of us.
icon_src="$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG"
if [[ -f "$icon_src" ]]; then
    install -m 0644 "$icon_src" "$PS3DEV/bin/ICON0.PNG"
    say "installed sdk/assets/ICON0.PNG -> $PS3DEV/bin/ICON0.PNG"
fi

say "done"
