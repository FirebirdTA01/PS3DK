#!/usr/bin/env bash
# <cell/gcm.h>, <cell/gcm/gcm_command_c.h>, <cell/gcm/gcm_enum.h> and
# <rsx/gcm_sys.h> must compile together, in any order, with no macro
# redefinition warning under -Wall -Wextra -Werror: each CELL_GCM_* name has
# exactly one definition.  CELL_GCM_DEBUG_LEVEL0..2 were defined twice with
# different spellings, and CELL_GCM_ZCULL_Z24S8 twice in gcm_enum.h, so every
# -Werror GCM user failed.  The probe also pins the values.
#
# usage: gcm-redefinition-test.sh [--ps3dev DIR] [--include DIR]...
#   --include  header directories to test, in order (default: this tree's
#              sdk/include and sdk/libgcm_cmd/include).  Point it at an older
#              tree's headers to reproduce the failure.
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
    echo "gcm-redefinition: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
[ ${#incs[@]} -gt 0 ] || incs=("-I$root/sdk/include" "-I$root/sdk/libgcm_cmd/include")
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
src="$root/tests/sdk/gcm-redefinition-test.c"
status=0
fail() { echo "gcm-redefinition: FAIL: $*"; status=1; }
ok() { echo "gcm-redefinition: ok   $*"; }
[ -x "$cc" ] && [ -x "$cxx" ] || { fail "no PPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for lang in c c++; do
    drv="$cc"; stds="-std=gnu99 -std=c11"
    [ "$lang" = c++ ] && { drv="$cxx"; stds="-std=c++17"; }
    for std in $stds; do
        for abi in "" -mlp64; do
            for order in CELL_FIRST RSX_FIRST ENUM_FIRST ENUM_RSX; do
                label="$lang $std ${abi:-ilp32} $order"
                if "$drv" -x "$lang" "$std" $abi "-DORDER_$order" \
                        -Wall -Wextra -Werror \
                        "${incs[@]}" -c "$src" -o "$work/p.o" > "$work/e.log" 2>&1; then
                    ok "$label"
                else
                    fail "$label: $(grep -m1 -E 'error|redefined' "$work/e.log")"
                fi
            done
        done
    done
done

[ "$status" -eq 0 ] && echo "gcm-redefinition: PASS"
exit $status
