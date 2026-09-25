#!/usr/bin/env bash
# spu-thread-events-test.sh — verifies SPU thread event constants and function signatures
# across both ABIs (ilp32, lp64), C (-std=gnu99, -std=c11) and C++17.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=${LV2_INCLUDE_DIR:-"$root/sdk/include"}
src="$root/tests/sdk/spu-thread-events-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true
checks=0

PPU_CC=${PPU_CC:-powerpc64-ps3-elf-gcc}
PPU_CXX=${PPU_CXX:-powerpc64-ps3-elf-g++}

if ! command -v "$PPU_CC" >/dev/null 2>&1; then
    if [ -n "${PS3DEV:-}" ] && [ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]; then
        PPU_CC="$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc"
        PPU_CXX="$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++"
    fi
fi

stage_inc=${PS3DK_INC:-${PS3DEV:+$PS3DEV/ps3dk/ppu/include}}
inc_flags=(-I"$inc" -D__PS3DK_SDK_SELFBUILD__)
if [ -n "${stage_inc:-}" ] && [ -d "$stage_inc" ]; then
    inc_flags+=(-isystem "$stage_inc")
fi

# 1. Cross compiler checks for both ABIs
for abi in ilp32 lp64; do
    flags=()
    if [ "$abi" = lp64 ]; then flags=(-mlp64); fi

    # C -std=gnu99
    if command -v "$PPU_CC" >/dev/null 2>&1; then
        "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_gnu99.o"
        echo "spu-thread-events: PASS PPU C (gnu99) $abi"
        checks=$((checks + 1))

        # C -std=c11
        "$PPU_CC" -x c -std=c11 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_c11.o"
        echo "spu-thread-events: PASS PPU C (c11) $abi"
        checks=$((checks + 1))
    fi

    # C++ -std=c++17
    if command -v "$PPU_CXX" >/dev/null 2>&1; then
        "$PPU_CXX" -x c++ -std=c++17 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_cxx17.o"
        echo "spu-thread-events: PASS PPU C++ (c++17) $abi"
        checks=$((checks + 1))
    fi
done

if [ "$checks" = 0 ]; then
    echo "spu-thread-events: SKIP (PPU cross-compiler unavailable)"
    exit 0
fi
echo "spu-thread-events: ALL PASS ($checks configurations checked)"
