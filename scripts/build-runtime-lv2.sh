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
# crt1.c is now native (runtime/lv2/crt/crt1.c) — no PSL1GHT crt
# dependency.  lv2-sprx.o, lv2-crti.o, lv2-crt0.o emitted for both
# ILP32 (default) and LP64 (-mlp64) multilib.  lv2.ld is ABI-invariant.
# Future-proofing: the tipping point that lets us drop this override
# entirely is when PSL1GHT is fully phased out of the toolchain and
# STARTFILE_LV2_SPEC searches under $PS3DK/ppu/lib first.
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
librt_src="${LIBRT_SRC_DIR:-$PS3_TOOLCHAIN_ROOT/runtime/lv2/librt}"
gcc_startfile_dir="$PS3DEV/ppu/powerpc64-ps3-elf/lib"
ps3dk_dir="$PS3DK/ppu/lib"
mkdir -p "$gcc_startfile_dir" "$ps3dk_dir"

# Allow callers / CI to pin the SDK version word; default matches the
# reference fixture so RPCS3 / hardware loaders that gate on it accept
# our output.
: "${LV2_SDK_VERSION:=0x00475001}"

# Build each CRT artefact for both ILP32 (default) and LP64 (-mlp64)
# multilib variants.  lv2.ld (linker script) is ABI-invariant; install
# one copy per tree for explicitness.
for abi_flag in "" "-mlp64"; do
    if [[ -z "$abi_flag" ]]; then
        abi_subdir=""
        abi_label="ILP32"
    else
        abi_subdir="lp64"
        abi_label="LP64"
    fi

    install_gcc="$gcc_startfile_dir/$abi_subdir"
    install_ps3dk="$ps3dk_dir/$abi_subdir"
    mkdir -p "$install_gcc" "$install_ps3dk"

    say "building lv2-sprx.o ($abi_label, LV2_SDK_VERSION=$LV2_SDK_VERSION)"
    "$CC" -c $abi_flag \
        "-DLV2_SDK_VERSION=$LV2_SDK_VERSION" \
        -o "$install_gcc/lv2-sprx.o" \
        "$runtime_src/lv2-sprx.S"
    cp -f "$install_gcc/lv2-sprx.o" "$install_ps3dk/lv2-sprx.o"

    say "building lv2-crti.o ($abi_label, compact-OPD _init/_fini)"
    "$CC" -c $abi_flag \
        -mcpu=cell -mregnames -D__ASSEMBLY__ -Wa,-mcell \
        -o "$install_gcc/lv2-crti.o" \
        "$runtime_src/lv2-crti.S"
    cp -f "$install_gcc/lv2-crti.o" "$install_ps3dk/lv2-crti.o"

    # lv2-crt0.o = crt0.S (compact __start descriptor) + native
    # crt1.c (libsysbase syscall-table wiring), merged with ld -r.
    # Fall back to PSL1GHT's pre-built crt1.o only if the native
    # compile fails and we're building ILP32.
    say "building lv2-crt0.o ($abi_label, compact-OPD __start + native crt1.c)"
    "$CC" -c $abi_flag \
        -mcpu=cell -mregnames -D__ASSEMBLY__ -Wa,-mcell \
        -o "$install_gcc/crt0.o" \
        "$runtime_src/crt0.S"

    if "$CC" -c $abi_flag -mcpu=cell \
        -o "$install_gcc/crt1.o" \
        "$runtime_src/crt1.c"; then
        : # native crt1.c compiled
    elif [[ -z "$abi_flag" ]]; then
        say "WARNING: native crt1.c failed, using PSL1GHT crt1.o fallback (ILP32)"
        psl1ght_crt1="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/ppu/crt/ppu/crt1.o"
        [[ -f "$psl1ght_crt1" ]] \
            || die "native crt1.c failed and PSL1GHT crt1.o not found"
        cp -f "$psl1ght_crt1" "$install_gcc/crt1.o"
    else
        die "native crt1.c failed to compile under -mlp64 (no LP64 fallback)"
    fi

    "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ld" -r \
        "$install_gcc/crt0.o" "$install_gcc/crt1.o" \
        -o "$install_gcc/lv2-crt0.o"
    cp -f "$install_gcc/lv2-crt0.o" "$install_ps3dk/lv2-crt0.o"

    say "installing lv2.ld ($abi_label)"
    install -m 644 "$runtime_src/lv2.ld" "$install_gcc/lv2.ld"
    install -m 644 "$runtime_src/lv2.ld" "$install_ps3dk/lv2.ld"

    # Build native librt (replaces PSL1GHT's librt.a).  Each wrapper
    # compiles cleanly under both ILP32 and LP64 because the syscall
    # interface is ABI-invariant and width-sensitive types use newlib
    # typedefs throughout.
    if [[ -d "$librt_src" ]]; then
        say "building librt.a ($abi_label)"
        librt_work="$PS3_TOOLCHAIN_ROOT/build/runtime-lv2/librt/$abi_subdir"
        mkdir -p "$librt_work"
        librt_objs=""
        for src in "$librt_src"/*.c; do
            [[ -f "$src" ]] || continue
            obj="$librt_work/$(basename "${src%.c}.o")"
            "$CC" -c $abi_flag -mcpu=cell -I"$librt_src" -I"$PS3DK/ppu/include" -o "$obj" "$src"
            librt_objs="$librt_objs $obj"
        done
        if [[ -n "$librt_objs" ]]; then
            "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" rcs \
                "$install_gcc/librt.a" $librt_objs
            cp -f "$install_gcc/librt.a" "$install_ps3dk/librt.a"
        fi
        say "installed librt.a ($abi_label, $(echo $librt_objs | wc -w) objects)"
    fi

    say "installed $install_gcc/{lv2-sprx.o,lv2-crti.o,lv2-crt0.o,lv2.ld} ($abi_label GCC startfile)"
    say "installed $install_ps3dk/{lv2-sprx.o,lv2-crti.o,lv2-crt0.o,lv2.ld} ($abi_label PS3DK mirror)"
done

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
