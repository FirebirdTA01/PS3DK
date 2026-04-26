#!/usr/bin/env bash
# PS3 Custom Toolchain — PSL1GHT runtime
#
# Builds the PSL1GHT runtime (PPU + SPU libraries, tools, samples) against our
# new toolchain. Applies the v3 RFC patch series from patches/psl1ght/ to:
#   - Migrate symbol/type/constant naming to cell-SDK style (cellXxx, CellXxx,
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
# PSL1GHT runtime now installs into $PS3DK so libsysbase / libc / librt /
# liblv2 land at $PS3DK/ppu/lib — the path the PPU GCC driver reads from
# the PS3DK env var via getenv() in its LIB_LV2_SPEC.  Same for SPU libs
# and the ppu_rules / spu_rules / base_rules Makefile fragments used by
# sample builds.  Older builds installed under $PS3DEV/psl1ght; the
# previous tree can be deleted by hand once a fresh PS3DK install is
# verified.
INSTALL="$PS3DK"

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

# --------------------------------------------------------------------
# CRT override — PSL1GHT-stale-runtime regression guard
# --------------------------------------------------------------------
# PSL1GHT's `make install` writes its own lv2-sprx.o / lv2-crti.o /
# lv2-crt0.o / lv2.ld into $PS3DEV/ppu/powerpc64-ps3-elf/lib/ — the
# directory GCC searches via the "%s" suffix in STARTFILE_LV2_SPEC.
# Those files predate our compact-OPD migration and our cellos-lv2 ABI
# spec work:
#
#   * lv2-sprx.o has a 40-byte .sys_proc_prx_param (PSL1GHT-era);
#     the modern Lv-2 loader (and RPCS3) require the 64-byte form per
#     docs/abi/cellos-lv2-abi-spec.md section 3.  Symptom: PSL1GHT's
#     own make_self refuses with "elf does not have a prx parameter
#     section.", or the SELF builds and RPCS3 segfaults during load
#     ("Segfault reading 0x0 at <host_addr>" with PC stuck at the
#     ELF's ._start) because the loader can't parse the 40-byte
#     descriptor and bails before transferring control.
#
#   * lv2-crti.o emits ELFv1 24-byte .opd entries with section flags
#     "ax".  Mixed with our compact 8-byte .opd from C / C++ TUs, the
#     binutils edit_opd path produces a Frankenstein .opd whose first
#     few entries have entry_ea_32 = 0 (the high zero-bytes of the
#     64-bit entry_ea), so anything walking those entries via
#     bl-redirect or __init_array crashes.
#
#   * lv2-crt0.o reads TOC from offset +8 of the __start descriptor
#     (assumes 24-byte ELFv1 layout).  In compact-OPD output the TOC
#     field is at offset +4 (32-bit, lwz-loaded into r2).
#
# The fix is run-it-after-PSL1GHT, encoded here so a fresh
# build-psl1ght.sh leaves a working tree.  scripts/build-runtime-lv2.sh
# rebuilds runtime/lv2/crt/{lv2-sprx,lv2-crti,crt0}.S into compact-OPD
# objects (along with the 64-byte .sys_proc_prx_param payload) and
# overwrites PSL1GHT's stale copies at the GCC startfile path and the
# $PS3DK mirror, plus replaces $PS3DEV/bin/sprxlinker with our fork
# from tools/sprx-linker/.
#
# Skipping this step is the regression mode that surfaced in the
# Apr 25 WSL2/cross-build commit train — PSL1GHT got rebuilt, the
# CRT override step was missing from the orchestration, every C++
# sample crashed during SELF load, and the symptom looked like a
# compiler / .opd / .TOC. issue (it isn't — the .opd looked broken
# only because PSL1GHT's 24-byte lv2-crti.o was being merged with our
# compact entries).  See the commit message for build(toolchain):
# wire build-runtime-lv2.sh into build-psl1ght.sh for the full
# investigation log.
say "running build-runtime-lv2.sh to override PSL1GHT's CRT with our compact-OPD runtime"
"$script_dir/build-runtime-lv2.sh"

say "=== Done ==="
say "PSL1GHT installed at: $INSTALL"
say "Try: cd samples/hello-ppu-c++17 && make"
