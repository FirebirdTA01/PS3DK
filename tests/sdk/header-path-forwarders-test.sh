#!/usr/bin/env bash
# Ten cell/* forwarder headers, each bridging an include path that the
# canonical SDK layout has but this tree lacked (see the .c): the forwarder
# includes the one header that carries the surface, and a TU that touches a
# symbol from every forwarder's target must compile.
#
#  1. compile header-path-forwarders-test.c against the PPU toolchain as
#     gnu99, c11 and c++17, ILP32 and LP64, under -Wall -Wextra -Werror
#     (the STATIC_ASSERTs — the four "Check:" equalities — run in every
#     mode); and
#  2. each forwarder, alone, in its own translation unit, compiles in the
#     same matrix (a forwarder that includes a non-existent header breaks
#     here).
#
# Self-check: pointing one forwarder at an unrelated header (cell/rtc.h)
# makes the TU referencing the sync surface fail to compile; the TU that
# only touches the target that forward now points at still compiles.  The
# script requires exactly that split.
#
# usage: header-path-forwarders-test.sh [--ps3dev DIR] [--include DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev)  ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "header-path-forwarders: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "header-path-forwarders: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/header-path-forwarders-test.c"
status=0

# matrix: c gnu99 / c c11 / c++ c++17, plain (ILP32) and -mlp64 (LP64)
for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$inc" -I"$sdk/ppu/include" -c "$src" -o "$work/p.o" 2>&1); then
                echo "header-path-forwarders: ok   compile $label (all forwarders)"
            else
                echo "header-path-forwarders: FAIL compile $label (all forwarders)"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# One translation unit per forwarder, alone (catches a broken include).
forwarders="cell/sync/error.h
cell/sync/lfqueue.h
cell/sync/mutex.h
cell/sync/queue.h
cell/sync/rwm.h
cell/rtc/error.h
cell/rtc/rtcsvc.h
cell/ssl/error.h
cell/keyboard/libkb.h
cell/mouse/libmouse.h"
for fwd in $forwarders; do
    probe="$work/fwd.c"
    printf '#include <%s>\nint main(void) { return 0; }\n' "$fwd" > "$probe"
    if out=$("$cc" -x c -std=gnu99 -Wall -Wextra -Werror \
            -I"$inc" -I"$sdk/ppu/include" -c "$probe" -o "$work/fwd.o" 2>&1); then
        echo "header-path-forwarders: ok   lone forwarder $fwd"
    else
        echo "header-path-forwarders: FAIL lone forwarder $fwd"
        printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -4
        status=1
    fi
done

# ---- self-check --------------------------------------------------------
# Point cell/sync/mutex.h at the UNRELATED cell/rtc.h.  A TU that only
# includes that forwarder and touches an rtc symbol still compiles (it now
# really has the rtc surface); a TU that includes it and touches a sync
# mutex symbol (cellSyncMutexInitialize) must FAIL to compile.
bad="$work/bad"; mkdir -p "$bad/cell/sync"
printf '#ifndef PS3TC_CELL_SYNC_MUTEX_H\n#define PS3TC_CELL_SYNC_MUTEX_H\n#include <cell/rtc.h>\n#endif\n' \
    > "$bad/cell/sync/mutex.h"

sync_probe="$work/sync_probe.c"
printf '#include <cell/sync/mutex.h>\nint *p = &cellSyncMutexInitialize; (void)p;\n' > "$sync_probe"
rtc_probe="$work/rtc_probe.c"
printf '#include <cell/sync/mutex.h>\nint *p = &cellRtcGetCurrentTick; (void)p;\n' > "$rtc_probe"

sync_ok="no"; rtc_ok="no"
"$cc" -x c -std=gnu99 -Wall -Wextra -Werror -I"$bad" -I"$inc" \
      -I"$sdk/ppu/include" -c "$sync_probe" -o "$work/sync.o" 2>/dev/null && sync_ok="yes"
"$cc" -x c -std=gnu99 -Wall -Wextra -Werror -I"$bad" -I"$inc" \
      -I"$sdk/ppu/include" -c "$rtc_probe" -o "$work/rtc.o" 2>/dev/null && rtc_ok="yes"

# Required split: the sync-surface TU fails to build, the rtc-surface TU builds.
if [ "$sync_ok" = "no" ] && [ "$rtc_ok" = "yes" ]; then
    echo "header-path-forwarders: ok   self-check: a forwarder pointed at an unrelated header is caught"
else
    echo "header-path-forwarders: FAIL self-check: forwarder mis-point (sync=$sync_ok rtc=$rtc_ok), want sync=no rtc=yes"
    status=1
fi

[ $status -eq 0 ] && echo "PASS: header-path-forwarders-test"
exit $status
