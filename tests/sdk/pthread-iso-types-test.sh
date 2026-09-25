#!/usr/bin/env bash
# pthread-iso-types-test.sh — Test matrix for pthread types under strict-ISO and GNU modes
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SDK_INC="$ROOT_DIR/sdk/include"

checks=0

ORDERS=(
    "TEST_ORDER_SYS_TYPES_THEN_PTHREAD"
    "TEST_ORDER_PTHREAD_THEN_SYS_TYPES"
)

# 1. Host compiler checks (run in CI and on development hosts)
HOST_CC=${CC:-gcc}
HOST_CXX=${CXX:-g++}

C_MODES=("-std=c99" "-std=c11" "-std=gnu99" "-std=gnu11")
CXX_MODES=("-std=c++11" "-std=c++14" "-std=c++17")

if command -v "$HOST_CC" >/dev/null 2>&1; then
    for order in "${ORDERS[@]}"; do
        for c_mode in "${C_MODES[@]}"; do
            "$HOST_CC" $c_mode -Wall -Wextra -Werror -fsyntax-only \
                -isystem "$SDK_INC" -D"$order"=1 \
                "$SCRIPT_DIR/pthread-iso-types-test.c"
            checks=$((checks + 1))
        done
    done
    echo "pthread-iso-types: PASS host C ($(( ${#ORDERS[@]} * ${#C_MODES[@]} )) checks)"
fi

if command -v "$HOST_CXX" >/dev/null 2>&1; then
    for order in "${ORDERS[@]}"; do
        for cxx_mode in "${CXX_MODES[@]}"; do
            "$HOST_CXX" $cxx_mode -Wall -Wextra -Werror -fsyntax-only \
                -isystem "$SDK_INC" -D"$order"=1 -x c++ \
                "$SCRIPT_DIR/pthread-iso-types-test.c"
            checks=$((checks + 1))
        done
    done
    echo "pthread-iso-types: PASS host C++ ($(( ${#ORDERS[@]} * ${#CXX_MODES[@]} )) checks)"
fi

# 2. PPU cross-compiler checks
PPU_CC="${PPU_CC:-powerpc64-ps3-elf-gcc}"
PPU_CXX="${PPU_CXX:-powerpc64-ps3-elf-g++}"

if ! command -v "$PPU_CC" >/dev/null 2>&1; then
    if [ -n "${PS3DEV:-}" ] && [ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]; then
        PPU_CC="$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc"
        PPU_CXX="$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++"
    fi
fi

if command -v "$PPU_CC" >/dev/null 2>&1; then
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "$tmp_dir"' EXIT

    for order in "${ORDERS[@]}"; do
        for c_mode in "${C_MODES[@]}"; do
            for abi in "" "-mlp64"; do
                $PPU_CC $abi $c_mode -Wall -Wextra -Werror \
                    -isystem "$SDK_INC" \
                    -D"$order"=1 \
                    -c "$SCRIPT_DIR/pthread-iso-types-test.c" \
                    -o "$tmp_dir/test_c.o"
                checks=$((checks + 1))
            done
        done
    done

    for order in "${ORDERS[@]}"; do
        for cxx_mode in "${CXX_MODES[@]}"; do
            for abi in "" "-mlp64"; do
                $PPU_CXX $abi $cxx_mode -Wall -Wextra -Werror \
                    -isystem "$SDK_INC" \
                    -D"$order"=1 \
                    -x c++ \
                    -c "$SCRIPT_DIR/pthread-iso-types-test.c" \
                    -o "$tmp_dir/test_cxx.o"
                checks=$((checks + 1))
            done
        done
    done
    echo "pthread-iso-types: PASS PPU ($(( ${#ORDERS[@]} * (${#C_MODES[@]} + ${#CXX_MODES[@]}) * 2 )) checks)"
else
    echo "pthread-iso-types: SKIP PPU (cross-compiler unavailable)"
fi

if [ "$checks" -eq 0 ]; then
    echo "pthread-iso-types: FAIL no compilers available" >&2
    exit 1
fi

echo "pthread-iso-types: ALL PASS ($checks configurations checked)"
