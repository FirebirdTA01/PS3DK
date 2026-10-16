#!/usr/bin/env bash
# gcc-16.x 0073: on the Cell PPE the register allocator must not park values in LR.
#
# GCC 16 (unpatched) holds loop values in the link register under register pressure (mtlr rX / mflr rY with
# X, Y != 0, i.e. not the prologue's save through r0); moves to and from LR are slow on the PPE.  0073 prices
# LR/CTR moves above memory for the Cell tuning, as GCC already does for POWER6-POWER9.
#
#  0. self-test: a failing compiler, a failing objdump and a missing function are errors, never a count of 0;
#     with no argument, a compiler whose optimizer query fails is a failure, and only one that answers without
#     an ext-dce option (a GCC 12, which never showed this) is skipped;
#  1. cell-lr-spill/blend.c (a masked over-blend of packed 8-bit pixels) has no LR-as-register move in
#     blend_over_masked at -O2, -O3 and -Os, ILP32 and LP64 (ILP32 is where GCC 16 used LR).
#
# usage: cell-lr-spill-test.sh [--ppu-gcc <powerpc64-ps3-elf-gcc>]
#   With no argument: $PS3DEV's compiler; it skips without PS3DEV or on GCC 12.  A named compiler never skips.
#   Exit 0 pass (or skip), 1 fail, 2 usage or missing input.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
gcc=""
while [ $# -gt 0 ]; do
    case $1 in
        --ppu-gcc) gcc=$2; shift 2 ;;
        *) echo "usage: $0 [--ppu-gcc <gcc>]"; exit 2 ;;
    esac
done
if [ -z "$gcc" ]; then
    gcc="${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-gcc"
    if [ -z "${PS3DEV:-}" ] || [ ! -x "$gcc" ]; then
        echo "SKIP: cell-lr-spill-test (no PS3DEV compiler)"; exit 0
    fi
    # GCC 16 is told apart from GCC 12 by its ext-dce pass.  A failing query fails; only an answer without
    # ext-dce skips.
    if ! q=$(PS3DK_NO_DEFAULT_INCLUDE=1 "$gcc" -Q --help=optimizers 2>&1); then
        echo "cell-lr-spill: FAIL optimizer query failed: $gcc -Q --help=optimizers"
        printf '%s\n' "$q" | head -5 | sed 's/^/    /'
        echo "FAIL: cell-lr-spill-test"; exit 1
    fi
    if ! printf '%s\n' "$q" | grep -q -- '-fext-dce'; then
        echo "SKIP: cell-lr-spill-test ($gcc is not GCC 16)"; exit 0
    fi
fi
[ -x "$gcc" ] || { echo "cell-lr-spill: no compiler at '$gcc'"; exit 2; }
od="${gcc%gcc}objdump"
[ -x "$od" ] || { echo "cell-lr-spill: no objdump at '$od'"; exit 2; }
src="$root/tests/host/cell-lr-spill/blend.c"
[ -f "$src" ] || { echo "cell-lr-spill: missing $src"; exit 2; }

status=0
ok()   { echo "cell-lr-spill: ok   $*"; }
fail() { echo "cell-lr-spill: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
export PS3DK_NO_DEFAULT_INCLUDE=1

# lr_moves <object> <function>: sets $n to the mtlr/mflr instructions on r1-r31 in the function's code
# (ELFv1 function sections: "<.text.<function>>:"); returns 1 (n empty) if objdump fails or the function is
# not in the object exactly once.
lr_moves() {
    n=""
    "$od" -d "$1" > "$work/od.txt" 2>"$work/od.err" || return 1
    awk -v fn="$2" '$NF == ("<" fn ">:") || $NF == ("<." fn ">:") || $NF == ("<.text." fn ">:") {f=1; found++; next}
                    f && (/^$/ || /^Disassembly of section/) {f=0} f {print} END {exit found != 1}' \
        "$work/od.txt" > "$work/fn.txt" || return 1
    n=$(grep -cE '[[:space:]](mflr|mtlr)[[:space:]]+r?([1-9]|[12][0-9]|3[01])$' "$work/fn.txt")
    return 0
}

# 0: self-test.
printf 'int other(int x) { return x + 1; }\n' > "$work/other.c"
if "$gcc" -O2 -fno-such-flag-lr-selftest -c "$src" -o "$work/bad.o" > /dev/null 2>&1; then
    fail "self-test: an invalid flag compiled (the compiler status cannot be trusted)"
else ok "self-test: a failing compile is reported by its status"; fi
if "$gcc" -O2 -c "$work/other.c" -o "$work/other.o" > "$work/cc.log" 2>&1; then
    lr_moves "$work/other.o" blend_over_masked && fail "self-test: a missing function counted as $n" \
        || ok "self-test: a missing function is an error, not a count"
    lr_moves "$work/missing.o" blend_over_masked && fail "self-test: a missing object counted as $n" \
        || ok "self-test: a failing objdump is an error, not a count"
else
    fail "self-test: the control compile failed"; head -5 "$work/cc.log" | sed 's/^/    /'
fi
printf 'x:\n\tmtlr 10\n\tmflr 31\n\tmflr 0\n\tmtlr 0\n' > "$work/count.txt"
c=$(grep -cE '[[:space:]](mflr|mtlr)[[:space:]]+r?([1-9]|[12][0-9]|3[01])$' "$work/count.txt")
[ "$c" -eq 2 ] && ok "self-test: the pattern counts mtlr 10 / mflr 31 and not the r0 prologue save" \
    || fail "self-test: the pattern counted $c of the 2 LR-as-register moves"
mkdir -p "$work/broken/ppu/bin" "$work/old/ppu/bin"
printf '#!/bin/sh\necho "stand-in: cannot run" >&2\nexit 1\n' > "$work/broken/ppu/bin/powerpc64-ps3-elf-gcc"
printf '#!/bin/sh\necho "  -fgcse  [enabled]"\nexit 0\n' > "$work/old/ppu/bin/powerpc64-ps3-elf-gcc"
chmod +x "$work/broken/ppu/bin/powerpc64-ps3-elf-gcc" "$work/old/ppu/bin/powerpc64-ps3-elf-gcc"
out=$(PS3DEV="$work/broken" bash "${BASH_SOURCE[0]}" 2>&1); rc=$?
[ $rc -eq 1 ] && ! printf '%s\n' "$out" | grep -q '^SKIP' && ok "self-test: no-argument run with a failing query fails (rc 1)" \
    || fail "self-test: no-argument run with a failing query gave rc $rc: $(printf '%s' "$out" | tail -n 1)"
out=$(PS3DEV="$work/old" bash "${BASH_SOURCE[0]}" 2>&1); rc=$?
[ $rc -eq 0 ] && printf '%s\n' "$out" | grep -q '^SKIP' && ok "self-test: no-argument run with a GCC 12 stand-in skips (rc 0)" \
    || fail "self-test: no-argument run with a GCC 12 stand-in gave rc $rc: $(printf '%s' "$out" | tail -n 1)"

# 1
for abi in "" -mlp64; do
    for o in -O2 -O3 -Os; do
        if [ -n "$abi" ]; then label="LP64 $o"; else label="ILP32 $o"; fi
        if ! "$gcc" $abi $o -c "$src" -o "$work/blend.o" > "$work/cc.log" 2>&1; then
            fail "$label: compiler failed"; head -5 "$work/cc.log" | sed 's/^/    /'; continue
        fi
        if ! lr_moves "$work/blend.o" blend_over_masked; then
            fail "$label: no blend_over_masked in the disassembly (objdump failed or the function is missing)"; continue
        fi
        [ "$n" -eq 0 ] && ok "$label: no LR-as-register moves" \
            || { fail "$label: $n LR-as-register moves"; grep -E '(mflr|mtlr)' "$work/fn.txt" | head -4 | sed 's/^/    /'; }
    done
done

[ $status -eq 0 ] && echo "PASS: cell-lr-spill-test" || echo "FAIL: cell-lr-spill-test"
exit $status
