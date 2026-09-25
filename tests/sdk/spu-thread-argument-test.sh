#!/usr/bin/env bash
# spu-thread-argument-test.sh — verifies canonical sys_spu_thread_argument_t
# (arg1..arg4) and legacy sysSpuThreadArgument (arg0..arg3) across both ABIs
# (ilp32, lp64), C (-std=gnu99, -std=c11), and C++17.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=${LV2_INCLUDE_DIR:-"$root/sdk/include"}
stage_inc=${STAGE_INCLUDE_DIR:-${PS3DK_INC:-${PS3DEV:+$PS3DEV/ps3dk/ppu/include}}}
src="$root/tests/sdk/spu-thread-argument-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true
host_checks=0
ppu_checks=0
ppu_skips=0

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

# 1. Host compiler execution checks (using test fixture for mock sys/spu.h)
HOST_CC=${CC:-gcc}
HOST_CXX=${CXX:-g++}
fixture_inc="$root/tests/sdk/fixtures/spu-arg-fixture"

if command -v "$HOST_CC" >/dev/null 2>&1; then
    "$HOST_CC" -x c -std=gnu99 -Wall -Wextra -Werror -I"$fixture_inc" -I"$inc" "$src" -o "$tmp/host_gnu99"
    "$tmp/host_gnu99"
    echo "spu-thread-arg: PASS host C (gnu99)"
    host_checks=$((host_checks + 1))

    "$HOST_CC" -x c -std=c11 -Wall -Wextra -Werror -I"$fixture_inc" -I"$inc" "$src" -o "$tmp/host_c11"
    "$tmp/host_c11"
    echo "spu-thread-arg: PASS host C (c11)"
    host_checks=$((host_checks + 1))
else
    echo "spu-thread-arg: SKIP host C ($HOST_CC not found)"
fi

if command -v "$HOST_CXX" >/dev/null 2>&1; then
    "$HOST_CXX" -x c++ -std=c++17 -Wall -Wextra -Werror -I"$fixture_inc" -I"$inc" "$src" -o "$tmp/host_cxx17"
    "$tmp/host_cxx17"
    echo "spu-thread-arg: PASS host C++ (c++17)"
    host_checks=$((host_checks + 1))
else
    echo "spu-thread-arg: SKIP host C++ ($HOST_CXX not found)"
fi

# 2. Cross compiler checks for both ABIs
for abi in ilp32 lp64; do
    flags=()
    if [ "$abi" = lp64 ]; then flags=(-mlp64); fi

    if command -v "$PPU_CC" >/dev/null 2>&1; then
        "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_gnu99.o"
        echo "spu-thread-arg: PASS PPU C (gnu99) $abi"
        ppu_checks=$((ppu_checks + 1))

        "$PPU_CC" -x c -std=c11 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_c11.o"
        echo "spu-thread-arg: PASS PPU C (c11) $abi"
        ppu_checks=$((ppu_checks + 1))
    else
        echo "spu-thread-arg: SKIP PPU C (gnu99) $abi ($PPU_CC not found)"
        echo "spu-thread-arg: SKIP PPU C (c11) $abi ($PPU_CC not found)"
        ppu_skips=$((ppu_skips + 2))
    fi

    if command -v "$PPU_CXX" >/dev/null 2>&1; then
        "$PPU_CXX" -x c++ -std=c++17 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_cxx17.o"
        echo "spu-thread-arg: PASS PPU C++ (c++17) $abi"
        ppu_checks=$((ppu_checks + 1))
    else
        echo "spu-thread-arg: SKIP PPU C++ (c++17) $abi ($PPU_CXX not found)"
        ppu_skips=$((ppu_skips + 1))
    fi
done

total_passed=$((host_checks + ppu_checks))
if [ "$total_passed" -eq 0 ]; then
    echo "spu-thread-arg: FAIL no compilers available" >&2
    exit 1
fi

if [ "$ppu_skips" -gt 0 ]; then
    echo "spu-thread-arg: PASS (host only: $host_checks passed, $ppu_skips cross skipped - cross compiler not found)"
else
    echo "spu-thread-arg: ALL PASS ($total_passed configurations checked)"
fi
