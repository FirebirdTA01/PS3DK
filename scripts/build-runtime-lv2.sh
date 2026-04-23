#!/usr/bin/env bash
# PS3 Custom Toolchain - build + install the native Lv-2 runtime.
#
# Produces CRT objects that implement the CellOS Lv-2 ABI directly, without
# going through PSL1GHT.
#
# Two install targets:
#   1. $PS3DEV/ppu/powerpc64-ps3-elf/lib/  - GCC's startfile search path
#      (the one "%s" in STARTFILE_LV2_SPEC resolves against). Writing here
#      supersedes PSL1GHT's stale copy so -llv2 picks up our 64-byte
#      .sys_proc_prx_param. Re-run this script whenever PSL1GHT is rebuilt,
#      since PSL1GHT's crt/Makefile reinstalls its own lv2-sprx.o over us.
#   2. $PS3DK/ppu/lib/  - PS3DK library root (mirror for visibility /
#      future explicit references).
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
gcc_startfile_dir="$PS3DEV/ppu/powerpc64-ps3-elf/lib"
ps3dk_dir="$PS3DK/ppu/lib"
mkdir -p "$gcc_startfile_dir" "$ps3dk_dir"

# Allow callers / CI to pin the SDK version word; default matches the
# reference fixture so RPCS3 / hardware loaders that gate on it accept
# our output.
: "${LV2_SDK_VERSION:=0x00475001}"

say "building lv2-sprx.o (LV2_SDK_VERSION=$LV2_SDK_VERSION)"
"$CC" -c \
    "-DLV2_SDK_VERSION=$LV2_SDK_VERSION" \
    -o "$gcc_startfile_dir/lv2-sprx.o" \
    "$runtime_src/lv2-sprx.S"
cp -f "$gcc_startfile_dir/lv2-sprx.o" "$ps3dk_dir/lv2-sprx.o"

say "building lv2-crti.o (compact-OPD _init/_fini)"
"$CC" -c \
    -mcpu=cell -mregnames -D__ASSEMBLY__ -Wa,-mcell \
    -o "$gcc_startfile_dir/lv2-crti.o" \
    "$runtime_src/lv2-crti.S"
cp -f "$gcc_startfile_dir/lv2-crti.o" "$ps3dk_dir/lv2-crti.o"

# lv2-crt0.o = our crt0.o (compact __start descriptor) + PSL1GHT's
# crt1.o (newlib/syscalls glue - unchanged), merged with `ld -r`.
# PSL1GHT ships a pre-built crt1.o that we reuse; porting it over is
# a separate chunk of work that isn't necessary to fix the .opd
# regression.
say "building lv2-crt0.o (compact-OPD __start, reuses PSL1GHT crt1.o)"
psl1ght_crt1_o="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/ppu/crt/ppu/crt1.o"
[[ -f "$psl1ght_crt1_o" ]] \
    || die "PSL1GHT crt1.o not at $psl1ght_crt1_o. Run scripts/build-psl1ght.sh."
"$CC" -c \
    -mcpu=cell -mregnames -D__ASSEMBLY__ -Wa,-mcell \
    -o "$gcc_startfile_dir/crt0.o" \
    "$runtime_src/crt0.S"
"$PS3DEV/ppu/bin/powerpc64-ps3-elf-ld" -r \
    "$gcc_startfile_dir/crt0.o" \
    "$psl1ght_crt1_o" \
    -o "$gcc_startfile_dir/lv2-crt0.o"
cp -f "$gcc_startfile_dir/lv2-crt0.o" "$ps3dk_dir/lv2-crt0.o"

say "installing lv2.ld (compact .opd ALIGN(4))"
install -m 644 "$runtime_src/lv2.ld" "$gcc_startfile_dir/lv2.ld"
install -m 644 "$runtime_src/lv2.ld" "$ps3dk_dir/lv2.ld"

say "installed $gcc_startfile_dir/{lv2-sprx.o,lv2-crti.o,lv2-crt0.o,lv2.ld} (GCC startfile)"
say "installed $ps3dk_dir/{lv2-sprx.o,lv2-crti.o,lv2-crt0.o,lv2.ld} (PS3DK mirror)"
say "done"
