#!/usr/bin/env bash
# <np/error.h> (np-error-codes-test.c): every NP error code in one header.
#  1. the header defines 931 codes, each guarded;
#  2. it compiles with the PPU compiler before and after the NP headers that
#     define some codes too, in gnu99, c11 and c++17, ILP32 and LP64, under
#     -Wall -Wextra -Werror (a code spelled two ways is a redefinition), and
#     the spot asserts hold in every mode.
# Self-check: a code respelled in the error header must fail the build.
#
# usage: np-error-codes-test.sh [--ps3dev DIR] [--include DIR]
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
status=0
codes=$(grep -c '^#define SCE_NP' "$inc/cell/np/error.h")
guards=$(grep -c '^#ifndef SCE_NP' "$inc/cell/np/error.h")
if [ "$codes" -eq 931 ] && [ "$guards" -eq 931 ]; then
    echo "np-error-codes: ok   931 codes, each guarded"
else
    echo "np-error-codes: FAIL $codes codes, $guards guards (want 931)"; status=1
fi
if [ -z "$ps3dev" ]; then
    echo "np-error-codes: SKIP compile (set PS3DEV or --ps3dev)"
    [ $status -eq 0 ] && echo "PASS: np-error-codes-test"
    exit $status
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "np-error-codes: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/np-error-codes-test.c"

# The NP headers come from this tree, in front of the installed SDK.
mkdir -p "$work/ov/cell" "$work/ov/np"
cp -r "$inc/cell/np" "$inc/cell/np2" "$work/ov/cell/"
cp "$inc/np/"*.h "$work/ov/np/"

compile() {  # <include dir> <lang> <std> <abi> <order flag>
    "$cc" -x "$2" "$3" $4 $5 -Wall -Wextra -Werror -I"$1" -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1
}
for order in "" -DNP_ERROR_LAST; do
    for lang in c c++; do
        stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
        for std in $stds; do
            for abi in "" -mlp64; do
                label="$lang $std ${abi:-ilp32} ${order:-error-first}"
                if out=$(compile "$work/ov" "$lang" "$std" "$abi" "$order"); then
                    echo "np-error-codes: ok   compile $label"
                else
                    echo "np-error-codes: FAIL compile $label"
                    printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -6
                    status=1
                fi
            done
        done
    done
done

# Self-check: respell one shared code (a U suffix) in the error header,
# included first: the NP header that defines it too must then fail.
mkdir -p "$work/bad"; cp -r "$work/ov/." "$work/bad/"
sed 's/^\(#define SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED  *\)0x80022701$/\10x80022701U/' \
    "$inc/cell/np/error.h" > "$work/bad/cell/np/error.h"
if cmp -s "$inc/cell/np/error.h" "$work/bad/cell/np/error.h"; then
    echo "np-error-codes: FAIL self-check could not plant the respelling"; status=1
elif ! compile "$work/bad" c -std=gnu99 "" "" > "$work/b.err"; then
    echo "np-error-codes: ok   self-check: a respelled shared code fails the build"
else
    echo "np-error-codes: FAIL self-check: a respelled shared code was not caught"; status=1
fi

[ $status -eq 0 ] && echo "PASS: np-error-codes-test"
exit $status
