#!/usr/bin/env bash
# tools/toc-linked-check.py on real PPU links (the linked TOC check).
#
# Rows, each a capture (link with map and trace) then a check:
#   ilp32-gc       toc-edit-ilp32 fixture, --gc-sections         PASS
#   ilp32-gc-ctl   the same with --no-toc-optimize                PASS
#   lp64-gc        the fixture with -mlp64 --gc-sections          PASS
#   cxx-ilp32      a C++ program (libstdc++, libc)                PASS
#   cxx-lp64       the same with -mlp64                           PASS
# With --red-ps3dev DIR (a toolchain whose ld lacks binutils patch 0008):
#   red-ilp32-gc     the ilp32-gc row on that ld must FAIL, at draw()'s lwz
#   red-ilp32-gc-ctl its --no-toc-optimize control must PASS
# PASS means every TOC access the inputs describe was verified (nothing
# failed, refused or unaccounted); it says nothing about other code.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: toc-linked-check-test.sh [--ps3dev DIR] [--red-ps3dev DIR]
set -u
ps3dev="${PS3DEV:-}"; red=""
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --red-ps3dev) red="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--red-ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "toc-linked-check: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
tool_of() {  # dir tool -> path
    local p="$1/ppu/bin/powerpc64-ps3-elf-$2"
    [ -x "$p" ] || p="$p.exe"
    printf '%s' "$p"
}
cc="$(tool_of "$ps3dev" gcc)"; cxx="$(tool_of "$ps3dev" g++)"; readelf="$(tool_of "$ps3dev" readelf)"
for t in "$cc" "$cxx" "$readelf"; do
    [ -x "$t" ] || { echo "toc-linked-check: FAIL: no $t"; exit 1; }
done
py=python3; command -v $py > /dev/null 2>&1 || py=python
here="$(cd "$(dirname "$0")" && pwd)"
checker="$here/../../tools/toc-linked-check.py"
fixture="$here/toc-edit-ilp32"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Windows-hosted drivers need mixed paths.
if command -v cygpath > /dev/null 2>&1; then w() { cygpath -m "$1"; }; else w() { printf '%s' "$1"; }; fi

for abi in ilp32 lp64; do
    A=(); [ "$abi" = lp64 ] && A=(-mlp64)
    for s in a b main; do
        "$cc" "${A[@]}" -O3 -fdata-sections -ffunction-sections \
            -c "$(w "$fixture/$s.c")" -o "$(w "$work/$abi-$s.o")" \
            || { echo "toc-linked-check: FAIL: compile $abi $s.c"; exit 1; }
    done
    "$cxx" "${A[@]}" -O2 -ffunction-sections -fdata-sections \
        -c "$(w "$here/toc-linked-check/cxx.cpp")" -o "$(w "$work/$abi-cxx.o")" \
        || { echo "toc-linked-check: FAIL: compile $abi cxx.cpp"; exit 1; }
done

fail=0
row() {  # name want(PASS|FAIL) driver abi objs ldflags...
    local name="$1" want="$2" drv="$3" abi="$4" objs="$5"; shift 5
    local A=(); [ "$abi" = lp64 ] && A=(-mlp64)
    local in=() o
    for o in $objs; do in+=("$abi-$o.o"); done
    # Capture from inside the work dir so the manifest names short relative paths.
    if ! (cd "$work" && "$py" "$checker" capture --out "$name" -- "$drv" "${A[@]}" "$@" "${in[@]}" -o "$name.elf") \
            > "$work/$name.capture" 2>&1; then
        echo "toc-linked-check: FAIL $name: capture"; cat "$work/$name.capture"; fail=1; return
    fi
    "$py" "$checker" check "$(w "$work/$name/manifest.json")" --readelf "$(w "$readelf")" > "$work/$name.check" 2>&1
    local rc=$?
    local status; status="$(sed -n 's/^toc-linked-check: \(PASS\|FAIL\|INCOMPLETE\)$/\1/p' "$work/$name.check")"
    local counts; counts="$(awk '$1 ~ /^sites_(verified|failed|refused|discarded)$/ {printf "%s %s, ", substr($1, 7), $2}' "$work/$name.check")"
    if [ "$want" = PASS ] && [ $rc -eq 0 ] && [ "$status" = PASS ]; then
        echo "toc-linked-check: ok   $name PASS (${counts%, })"
    elif [ "$want" = FAIL ] && [ $rc -eq 1 ] && [ "$status" = FAIL ] \
            && grep -q 'FAILED .*a\.o \.text\.draw+' "$work/$name.check"; then
        echo "toc-linked-check: ok   $name FAIL as required (${counts%, })"
        grep 'FAILED' "$work/$name.check" | sed 's/^/    /'
    else
        echo "toc-linked-check: FAIL $name: rc=$rc status=${status:-none}, expected $want"
        sed 's/^/    /' "$work/$name.check"; fail=1
    fi
}
fx="a b main"
row ilp32-gc     PASS "$(w "$cc")"  ilp32 "$fx" -Wl,--gc-sections
row ilp32-gc-ctl PASS "$(w "$cc")"  ilp32 "$fx" -Wl,--gc-sections -Wl,--no-toc-optimize
row lp64-gc      PASS "$(w "$cc")"  lp64  "$fx" -Wl,--gc-sections
row cxx-ilp32    PASS "$(w "$cxx")" ilp32 cxx   -Wl,--gc-sections
row cxx-lp64     PASS "$(w "$cxx")" lp64  cxx   -Wl,--gc-sections
if [ -n "$red" ]; then
    rcc="$(tool_of "$red" gcc)"
    [ -x "$rcc" ] || { echo "toc-linked-check: FAIL: no $rcc"; exit 1; }
    row red-ilp32-gc     FAIL "$(w "$rcc")" ilp32 "$fx" -Wl,--gc-sections
    row red-ilp32-gc-ctl PASS "$(w "$rcc")" ilp32 "$fx" -Wl,--gc-sections -Wl,--no-toc-optimize
else
    echo "toc-linked-check: note: no --red-ps3dev, the RED row did not run"
fi
if [ $fail -eq 0 ]; then echo "toc-linked-check: PASS"; else echo "toc-linked-check: FAIL"; fi
exit $fail
