#!/usr/bin/env bash
# in-addr-redefinition-test.sh — Test matrix for struct in_addr header visibility and ordering
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SDK_INC="$ROOT_DIR/sdk/include"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

checks=0

ORDERS=(
    "TEST_ORDER_NETINET_THEN_LIBNETCTL"
    "TEST_ORDER_LIBNETCTL_THEN_NETINET"
    "TEST_ORDER_NETEX_THEN_NETINET"
    "TEST_ORDER_NETINET_THEN_NETEX"
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
                "$SCRIPT_DIR/in-addr-redefinition-test.c"
            checks=$((checks + 1))
        done
    done
    echo "in-addr-redefinition: PASS host C ($(( ${#ORDERS[@]} * ${#C_MODES[@]} )) checks)"
fi

if command -v "$HOST_CXX" >/dev/null 2>&1; then
    for order in "${ORDERS[@]}"; do
        for cxx_mode in "${CXX_MODES[@]}"; do
            "$HOST_CXX" $cxx_mode -Wall -Wextra -Werror -fsyntax-only \
                -isystem "$SDK_INC" -D"$order"=1 -x c++ \
                "$SCRIPT_DIR/in-addr-redefinition-test.c"
            checks=$((checks + 1))
        done
    done
    echo "in-addr-redefinition: PASS host C++ ($(( ${#ORDERS[@]} * ${#CXX_MODES[@]} )) checks)"
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
    for order in "${ORDERS[@]}"; do
        for c_mode in "${C_MODES[@]}"; do
            for abi in "" "-mlp64"; do
                $PPU_CC $abi $c_mode -Wall -Wextra -Werror \
                    -isystem "$SDK_INC" \
                    -D"$order"=1 \
                    -c "$SCRIPT_DIR/in-addr-redefinition-test.c" \
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
                    -c "$SCRIPT_DIR/in-addr-redefinition-test.c" \
                    -o "$tmp_dir/test_cxx.o"
                checks=$((checks + 1))
            done
        done
    done
    echo "in-addr-redefinition: PASS PPU ($(( ${#ORDERS[@]} * (${#C_MODES[@]} + ${#CXX_MODES[@]}) * 2 )) checks)"
else
    echo "in-addr-redefinition: SKIP PPU (cross-compiler unavailable)"
fi

if [ "$checks" -eq 0 ]; then
    echo "in-addr-redefinition: FAIL no compilers available" >&2
    exit 1
fi

# 3. Red control: unpatched cell/libnetctl.h duplicating struct in_addr
cc_probe="${HOST_CC:-gcc}"
if ! command -v "$cc_probe" >/dev/null 2>&1; then
    cc_probe="$PPU_CC"
fi
if command -v "$cc_probe" >/dev/null 2>&1; then
    red_netctl_dir="$tmp_dir/red_netctl/cell"
    mkdir -p "$red_netctl_dir"
    sed 's/#include <netinet\/in.h>/struct in_addr { unsigned int s_addr; };/' "$SDK_INC/cell/libnetctl.h" > "$red_netctl_dir/libnetctl.h"

    set +e
    red_netctl_out=$("$cc_probe" -std=c99 -Wall -Wextra -Werror -fsyntax-only \
        -I"$tmp_dir/red_netctl" \
        -isystem "$SDK_INC" \
        -DTEST_ORDER_NETINET_THEN_LIBNETCTL=1 \
        "$SCRIPT_DIR/in-addr-redefinition-test.c" 2>&1)
    red_netctl_status=$?
    set -e

    if [ "$red_netctl_status" -eq 0 ]; then
        echo "in-addr-redefinition: FAIL red control cell/libnetctl.h unexpectedly succeeded" >&2
        exit 1
    fi
    if ! printf '%s\n' "$red_netctl_out" | grep -E "(redefinition of 'struct in_addr'|redefinition of ‘struct in_addr’)" >/dev/null; then
        echo "in-addr-redefinition: FAIL red control cell/libnetctl.h failed for wrong reason: $red_netctl_out" >&2
        exit 1
    fi
    echo "in-addr-redefinition: PASS red control cell/libnetctl.h (redefinition caught)"
fi

echo "in-addr-redefinition: ALL PASS ($checks candidate configurations and 1 red control passed)"
