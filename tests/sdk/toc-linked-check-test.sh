#!/usr/bin/env bash
# tools/toc-linked-check.py on real PPU links (the linked TOC check).
#
# Rows, each a capture (link with map and trace) then a check:
#   ilp32-gc       toc-edit-ilp32 fixture, --gc-sections
#   ilp32-gc-ctl   the same with --no-toc-optimize
#   lp64-gc        the fixture with -mlp64 --gc-sections
#   cxx-ilp32      a C++ program (libstdc++, libc)
#   cxx-lp64       the same with -mlp64
# With --red-ps3dev DIR (a toolchain whose ld lacks binutils patch 0008):
#   red-ilp32-gc     the ilp32-gc row on that ld: must FAIL at draw()'s lwz, and only there
#   red-ilp32-gc-ctl its --no-toc-optimize control
# Every row is pinned in toc-linked-check/known-refusals-<row>.txt: its
# verified, refused and failed site counts and the exact set of refusal and
# failure details (see known-refusals.py).  A row passes only when its check
# matches the pin exactly; a refusal the pin does not list, or a pinned one
# that went away, fails the row.  The refusals are the checker's documented
# coverage limits (--help), not accepted defects.
#
# --regen rewrites the pins from this run instead of checking them: for a
# reviewed re-pin after a toolchain or checker change; never in CI.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: toc-linked-check-test.sh [--ps3dev DIR] [--red-ps3dev DIR] [--regen]
#   TOC_LINKED_KEEP=DIR keeps every capture and check output in DIR.
set -u
ps3dev="${PS3DEV:-}"; red=""; regen=0
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --red-ps3dev) red="$2"; shift 2 ;;
        --regen) regen=1; shift ;;
        *) echo "usage: $0 [--ps3dev DIR] [--red-ps3dev DIR] [--regen]" >&2; exit 2 ;;
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
pins="$here/toc-linked-check"
fixture="$here/toc-edit-ilp32"
# TOC_LINKED_KEEP=DIR keeps the captures and checks in DIR (a new directory).
if [ -n "${TOC_LINKED_KEEP:-}" ]; then
    work="$TOC_LINKED_KEEP"
    mkdir "$work" || { echo "toc-linked-check: FAIL: $work exists or cannot be made"; exit 1; }
else
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
fi
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
row() {  # name driver abi objs ldflags...
    local name="$1" drv="$2" abi="$3" objs="$4"; shift 4
    local A=(); [ "$abi" = lp64 ] && A=(-mlp64)
    local in=() o
    for o in $objs; do in+=("$abi-$o.o"); done
    # Capture from inside the work dir so the manifest names short relative paths.
    if ! (cd "$work" && "$py" "$checker" capture --out "$name" -- "$drv" "${A[@]}" "$@" "${in[@]}" -o "$name.elf") \
            > "$work/$name.capture" 2>&1; then
        echo "toc-linked-check: FAIL $name: capture"; cat "$work/$name.capture"; fail=1; return
    fi
    "$py" "$checker" check "$(w "$work/$name/manifest.json")" --readelf "$(w "$readelf")" \
        --report "$(w "$work/$name.json")" -v > "$work/$name.check" 2>&1
    local rc=$?
    local counts; counts="$(awk '$1 ~ /^sites_(verified|failed|refused|discarded)$/ {printf "%s %s, ", substr($1, 7), $2}' "$work/$name.check")"
    local pin="$pins/known-refusals-$name.txt" got krc
    if [ $regen -eq 1 ]; then
        "$py" "$pins/known-refusals.py" --regen "$(w "$work/$name.json")" "$(w "$pin")" || fail=1
        return
    fi
    got="$("$py" "$pins/known-refusals.py" "$(w "$work/$name.json")" "$(w "$pin")" 2>&1)"
    krc=$?
    # The checker's exit status must agree with the pinned status (0 only for PASS).
    local want_rc=1
    grep -q '^refused \|^failed ' "$pin" 2> /dev/null || want_rc=0
    if [ $krc -eq 0 ] && [ $rc -eq $want_rc ]; then
        echo "toc-linked-check: ok   $name ($got) (${counts%, })"
    else
        echo "toc-linked-check: FAIL $name: rc=$rc, the check does not match its pin"
        echo "$got" | sed 's/^/    /'
        fail=1
    fi
}
fx="a b main"
row ilp32-gc     "$(w "$cc")"  ilp32 "$fx" -Wl,--gc-sections
row ilp32-gc-ctl "$(w "$cc")"  ilp32 "$fx" -Wl,--gc-sections -Wl,--no-toc-optimize
row lp64-gc      "$(w "$cc")"  lp64  "$fx" -Wl,--gc-sections
row cxx-ilp32    "$(w "$cxx")" ilp32 cxx   -Wl,--gc-sections
row cxx-lp64     "$(w "$cxx")" lp64  cxx   -Wl,--gc-sections
if [ -n "$red" ]; then
    rcc="$(tool_of "$red" gcc)"
    [ -x "$rcc" ] || { echo "toc-linked-check: FAIL: no $rcc"; exit 1; }
    row red-ilp32-gc     "$(w "$rcc")" ilp32 "$fx" -Wl,--gc-sections
    row red-ilp32-gc-ctl "$(w "$rcc")" ilp32 "$fx" -Wl,--gc-sections -Wl,--no-toc-optimize
    # The RED row's pin must hold exactly one failure, draw()'s lwz.
    if [ $regen -eq 0 ]; then
        nfail="$(grep -c '^failed ' "$pins/known-refusals-red-ilp32-gc.txt")"
        if [ "$nfail" != 1 ] || ! grep -q '^failed ilp32-a\.o \.text\.draw+0x48: ' "$pins/known-refusals-red-ilp32-gc.txt"; then
            echo "toc-linked-check: FAIL red-ilp32-gc: its pin must hold exactly the failure at ilp32-a.o .text.draw+0x48"
            fail=1
        fi
    fi
else
    echo "toc-linked-check: note: no --red-ps3dev, the RED row did not run"
fi
if [ $regen -eq 1 ]; then
    echo "toc-linked-check: pins regenerated (review every changed line before committing)"
elif [ $fail -eq 0 ]; then
    echo "toc-linked-check: PASS"
else
    echo "toc-linked-check: FAIL"
fi
exit $fail
