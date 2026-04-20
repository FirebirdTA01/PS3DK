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

say "done"
