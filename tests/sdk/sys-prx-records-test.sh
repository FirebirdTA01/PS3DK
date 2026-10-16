#!/usr/bin/env bash
# <sys/prx.h> PRX record types and relocation constants, and the <sys/event.h> queue types (SYS_PPU_QUEUE,
# SYS_SPU_QUEUE): tests/sdk/sys-prx-records-test.c pins every value, size and offset with static asserts and is
# compiled as gnu99, c11 and c++17, ILP32 and LP64, under -Wall -Wextra -Werror, with this tree's sdk/include in
# front of the installed SDK.  Its self-check: a copy of prx.h with one libent32 field widened must fail.
# Skips without PS3DEV.  usage: sys-prx-records-test.sh [--ps3dev DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; [ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then echo "sys-prx-records: SKIP (set PS3DEV or --ps3dev)"; exit 0; fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sys-prx-records: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
export PS3DK="$sdk"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/sys-prx-records-test.c"
status=0; n=0
compile() { # <include dir> <lang> <std> <abi>
    "$cc" -x "$2" "$3" $4 -Wall -Wextra -Werror -I"$1" -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1
}
for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            n=$((n + 1))
            if ! out=$(compile "$root/sdk/include" "$lang" "$std" "$abi"); then
                echo "sys-prx-records: FAIL $lang $std ${abi:-ilp32}"; printf '%s\n' "$out" | grep -E "error" | head -4 | sed 's/^/    /'; status=1
            fi
        done
    done
done
[ $status -eq 0 ] && echo "sys-prx-records: ok   $n compiles (gnu99/c11/c++17 x ILP32/LP64)"
# Self-check: the asserts must catch a layout change (nidaltsets widened shifts libname).
mkdir -p "$work/bad/sys"; cp -r "$root/sdk/include/." "$work/bad/"
sed 's/unsigned char  nidaltsets;/unsigned short nidaltsets;/' "$root/sdk/include/sys/prx.h" > "$work/bad/sys/prx.h"
if cmp -s "$root/sdk/include/sys/prx.h" "$work/bad/sys/prx.h"; then
    echo "sys-prx-records: FAIL self-check could not plant the layout change"; status=1
elif out=$(compile "$work/bad" c -std=c11 ""); then
    echo "sys-prx-records: FAIL self-check: a widened libent32 field was not caught"; status=1
elif grep -q "sys_prx_libent32_t" <<< "$out"; then
    echo "sys-prx-records: ok   self-check: a widened libent32 field fails the build"
else
    echo "sys-prx-records: FAIL self-check failed for another reason"; printf '%s\n' "$out" | head -3; status=1
fi
[ $status -eq 0 ] && echo "PASS: sys-prx-records-test" || echo "FAIL: sys-prx-records-test"
exit $status
