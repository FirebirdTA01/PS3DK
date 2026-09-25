#!/usr/bin/env bash
# <sdk_version.h> defines CELL_SDK_VERSION 0x475001 and is installed for SPU
# as well as PPU, as the reference ships it (target/common/include, seen by
# both processors).  Reference samples gate features on the macro and compare
# data files against it; SPU samples failed "sdk_version.h: No such file".
#
# A. (host, always) run the SDK's real install targets for the version and
#    shared headers into a temporary PS3DK and check: both trees carry
#    sdk_version.h and cell/sdk_version.h, the SPU copies are byte-identical
#    to the PPU ones, and a host preprocessor sees CELL_SDK_VERSION ==
#    0x475001 through the SPU tree.
# B. (installed, when PS3DEV is set) PPU (both ABIs) and SPU compilers see
#    the macro through the installed tree, and libPSGL no longer defines
#    psglRescAdjustAspectRatio (the reference's libPSGL does not either: at
#    CELL_SDK_VERSION >= 0x180000 it is a header inline over
#    cellRescAdjustAspectRatio).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
status=0
fail() { echo "sdk-version-header: FAIL: $*" >&2; status=1; }
ok() { echo "sdk-version-header: ok   $*"; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# psgl_archives_clean <nm> <ps3dk>: both ABI libPSGL archives must exist, nm
# must succeed on each, and neither may DEFINE psglRescAdjustAspectRatio
# (function descriptor) or .psglRescAdjustAspectRatio (entry point).
psgl_archives_clean() {
    local nm="$1" sdk="$2" lib out rc=0
    for lib in "$sdk/ppu/lib/libPSGL.a" "$sdk/ppu/lib/lp64/libPSGL.a"; do
        if [ ! -f "$lib" ]; then
            fail "missing $lib"; rc=1; continue
        fi
        if ! out=$("$nm" --defined-only "$lib" 2>"$work/nm.err"); then
            fail "nm failed on $lib: $(head -1 "$work/nm.err")"; rc=1; continue
        fi
        if printf '%s\n' "$out" | grep -Eq ' \.?psglRescAdjustAspectRatio$'; then
            fail "$lib still defines psglRescAdjustAspectRatio"; rc=1
        else
            ok "$lib leaves psglRescAdjustAspectRatio to the header"
        fi
    done
    return $rc
}

probe='#include <sdk_version.h>
#if !defined(CELL_SDK_VERSION) || CELL_SDK_VERSION != 0x475001
#error CELL_SDK_VERSION is not 0x475001
#endif
#if !defined(PS3SDK_VERSION_MAJOR)
#error cell/sdk_version.h was not reached
#endif
int sdk_version_probe = CELL_SDK_VERSION;'
printf '%s\n' "$probe" > "$work/probe.c"

# --- A: host install check ---------------------------------------------------
host_cc=$(command -v cc || command -v gcc || true)
if ! command -v make >/dev/null || [ -z "$host_cc" ]; then
    echo "sdk-version-header: SKIP A (no host make/cc)"
else
    stage="$work/ps3dk"
    if make -s -C "$root/sdk" install-headers install-version install-shared-spurs-spu-headers \
            PS3DEV="$work/unused" PS3DK="$stage" >"$work/install.log" 2>&1; then
        for side in ppu spu; do
            for h in sdk_version.h cell/sdk_version.h; do
                [ -s "$stage/$side/include/$h" ] && ok "$side installs $h" \
                    || fail "$side does not install $h"
            done
        done
        for h in sdk_version.h cell/sdk_version.h; do
            cmp -s "$stage/ppu/include/$h" "$stage/spu/include/$h" \
                && ok "spu $h is the ppu copy" || fail "spu $h differs from ppu"
        done
        "$host_cc" -I"$stage/spu/include" -c "$work/probe.c" -o "$work/host.o" 2>"$work/host.log" \
            && ok "CELL_SDK_VERSION == 0x475001 through the spu tree" \
            || fail "spu tree preprocess: $(head -2 "$work/host.log")"
    else
        fail "install targets failed: $(tail -3 "$work/install.log")"
    fi
fi

# --- B: installed tree ---------------------------------------------------------
if [ -z "${PS3DEV:-}" ]; then
    echo "sdk-version-header: SKIP B (set PS3DEV)"
else
    sdk="${PS3DK:-$PS3DEV/ps3dk}"
    ppu="$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc"
    spu="$PS3DEV/spu/bin/spu-elf-gcc"
    for abi in "" -mlp64; do
        "$ppu" $abi -I"$sdk/ppu/include" -c "$work/probe.c" -o "$work/ppu.o" 2>"$work/ppu.log" \
            && ok "ppu ${abi:-ilp32} sees CELL_SDK_VERSION" \
            || fail "ppu ${abi:-ilp32}: $(head -2 "$work/ppu.log")"
    done
    "$spu" -I"$sdk/spu/include" -c "$work/probe.c" -o "$work/spu.o" 2>"$work/spu.log" \
        && ok "spu sees CELL_SDK_VERSION" || fail "spu: $(head -2 "$work/spu.log")"
    psgl_archives_clean "$PS3DEV/ppu/bin/powerpc64-ps3-elf-nm" "$sdk" || status=1

    # A consumer's call goes through the header inline to cellResc: the
    # object references cellRescAdjustAspectRatio and not the psgl symbol.
    printf '#include <PSGL/psgl.h>\nvoid adjust(void) { psglRescAdjustAspectRatio(0.5f, 0.75f); }\n' > "$work/use.c"
    cp "$work/use.c" "$work/use.cpp"
    nm="$PS3DEV/ppu/bin/powerpc64-ps3-elf-nm"
    for abi in "" -mlp64; do
        for lang in c cpp; do
            drv="$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc"
            [ "$lang" = cpp ] && drv="$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++"
            label="${abi:-ilp32} $lang consumer"
            if ! "$drv" $abi -O2 -I"$sdk/ppu/include" -c "$work/use.$lang" -o "$work/use.o" 2>"$work/use.log"; then
                fail "$label does not compile: $(grep -m1 "error" "$work/use.log")"; continue
            fi
            if ! "$nm" "$work/use.o" > "$work/use.nm" 2>"$work/use.nmerr"; then
                fail "$label: nm failed: $(head -1 "$work/use.nmerr")"; continue
            fi
            if grep -Eq ' U \.?cellRescAdjustAspectRatio$' "$work/use.nm" &&
               ! grep -Eq ' \.?psglRescAdjustAspectRatio$' "$work/use.nm"; then
                ok "$label calls cellRescAdjustAspectRatio through the header inline"
            else
                fail "$label: expected U cellRescAdjustAspectRatio and no psgl symbol: $(grep -i adjust "$work/use.nm" | tr '\n' ' ')"
            fi
        done
    done
fi

# --- C: the archive check's own negatives (always run, no toolchain) --------
# A missing archive and a failing nm must FAIL the archive check, not read as
# "symbol absent" (the first draft skipped a missing archive and swallowed an
# nm error in a pipeline).
selftest() {  # selftest <label> <nm> <sdk> <want 0|1>
    local rc=0
    # A subshell: the deliberate failures must not set this script's status.
    ( psgl_archives_clean "$2" "$3" ) >/dev/null 2>&1 || rc=1
    [ "$rc" -eq "$4" ] && ok "self-test: $1" || fail "self-test: $1 (got $rc, want $4)"
}
fake="$work/fake"; mkdir -p "$fake/ppu/lib/lp64"
: > "$fake/ppu/lib/libPSGL.a"; : > "$fake/ppu/lib/lp64/libPSGL.a"
printf '#!/bin/sh\necho "00000000 T glEnable"\n' > "$work/nm-clean"
printf '#!/bin/sh\necho "nm: bad archive" >&2\nexit 1\n' > "$work/nm-broken"
printf '#!/bin/sh\necho "000000f8 D psglRescAdjustAspectRatio"\n' > "$work/nm-descriptor"
printf '#!/bin/sh\necho "00000010 T .psglRescAdjustAspectRatio"\n' > "$work/nm-dotentry"
chmod +x "$work"/nm-*
selftest "clean archives pass" "$work/nm-clean" "$fake" 0
selftest "a failing nm fails" "$work/nm-broken" "$fake" 1
selftest "a descriptor definition fails" "$work/nm-descriptor" "$fake" 1
selftest "a dot-entry definition fails" "$work/nm-dotentry" "$fake" 1
rm "$fake/ppu/lib/lp64/libPSGL.a"
selftest "a missing lp64 archive fails" "$work/nm-clean" "$fake" 1

[ "$status" -eq 0 ] && echo "sdk-version-header: PASS"
exit $status
