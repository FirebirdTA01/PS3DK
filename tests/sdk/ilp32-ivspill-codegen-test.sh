#!/usr/bin/env bash
# Codegen guard for GCC patch 0049: under the ILP32 data model the PPU compiler
# must not form an address by adding a 32-bit stack reload without truncating
# it (a negative IV bias then carries into bit 32 of the effective address).
#
# Compiles tests/regression/ilp32-ivspill/ivspill.cpp at -O2 and -O3, with and
# without -fivopts, and runs scripts/ilp32-address-lint.py on the result: zero
# findings required.  Since GCC patch 0051 forms ILP32 addresses in 64-bit
# registers, -fivopts no longer brings the shape back, so the red control is
# the v0.19.0-candidate (c13ee74f) listing of evolve() at -O3 -fivopts, kept
# in tests/sdk/data/ilp32-ivspill-red.dis: the scan must still report it.
# LP64 is compiled too and must be clean.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: ilp32-ivspill-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CXXFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "ilp32-ivspill-codegen: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
objdump="$ps3dev/ppu/bin/powerpc64-ps3-elf-objdump"
[ -x "$cxx" ] || { echo "ilp32-ivspill-codegen: FAIL: no compiler at $cxx"; exit 1; }
src="$root/tests/regression/ilp32-ivspill/ivspill.cpp"
lint="$root/scripts/ilp32-address-lint.py"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
status=0

findings() {  # <name> <flags...>: prints the finding count
    local name="$1"; shift
    "$cxx" ${EXTRA_CXXFLAGS:-} -mcpu=cell -mhard-float "$@" -c "$src" -o "$work/$name.o" || return 2
    "$objdump" -d -C "$work/$name.o" > "$work/$name.dis"
    python3 "$lint" "$work/$name.dis" --limit 0 | head -1 | sed -E 's/.*: ([0-9]+) hits.*/\1/'
}

for opt in -O2 -O3 "-O2 -fivopts" "-O3 -fivopts"; do
    n="$(findings "ilp32${opt// /}" $opt)"
    if [ "$n" = "0" ]; then
        echo "ilp32-ivspill-codegen: ok   ILP32 $opt: no unextended address formed from a reload"
    else
        echo "ilp32-ivspill-codegen: FAIL ILP32 $opt: $n finding(s)"; status=1
    fi
    n="$(findings "lp64${opt// /}" $opt -mlp64)"
    if [ "$n" = "0" ]; then
        echo "ilp32-ivspill-codegen: ok   LP64 $opt clean"
    else
        echo "ilp32-ivspill-codegen: FAIL LP64 $opt: $n finding(s)"; status=1
    fi
done

n="$(python3 "$lint" "$root/tests/sdk/data/ilp32-ivspill-red.dis" --limit 0 | head -1 | sed -E 's/.*: ([0-9]+) hits.*/\1/')"
if [ -n "$n" ] && [ "$n" -gt 0 ] 2>/dev/null; then
    echo "ilp32-ivspill-codegen: ok   red control: the c13ee74f listing is reported ($n findings)"
else
    echo "ilp32-ivspill-codegen: FAIL red control: the scan no longer reports the c13ee74f listing"
    status=1
fi

[ $status -eq 0 ] && echo "ilp32-ivspill-codegen: PASS" || echo "ilp32-ivspill-codegen: FAIL"
exit $status
