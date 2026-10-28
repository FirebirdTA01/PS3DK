#!/usr/bin/env bash
# ld's TOC edit must not move a reference onto the wrong entry of an ILP32
# .toc (binutils patch 0008).  An ILP32 .toc mixes 4-byte address words with
# 8-byte entries that are only 4-byte aligned, and the compiler reads such an
# entry's address with lwz at entry + 4.  ld edits the TOC in 8-byte steps;
# before 0008 a dead 4-byte word sharing a step with the first half of an
# 8-byte entry let ld drop that step and shift the lwz reference onto another
# entry, so the program loaded a wrong address.
#
# Fixture (toc-edit-ilp32/): draw() reads &g_flat.fragment through the low
# word of an 8-byte entry at 4 mod 8; first() is the only user of the 4-byte
# entry in front of it and --gc-sections removes it; sky() uses the same
# entry with addis/ld.  The fixture's shape is checked first: without it the
# rows below cannot fail.
# Rows, each compared with the same objects linked --no-toc-optimize:
#   ilp32-gc         -Wl,--gc-sections                    (wrong address before 0008)
#   ilp32-gc-nokeep  the same with -Wl,--no-keep-memory   (uncached reloc reads)
#   ilp32            no --gc-sections
#   lp64-gc          -mlp64 -Wl,--gc-sections
# An ILP32 row passes when draw, sky and passes make exactly the TOC loads of
# the unedited link, with the same counts and the same symbol+offset each:
# 0008 leaves this fixture's .toc unedited, so nothing may disappear.  The
# LP64 .toc is still edited, which may turn an addis/ld into addis/addi, so
# the LP64 row only requires every load that remains to read what the
# unedited link reads; it is a smoke check, not an address-equivalence
# proof (compare base and candidate ld outputs byte for byte for that).
# The checker's comparisons run on doctored inputs first (selftest).
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: toc-edit-ilp32-test.sh [--ps3dev DIR] [-B DIR]
#   -B DIR is passed to the driver (repeatable), e.g. a directory holding a
#   candidate ld.
set -u
ps3dev="${PS3DEV:-}"; B=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        -B) B+=("-B$2"); shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [-B DIR]..." >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "toc-edit-ilp32: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
bin="$ps3dev/ppu/bin/powerpc64-ps3-elf"
exe=""; [ -x "$bin-gcc" ] || exe=".exe"
cc="$bin-gcc$exe"; od="$bin-objdump$exe"; nm="$bin-nm$exe"
[ -x "$cc" ] || { echo "toc-edit-ilp32: FAIL: no compiler at $bin-gcc"; exit 1; }
py=python3; command -v $py > /dev/null 2>&1 || py=python
here="$(cd "$(dirname "$0")" && pwd)"
check="$here/toc-edit-ilp32-check.py"
src="$here/toc-edit-ilp32"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Windows-hosted drivers need mixed paths.
if command -v cygpath > /dev/null 2>&1; then w() { cygpath -m "$1"; }; else w() { printf '%s' "$1"; }; fi

for abi in ilp32 lp64; do
    A=(); [ "$abi" = lp64 ] && A=(-mlp64)
    for s in a b main; do
        "$cc" "${B[@]}" "${A[@]}" -O3 -fdata-sections -ffunction-sections \
            -c "$(w "$src/$s.c")" -o "$(w "$work/$abi-$s.o")" \
            || { echo "toc-edit-ilp32: FAIL: compile $abi $s.c"; exit 1; }
    done
done

"$od" -r "$(w "$work/ilp32-a.o")" > "$work/ilp32-a.relocs" \
    || { echo "toc-edit-ilp32: FAIL: objdump -r"; exit 1; }
if ! "$py" "$check" shape "$work/ilp32-a.relocs"; then
    echo "toc-edit-ilp32: FAIL: fixture lost its shape (no 8-byte .toc entry at 4 mod 8 read at entry+4)"
    exit 1
fi
echo "toc-edit-ilp32: ok   fixture shape (8-byte .toc entry at 4 mod 8, read at entry+4)"
"$py" "$check" selftest || { echo "toc-edit-ilp32: FAIL: checker selftest"; exit 1; }

fail=0
row() {  # name abi exact|subset ldflags...
    local name="$1" abi="$2" how="$3"; shift 3
    local A=(); [ "$abi" = lp64 ] && A=(-mlp64)
    local objs=("$(w "$work/$abi-a.o")" "$(w "$work/$abi-b.o")" "$(w "$work/$abi-main.o")")
    if ! "$cc" "${B[@]}" "${A[@]}" "$@" "${objs[@]}" -o "$(w "$work/$name.elf")" \
        || ! "$cc" "${B[@]}" "${A[@]}" "$@" -Wl,--no-toc-optimize "${objs[@]}" -o "$(w "$work/$name-ctl.elf")"; then
        echo "toc-edit-ilp32: FAIL $name: link"; fail=1; return
    fi
    if ! "$py" "$check" loads "$od" "$nm" "$(w "$work/$name.elf")" draw sky passes > "$work/$name.loads" \
        || ! "$py" "$check" loads "$od" "$nm" "$(w "$work/$name-ctl.elf")" draw sky passes > "$work/$name-ctl.loads"; then
        echo "toc-edit-ilp32: FAIL $name: checker"; fail=1; return
    fi
    if ! grep -q . "$work/$name-ctl.loads"; then
        echo "toc-edit-ilp32: FAIL $name: the unedited link has no TOC loads to compare"; fail=1; return
    fi
    if "$py" "$check" "$how" "$work/$name.loads" "$work/$name-ctl.loads"; then
        if [ "$how" = exact ]; then
            echo "toc-edit-ilp32: ok   $name (the same $(grep -c . "$work/$name.loads") TOC loads as the --no-toc-optimize link)"
        else
            echo "toc-edit-ilp32: ok   $name ($(grep -c . "$work/$name.loads") of $(grep -c . "$work/$name-ctl.loads") loads remain; each reads what the --no-toc-optimize link reads)"
        fi
    else
        echo "toc-edit-ilp32: FAIL $name: the TOC loads differ from the --no-toc-optimize link"; fail=1
    fi
}
row ilp32-gc        ilp32 exact  -Wl,--gc-sections
row ilp32-gc-nokeep ilp32 exact  -Wl,--gc-sections -Wl,--no-keep-memory
row ilp32           ilp32 exact
row lp64-gc         lp64  subset -Wl,--gc-sections
if [ $fail -eq 0 ]; then echo "toc-edit-ilp32: PASS"; else echo "toc-edit-ilp32: FAIL"; fi
exit $fail
