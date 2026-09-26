#!/usr/bin/env bash
# <sys/prx.h> and <lv2/prx.h> must each work alone and in either order, in C
# and C++, on both PPU ABIs, under -Wall -Wextra -Werror.  The headers they
# replace shared one include guard, so whichever came second was skipped:
# lv2/prx.h first lost every type, sys/prx.h first lost every call.  The
# ILP32 branch also declared sysPrxUser_pchar but used sysPrxUserPchar.
#
# usage: prx-headers-test.sh [--ps3dev DIR] [--include DIR]...
#   --include  header directories to test, in order (default: this tree's
#              sys/prx.h and lv2/prx.h, staged alone so the tree's other
#              wrapper headers do not shadow the installed ones).  Point it at
#              an older install's headers to reproduce the failure.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; incs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) incs+=("-I$2"); shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]..." >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "prx-headers: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
src="$root/tests/sdk/prx-headers-test.c"
# ppu-types.h and the rest of the installed SDK, after the headers under test.
installed="$ps3dev/ps3dk/ppu/include"
status=0
fail() { echo "prx-headers: FAIL: $*"; status=1; }
ok() { echo "prx-headers: ok   $*"; }
[ -x "$cc" ] && [ -x "$cxx" ] || { fail "no PPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ ${#incs[@]} -eq 0 ]; then
    mkdir -p "$work/inc/sys" "$work/inc/lv2"
    cp "$root/sdk/include/sys/prx.h" "$work/inc/sys/prx.h"
    cp "$root/sdk/include/lv2/prx.h" "$work/inc/lv2/prx.h"
    incs=("-I$work/inc")
fi

for lang in c c++; do
    drv="$cc"; stds="-std=gnu99 -std=c11"
    [ "$lang" = c++ ] && { drv="$cxx"; stds="-std=c++17"; }
    for std in $stds; do
        for abi in "" -mlp64; do
            for order in SYS_FIRST LV2_FIRST SYS_ONLY LV2_ONLY; do
                label="$lang $std ${abi:-ilp32} $order"
                if "$drv" -x "$lang" "$std" $abi "-DORDER_$order" \
                        -Wall -Wextra -Werror \
                        "${incs[@]}" -I"$installed" -c "$src" -o "$work/p.o" > "$work/e.log" 2>&1; then
                    ok "$label"
                else
                    fail "$label: $(grep -m1 -E 'error' "$work/e.log")"
                fi
            done
        done
    done
done

[ "$status" -eq 0 ] && echo "prx-headers: PASS"
exit $status
