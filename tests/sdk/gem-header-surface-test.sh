#!/usr/bin/env bash
# <cell/gem.h> PlayStation Move surface (see gem-header-surface-test.c):
#  1. the test TU compiles with the PPU compiler as gnu99, c11 and c++17,
#     ILP32 and LP64, under -O2 -Wall -Wextra -Werror; its static asserts
#     pin every structure's size, alignment and member offsets (identical
#     in both data models, since the structures are shared with the PRX),
#     every CELL_GEM_* constant, and the type of each of the 37 imports;
#  2. <cell/gem.h> and <cell/gem/libgem.h> each compile alone;
#  3. the TU links against libgem_stub.a in both data models, every one of
#     the 37 cellGem* imports resolves, and no call to
#     gem_helper_mismatch() survives (the inline helpers return the
#     expected values).
# Self-checks: dropping ATTRIBUTE_PRXPTR from CellGemAttribute.spurs must
# fail the LP64 layout asserts and nothing else, and a wrong YUV411 size
# in cellGemGetVideoConvertSize() must fail the link on
# gem_helper_mismatch.
#
# usage: gem-header-surface-test.sh [--ps3dev DIR] [--include DIR]
#   --include  header tree under test (default: this tree's sdk/include);
#              only the two gem headers are overlaid on the installed SDK.
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
    echo "gem-header-surface: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
nm="$ps3dev/ppu/bin/powerpc64-ps3-elf-nm"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "gem-header-surface: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/ov/cell/gem"
cp "$inc/cell/gem.h" "$work/ov/cell/gem.h"
cp "$inc/cell/gem/libgem.h" "$work/ov/cell/gem/libgem.h"
src="$root/tests/sdk/gem-header-surface-test.c"
status=0

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -O2 -Wall -Wextra -Werror \
                    -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                echo "gem-header-surface: ok   compile $label"
            else
                echo "gem-header-surface: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

for hdr in cell/gem.h cell/gem/libgem.h; do
    for abi in "" -mlp64; do
        if printf '#include <%s>\n' "$hdr" | "$cc" -x c -std=gnu99 $abi -Wall -Wextra -Werror \
                -I"$work/ov" -I"$sdk/ppu/include" -c - -o "$work/alone.o" 2>"$work/alone.err"; then
            echo "gem-header-surface: ok   $hdr alone ${abi:-ilp32}"
        else
            echo "gem-header-surface: FAIL $hdr alone ${abi:-ilp32}"
            sed 's/^/    /' "$work/alone.err" | head -5; status=1
        fi
    done
done

# link_check <include dir> <abi> <label>: link the TU with -lgem_stub and
# require all 37 imports defined.  Leaves the linker output in $work/link.err.
link_check() {
    local dir=$1 abi=$2 label=$3 n
    if ! "$cc" $abi -O2 -Wall -Wextra -Werror -I"$dir" -I"$sdk/ppu/include" \
            "$src" -lgem_stub -o "$work/t.elf" > "$work/link.err" 2>&1; then
        echo "gem-header-surface: FAIL link $label"
        grep -E "undefined reference|error" "$work/link.err" | sed 's/^/    /' | head -6
        return 1
    fi
    n=$("$nm" "$work/t.elf" | awk '$3 ~ /^cellGem[A-Za-z]+$/ && ($2 == "T" || $2 == "D") { print $3 }' | sort -u | wc -l)
    if [ "$n" -eq 37 ]; then
        echo "gem-header-surface: ok   link $label: 37 cellGem imports resolve"
    else
        echo "gem-header-surface: FAIL link $label: $n of 37 cellGem imports defined"
        return 1
    fi
}
for abi in "" -mlp64; do
    link_check "$work/ov" "$abi" "${abi:-ilp32}" || status=1
done

# Self-check 1: an 8-byte spurs pointer under LP64 must be caught by the
# layout asserts (and ILP32, where pointers are 4 bytes anyway, must not).
mkdir -p "$work/bad1/cell/gem"
cp "$work/ov/cell/gem.h" "$work/bad1/cell/gem.h"
sed 's/^\(    CellSpurs \*spurs\) ATTRIBUTE_PRXPTR;/\1;/' "$work/ov/cell/gem/libgem.h" > "$work/bad1/cell/gem/libgem.h"
if cmp -s "$work/ov/cell/gem/libgem.h" "$work/bad1/cell/gem/libgem.h"; then
    echo "gem-header-surface: FAIL self-check 1 could not drop ATTRIBUTE_PRXPTR"; status=1
else
    i_rc=0; "$cc" -x c -std=gnu99 -O2 -I"$work/bad1" -I"$sdk/ppu/include" -c "$src" -o "$work/b1.o" 2>/dev/null || i_rc=$?
    l_rc=0; "$cc" -x c -std=gnu99 -mlp64 -O2 -I"$work/bad1" -I"$sdk/ppu/include" -c "$src" -o "$work/b1.o" 2>"$work/b1.err" || l_rc=$?
    if [ "$i_rc" -eq 0 ] && [ "$l_rc" -ne 0 ] && grep -q "offsetof(CellGemAttribute, spu_priorities) == 16" "$work/b1.err"; then
        echo "gem-header-surface: ok   self-check 1: an 8-byte PRX pointer under LP64 is caught"
    else
        echo "gem-header-surface: FAIL self-check 1: ilp32 rc=$i_rc lp64 rc=$l_rc"; status=1
    fi
fi

# Self-check 2: a wrong helper result must fail the link on the marker.
mkdir -p "$work/bad2/cell/gem"
cp "$work/ov/cell/gem.h" "$work/bad2/cell/gem.h"
sed 's|return vga + 2 \* (vga / 4);|return vga + (vga / 4);|' "$work/ov/cell/gem/libgem.h" > "$work/bad2/cell/gem/libgem.h"
if cmp -s "$work/ov/cell/gem/libgem.h" "$work/bad2/cell/gem/libgem.h"; then
    echo "gem-header-surface: FAIL self-check 2 could not plant the wrong size"; status=1
elif link_check "$work/bad2" "" "self-check 2" > /dev/null; then
    echo "gem-header-surface: FAIL self-check 2: a wrong YUV411 size linked"; status=1
elif grep -q "undefined reference to .gem_helper_mismatch" "$work/link.err" \
        && [ "$(grep -c 'undefined reference' "$work/link.err")" -ge 1 ] \
        && ! grep -q "undefined reference to .cellGem" "$work/link.err"; then
    echo "gem-header-surface: ok   self-check 2: a wrong helper result fails the link"
else
    echo "gem-header-surface: FAIL self-check 2 failed for the wrong reason"; status=1
fi

[ $status -eq 0 ] && echo "PASS: gem-header-surface-test"
exit $status
