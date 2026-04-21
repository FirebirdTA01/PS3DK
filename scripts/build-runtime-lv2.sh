#!/usr/bin/env bash
# PS3 Custom Toolchain — build + install the native Lv-2 runtime.
#
# Produces CRT objects and linker scripts that implement the CellOS Lv-2
# ABI directly, without going through PSL1GHT. Installed to $PS3DK/ppu/lib/
# so GCC's startfile search path finds them before the PSL1GHT copies.
#
# Scope today: lv2-sprx.o only. Enough to close the .sys_proc_prx_param
# size delta measured against docs/abi/cellos-lv2-abi-spec.md. The rest
# of the CRT (lv2-crt0.o, lv2-crti.o, lv2-crtn.o, lv2.ld) follows in
# subsequent phases.
#
#   source ./scripts/env.sh
#   ./scripts/build-runtime-lv2.sh

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[runtime-lv2] %s\n" "$*"; }
die() { printf "[runtime-lv2] ERROR: %s\n" "$*" >&2; exit 1; }

CC="$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc"
[[ -x "$CC" ]] || die "PPU toolchain not installed. Run scripts/build-ppu-toolchain.sh first."

runtime_src="$PS3_TOOLCHAIN_ROOT/runtime/lv2/crt"
install_dir="$PS3DK/ppu/lib"
mkdir -p "$install_dir"

# Allow callers / CI to pin the SDK version word; default matches the
# reference fixture so RPCS3 / hardware loaders that gate on it accept
# our output.
: "${LV2_SDK_VERSION:=0x00475001}"

say "building lv2-sprx.o (LV2_SDK_VERSION=$LV2_SDK_VERSION)"
"$CC" -c \
    "-DLV2_SDK_VERSION=$LV2_SDK_VERSION" \
    -o "$install_dir/lv2-sprx.o" \
    "$runtime_src/lv2-sprx.S"

say "installed $install_dir/lv2-sprx.o"
say "done"
