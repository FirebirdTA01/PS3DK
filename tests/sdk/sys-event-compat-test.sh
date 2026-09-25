#!/usr/bin/env bash
# sys-event-compat-test.sh — verifies sys_event_t anonymous union compatibility
# across both ABIs (ilp32, lp64), C (-std=gnu99, -std=c11) and C++17.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=${LV2_INCLUDE_DIR:-"$root/sdk/include"}
src="$root/tests/sdk/sys-event-compat-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true
checks=0

PPU_CC=${PPU_CC:-powerpc64-ps3-elf-gcc}
PPU_CXX=${PPU_CXX:-powerpc64-ps3-elf-g++}

# 1. Cross compiler checks for both ABIs
for abi in ilp32 lp64; do
    flags=()
    if [ "$abi" = lp64 ]; then flags=(-mlp64); fi

    # C -std=gnu99
    if command -v "$PPU_CC" >/dev/null 2>&1; then
        "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror -I"$inc" \
            -c "$src" -o "$tmp/test_${abi}_gnu99.o"
        echo "sys-event-compat: PASS PPU C (gnu99) $abi"
        checks=$((checks + 1))

        # C -std=c11
        "$PPU_CC" -x c -std=c11 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror -I"$inc" \
            -c "$src" -o "$tmp/test_${abi}_c11.o"
        echo "sys-event-compat: PASS PPU C (c11) $abi"
        checks=$((checks + 1))
    fi

    # C++ -std=c++17
    if command -v "$PPU_CXX" >/dev/null 2>&1; then
        "$PPU_CXX" -x c++ -std=c++17 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror -I"$inc" \
            -c "$src" -o "$tmp/test_${abi}_cxx17.o"
        echo "sys-event-compat: PASS PPU C++ (c++17) $abi"
        checks=$((checks + 1))
    fi
done

# 2. Host compiler checks if available
HOST_CC=${CC:-gcc}
HOST_CXX=${CXX:-g++}

if command -v "$HOST_CC" >/dev/null 2>&1; then
    "$HOST_CC" -x c -std=gnu99 -Wall -Wextra -Werror -I"$inc" "$src" -o "$tmp/host_gnu99"
    "$tmp/host_gnu99"
    echo "sys-event-compat: PASS host C (gnu99)"
    checks=$((checks + 1))

    "$HOST_CC" -x c -std=c11 -Wall -Wextra -Werror -I"$inc" "$src" -o "$tmp/host_c11"
    "$tmp/host_c11"
    echo "sys-event-compat: PASS host C (c11)"
    checks=$((checks + 1))
fi

if command -v "$HOST_CXX" >/dev/null 2>&1; then
    "$HOST_CXX" -x c++ -std=c++17 -Wall -Wextra -Werror -I"$inc" "$src" -o "$tmp/host_cxx17"
    "$tmp/host_cxx17"
    echo "sys-event-compat: PASS host C++ (c++17)"
    checks=$((checks + 1))
fi

if [ "$checks" = 0 ]; then
    echo "sys-event-compat: no compilers available" >&2
    exit 77
fi
echo "sys-event-compat: ALL PASS ($checks configurations checked)"
