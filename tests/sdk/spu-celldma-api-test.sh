#!/usr/bin/env bash
# The SPU <cell/dma.h> surface: every cellDma* name, compiled as C (gnu11) and
# C++ (c++17) under -Wall -Wextra -Werror at -O0 and -O2, in each check mode
# (default, -DNO_CELL_DMA_ASSERT, -DCELL_DMA_ASSERT_VERBOSE), and linked
# against libdma built from this tree's sources.
#
# usage: spu-celldma-api-test.sh [--ps3dev DIR] [--include DIR] [--src DIR]
#   --include  directory holding cell/dma.h (default: this tree's sdk/include-spu)
#   --src      libdma sources (default: this tree's sdk/libdma/src)
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include-spu"; src="$root/sdk/libdma/src"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        --src) src="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR] [--src DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "spu-celldma-api: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/spu/bin/spu-elf-gcc"; ar="$ps3dev/spu/bin/spu-elf-ar"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "spu-celldma-api: FAIL: no SPU compiler under $ps3dev"; exit 1; }
# SDK header root: source build ($PS3DK=$ps3dev/ps3dk) and installed package
# (PS3DK==PS3DEV, headers directly under $ps3dev/spu/include) differ; resolve once.
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/spu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
# only the header under test goes ahead of the installed SPU headers
mkdir -p "$work/inc/cell" && cp "$inc/cell/dma.h" "$work/inc/cell/" || { echo "spu-celldma-api: FAIL: no cell/dma.h under $inc"; exit 1; }
status=0
for f in dma_and_wait dma_large_cmd dma_unaligned_cmd; do
    "$cc" -O2 -Wall -Wextra -Werror -c "$src/$f.c" -o "$work/$f.o" > "$work/lib.log" 2>&1 \
        || { echo "spu-celldma-api: FAIL: libdma $f.c"; sed 's/^/    /' "$work/lib.log" | head; exit 1; }
done
"$ar" rcs "$work/libdma.a" "$work"/dma_*.o
probe="$root/tests/sdk/spu-celldma-api-test.c"
for lang in c c++; do
    std=-std=gnu11; [ "$lang" = c++ ] && std=-std=c++17
    for opt in -O0 -O2; do
        for mode in default NO_CELL_DMA_ASSERT CELL_DMA_ASSERT_VERBOSE; do
            def=; [ "$mode" != default ] && def="-D$mode"
            label="$lang $opt $mode"
            if out=$("$cc" -x "$lang" "$std" "$opt" $def -Wall -Wextra -Werror -I"$work/inc" \
                    -I"$sdk/spu/include" \
                    "$probe" -x none -L"$work" -ldma -o "$work/probe.elf" 2>&1); then
                echo "spu-celldma-api: ok   $label"
            else
                echo "spu-celldma-api: FAIL $label"
                printf '%s\n' "$out" | grep -E "error|undefined" | sed 's/^/    /' | head -12
                status=1
            fi
        done
    done
done
exit $status
