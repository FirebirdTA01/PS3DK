#!/usr/bin/env bash
# <cell/sysmodule.h> module ids, pinned by value in C and C++ under
# -Wall -Wextra -Werror (see sysmodule-ids-test.c for why they are ABI).
#
# usage: sysmodule-ids-test.sh [--host | --ps3dev DIR] [--include DIR]
#   --host     always run with CC/CXX (defaults cc/c++), without a PS3 SDK.
#              Only the memory-container dependency is stubbed; the real
#              sysmodule header and the same value assertions are compiled.
#   --include  header directory to test (default: this tree's sdk/include).
#              Point it at an older tree's headers to reproduce the failure.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"; host=0
while [ $# -gt 0 ]; do
    case "$1" in
        --host) host=1; shift ;;
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--host | --ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ "$host" -eq 0 ] && [ -z "$ps3dev" ]; then
    echo "sysmodule-ids: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
if [ "$host" -eq 0 ]; then
    cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
    [ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "sysmodule-ids: FAIL: no PPU compiler under $ps3dev"; exit 1; }
fi
src="$root/tests/sdk/sysmodule-ids-test.c"
# only the header under test goes on the path: a whole sdk/include beside an
# installed SDK puts two copies of the libc wrapper headers in the chain
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/cell" && cp "$inc/cell/sysmodule.h" "$work/cell/" || { echo "sysmodule-ids: FAIL: no cell/sysmodule.h under $inc"; exit 1; }
# CELL_OK comes from cell/error.h, which includes nothing further
cp "$inc/cell/error.h" "$work/cell/" || { echo "sysmodule-ids: FAIL: no cell/error.h under $inc"; exit 1; }
includes=(-I"$work")
if [ "$host" -eq 1 ]; then
    # Value checks only: do not pull PPU syscall assembly into a host TU.
    # This does not validate the target memory-container ABI or module loads.
    mkdir -p "$work/sys" || { echo "sysmodule-ids: FAIL: cannot create $work/sys"; exit 1; }
    printf '#include <stdint.h>\ntypedef uint32_t sys_mem_container_t;\n' > "$work/sys/memory.h" || { echo "sysmodule-ids: FAIL: cannot write $work/sys/memory.h"; exit 1; }
else
    # SDK header root: source build ($PS3DK=$ps3dev/ps3dk) and installed package
    # (PS3DK==PS3DEV, headers directly under $ps3dev/ppu/include) differ; resolve once.
    sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
    includes+=(-I"$sdk/ppu/include")
fi
status=0
for lang in c c++; do
    std=c11; [ "$lang" = c++ ] && std=c++17
    if [ "$host" -eq 1 ]; then
        cc="${CC:-cc}"; [ "$lang" = c++ ] && cc="${CXX:-c++}"
    fi
    if out=$("$cc" -x "$lang" -std="$std" -fsyntax-only -Wall -Wextra -Werror "${includes[@]}" "$src" 2>&1); then
        echo "sysmodule-ids: ok   $lang: every pinned id matches"
    else
        echo "sysmodule-ids: FAIL $lang:"
        printf '%s\n' "$out" | grep -E "error|static assert" | sed 's/^/    /' | head -40
        status=1
    fi
done
exit $status
