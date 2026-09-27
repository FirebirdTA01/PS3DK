#!/usr/bin/env bash
# <cell/sysmodule.h> module ids, pinned by value in C and C++ under
# -Wall -Wextra -Werror (see sysmodule-ids-test.c for why they are ABI).
#
# usage: sysmodule-ids-test.sh [--ps3dev DIR] [--include DIR]
#   --include  header directory to test (default: this tree's sdk/include).
#              Point it at an older tree's headers to reproduce the failure.
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
    echo "sysmodule-ids: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "sysmodule-ids: FAIL: no PPU compiler under $ps3dev"; exit 1; }
src="$root/tests/sdk/sysmodule-ids-test.c"
# only the header under test goes on the path: a whole sdk/include beside an
# installed SDK puts two copies of the libc wrapper headers in the chain
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/cell" && cp "$inc/cell/sysmodule.h" "$work/cell/" || { echo "sysmodule-ids: FAIL: no cell/sysmodule.h under $inc"; exit 1; }
status=0
for lang in c c++; do
    std=c11; [ "$lang" = c++ ] && std=c++17
    if out=$("$cc" -x "$lang" -std="$std" -fsyntax-only -Wall -Wextra -Werror -I"$work" -I"$ps3dev/ppu/include" "$src" 2>&1); then
        echo "sysmodule-ids: ok   $lang: every pinned id matches"
    else
        echo "sysmodule-ids: FAIL $lang:"
        printf '%s\n' "$out" | grep -E "error|static assert" | sed 's/^/    /' | head -40
        status=1
    fi
done
exit $status
