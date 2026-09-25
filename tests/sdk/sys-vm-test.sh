#!/usr/bin/env bash
# sys-vm-test.sh — verifies <sys/vm.h> constants, struct layout, and function signatures
# across both ABIs (ilp32, lp64), C (-std=gnu99, -std=c11) and C++17.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=${LV2_INCLUDE_DIR:-"$root/sdk/include"}
stage_inc=${STAGE_INCLUDE_DIR:-${PS3DK_INC:-${PS3DEV:+$PS3DEV/ps3dk/ppu/include}}}
src="$root/tests/sdk/sys-vm-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true
checks=0

PPU_CC=${PPU_CC:-powerpc64-ps3-elf-gcc}
PPU_CXX=${PPU_CXX:-powerpc64-ps3-elf-g++}

if ! command -v "$PPU_CC" >/dev/null 2>&1; then
    if [ -n "${PS3DEV:-}" ] && [ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]; then
        PPU_CC="$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc"
    fi
fi
if ! command -v "$PPU_CXX" >/dev/null 2>&1; then
    if [ -n "${PS3DEV:-}" ] && [ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++" ]; then
        PPU_CXX="$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++"
    fi
fi

inc_flags=(-I"$inc" -D__PS3DK_SDK_SELFBUILD__)
if [ -d "$stage_inc" ]; then
    inc_flags+=(-isystem "$stage_inc")
fi

# 1. Host compiler syntax checks (run in CI and on development hosts)
HOST_CC=${CC:-gcc}
HOST_CXX=${CXX:-g++}

if command -v "$HOST_CC" >/dev/null 2>&1; then
    "$HOST_CC" -x c -std=gnu99 -Wall -Wextra -Werror -I"$inc" -fsyntax-only "$src"
    echo "sys-vm: PASS host C (gnu99)"
    checks=$((checks + 1))

    "$HOST_CC" -x c -std=c11 -Wall -Wextra -Werror -I"$inc" -fsyntax-only "$src"
    echo "sys-vm: PASS host C (c11)"
    checks=$((checks + 1))
fi

if command -v "$HOST_CXX" >/dev/null 2>&1; then
    "$HOST_CXX" -x c++ -std=c++17 -Wall -Wextra -Werror -I"$inc" -fsyntax-only "$src"
    echo "sys-vm: PASS host C++ (c++17)"
    checks=$((checks + 1))
fi

# 2. Cross compiler checks for both ABIs
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

if [ "$checks" -eq 0 ]; then
    echo "sys-vm: FAIL no compilers available" >&2
    exit 1
fi
echo "sys-vm: ALL PASS ($checks configurations checked)"
