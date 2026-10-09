#!/usr/bin/env bash
# The canonical sys_ synchronisation surface (see sys-sync-surface-test.c):
# compiled with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
# with <sys/dbg.h> before and after, under -Wall -Wextra -Werror.
#
# usage: sys-sync-surface-test.sh [--ps3dev DIR] [--include DIR]
#   --include  header tree under test (default: this tree's sdk/include).
#              Only the headers this change touches are overlaid on the
#              installed SDK's ppu/include, which keeps one copy of the libc
#              wrapper headers on the path.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "sys-sync-surface: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sys-sync-surface: FAIL: no PPU compiler under $ps3dev"; exit 1; }
# SDK header root: source build ($PS3DK=$ps3dev/ps3dk) and installed package
# (PS3DK==PS3DEV, headers directly under $ps3dev/ppu/include) differ; resolve once.
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/ov/sys" "$work/ov/lv2"
for h in sys/synchronization.h sys/event.h sys/event_queue.h sys/dbg.h lv2/mutex.h lv2/cond.h; do
    [ -f "$inc/$h" ] && cp "$inc/$h" "$work/ov/$h"
done
src="$root/tests/sdk/sys-sync-surface-test.c"
status=0
for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            for order in "" -DDBG_FIRST; do
                label="$lang $std ${abi:-ilp32} ${order:-dbg-last}"
                if out=$("$cc" -x "$lang" "$std" $abi $order -Wall -Wextra -Werror \
                        -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/p.o" 2>&1); then
                    echo "sys-sync-surface: ok   $label"
                else
                    echo "sys-sync-surface: FAIL $label"
                    printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                    status=1
                fi
            done
        done
    done
done
exit $status
