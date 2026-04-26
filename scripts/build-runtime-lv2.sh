#!/usr/bin/env bash
# PS3 Custom Toolchain - build + install the native Lv-2 runtime.
#
# Produces CRT objects that implement the CellOS Lv-2 ABI directly,
# without going through PSL1GHT, plus replaces $PS3DEV/bin/sprxlinker
# with our fork that knows the 64-byte .sys_proc_prx_param layout.
#
# WHY THIS SCRIPT MUST RUN AFTER build-psl1ght.sh
# -----------------------------------------------
# PSL1GHT's `make install` writes its own pre-compact-OPD CRT objects
# into $PS3DEV/ppu/powerpc64-ps3-elf/lib/ (GCC's startfile search root).
# Those files are stale on three axes simultaneously:
#
#   * lv2-sprx.o ships a 40-byte .sys_proc_prx_param.  Modern PS3 Lv-2
#     and RPCS3 require 64 bytes per docs/abi/cellos-lv2-abi-spec.md
#     section 3.  Symptom on a sample built without this override:
#     PSL1GHT's make_self refuses with "elf does not have a prx
#     parameter section.", or — if it doesn't — RPCS3 logs
#     "Segfault reading 0x0 at <host_addr>" during SELF load with PC
#     reported at the ELF's ._start because the loader bails before
#     transferring control.
#
#   * lv2-crti.o emits 24-byte ELFv1 .opd descriptors at section flag
#     "ax".  When that's merged with the compact 8-byte .opd produced
#     by GCC's CELL64LV2 codepath (gcc-12.x/0007-rs6000-...), the
#     binutils edit_opd path leaves a mixed-format .opd whose first
#     entries decode as NULL-entry-EA when read in compact mode, and
#     anything walking via bl-redirect / __init_array dispatch lands
#     at address 0.
#
#   * lv2-crt0.o reads TOC from offset +8 of __start (assumes the
#     24-byte ELFv1 layout).  Compact-OPD has TOC at offset +4.
#
# This script rebuilds $PS3_TOOLCHAIN_ROOT/runtime/lv2/crt/* (our
# spec-correct, compact-OPD sources) into objects that overwrite
# PSL1GHT's stale copies in two places:
#
#   1. $PS3DEV/ppu/powerpc64-ps3-elf/lib/  - GCC's startfile search
#      path (the "%s" suffix in STARTFILE_LV2_SPEC).  This is the
#      one that matters at link time — every sample resolves
#      lv2-crt0.o, lv2-crti.o, lv2-sprx.o, and lv2.ld here.
#   2. $PS3DK/ppu/lib/  - PS3DK library root (mirror for visibility
#      and explicit -B$(PS3DK)/ppu/lib references in samples that
#      want to be picky).
#
# Plus it overwrites $PS3DEV/bin/sprxlinker (and the $PS3DK mirror)
# with the fork from tools/sprx-linker/, since PSL1GHT also reinstalls
# its older sprxlinker — incompatible with the 64-byte
# .sys_proc_prx_param.
#
# Idempotent: re-run with the same git state and it just refreshes
# binaries.  The script is wired into build-psl1ght.sh's tail so a
# clean ./scripts/build-psl1ght.sh leaves a working install — but it
# can also be run standalone if PSL1GHT was reinstalled by hand
# (the symptom of skipping it is documented in build-psl1ght.sh's
# call site).
#
# Future-proofing: the tipping point that lets us drop this override
# is when our SDK provides lv2-* objects natively (no PSL1GHT crt at
# all) and STARTFILE_LV2_SPEC searches under $PS3DK/ppu/lib first.
# Until then, treat this script as a hard prereq for any sample build
# that goes through SELF.
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

# PSL1GHT's tools/sprxlinker/ ships an old sprxlinker that doesn't grok the
# 64-byte .sys_proc_prx_param we now emit; make_self.exe rejects the
# resulting ELF with "elf does not have a prx parameter section."  Our
# forked sprxlinker at tools/sprx-linker/ does.  Build PSL1GHT's
# `make install` deposits its stale binary into $PS3DEV/bin and
# $PS3DK/bin; overwrite both with the fork.  Idempotent — re-running
# just refreshes the binaries.
sprxlinker_fork="$PS3_TOOLCHAIN_ROOT/tools/sprx-linker/sprxlinker"
if [[ -x "$sprxlinker_fork" ]]; then
    say "overriding PSL1GHT's sprxlinker with our fork from tools/sprx-linker/"
    install -m 755 "$sprxlinker_fork" "$PS3DEV/bin/sprxlinker"
    install -m 755 "$sprxlinker_fork" "$PS3DK/bin/sprxlinker"
else
    say "WARNING: $sprxlinker_fork missing — run 'make -C tools/sprx-linker' first"
fi

say "done"
