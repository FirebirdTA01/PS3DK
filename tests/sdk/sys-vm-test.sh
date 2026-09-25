#!/usr/bin/env bash
# sys-vm-test.sh — verifies <sys/vm.h> constants, struct layout, and function signatures
# across both ABIs (ilp32, lp64), C (-std=gnu99, -std=c11) and C++17.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=${LV2_INCLUDE_DIR:-"$root/sdk/include"}
stage_inc=${PS3DK_INC:-/home/firebirdta01/ps3tc/gemini-stage/ps3dk/ppu/include}
src="$root/tests/sdk/sys-vm-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true
checks=0

PPU_CC=${PPU_CC:-powerpc64-ps3-elf-gcc}
PPU_CXX=${PPU_CXX:-powerpc64-ps3-elf-g++}

inc_flags=(-I"$inc" -D__PS3DK_SDK_SELFBUILD__)
if [ -d "$stage_inc" ]; then
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
        echo "sys-vm: PASS PPU C (gnu99) $abi"
        checks=$((checks + 1))

        # C -std=c11
        "$PPU_CC" -x c -std=c11 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_c11.o"
        echo "sys-vm: PASS PPU C (c11) $abi"
        checks=$((checks + 1))
    fi

    # C++ -std=c++17
    if command -v "$PPU_CXX" >/dev/null 2>&1; then
        "$PPU_CXX" -x c++ -std=c++17 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_cxx17.o"
        echo "sys-vm: PASS PPU C++ (c++17) $abi"
        checks=$((checks + 1))
    fi
done

if [ "$checks" = 0 ]; then
    echo "sys-vm: no compilers available" >&2
    exit 77
fi
echo "sys-vm: ALL PASS ($checks configurations checked)"
