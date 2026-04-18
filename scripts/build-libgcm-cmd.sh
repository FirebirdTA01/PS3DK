#!/usr/bin/env bash
# PS3 Custom Toolchain — build + install libgcm_cmd.a
#
# Our clean-room implementation of the cellGcmCg* struct-walker API
# over the CgBinaryProgram blob format (see sdk/libgcm_cmd/include/
# Cg/cgBinary.h).  Installs the archive under $PS3DEV/psl1ght/ppu/lib
# and the public headers under $PS3DEV/psl1ght/ppu/include/.
#
# Run after build-psl1ght.sh (which populates the lib install prefix).
#
#   source ./scripts/env.sh
#   ./scripts/build-libgcm-cmd.sh

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[libgcm_cmd] %s\n" "$*"; }
die() { printf "[libgcm_cmd] ERROR: %s\n" "$*" >&2; exit 1; }

[[ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]] \
    || die "PPU toolchain not installed. Run scripts/build-ppu-toolchain.sh first."
[[ -d "$PS3DEV/psl1ght/ppu/lib" ]] \
    || die "PSL1GHT not installed. Run scripts/build-psl1ght.sh first."

SRC="$PS3_TOOLCHAIN_ROOT/sdk/libgcm_cmd"

say "building + installing libgcm_cmd.a"
make -C "$SRC" clean >/dev/null
make -C "$SRC" install

say "done"
