#!/usr/bin/env bash
# Catches the ILP32 shift-by-32 regression and checks EA conversion boundaries.
# Set PPU_CC/PPU_CXX for cross checks, CC/CXX for host execution. Either lane
# can run alone (e.g. Windows PPU tools in Git Bash, host tools under WSL).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=${LV2_INCLUDE_DIR:-"$root/sdk/include"}
src="$root/tests/sdk/lv2-ea32-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true
checks=0

for lang in c c++; do
    if [ "$lang" = c ]; then
        ppu=${PPU_CC:-powerpc64-ps3-elf-gcc}
        host=${CC:-cc}
        std=c11
    else
        ppu=${PPU_CXX:-powerpc64-ps3-elf-g++}
        host=${CXX:-c++}
        std=c++17
    fi
    if command -v "$ppu" >/dev/null 2>&1; then
        for abi in ilp32 lp64; do
            flags=()
            width=4
            if [ "$abi" = lp64 ]; then flags=(-mlp64); width=8; fi
            for mode in debug release; do
                defines=()
                if [ "$mode" = release ]; then defines=(-DNDEBUG); fi
                "$ppu" -x "$lang" -std="$std" -mcpu=cell -mhard-float \
                    "${flags[@]}" "${defines[@]}" -DEXPECT_POINTER_SIZE="$width" \
                    -Wall -Wextra -Werror -Werror=shift-count-overflow -I"$inc" \
                    -c "$src" -o "$tmp/probe.o"
                echo "lv2-ea32: PASS PPU $lang $abi $mode"
                checks=$((checks + 1))
            done
        done
    else
        echo "lv2-ea32: SKIP PPU $lang (compiler unavailable)"
    fi
    if command -v "$host" >/dev/null 2>&1; then
        for mode in debug release; do
            defines=()
            if [ "$mode" = release ]; then defines=(-DNDEBUG); fi
            "$host" -x "$lang" -std="$std" "${defines[@]}" \
                -Wall -Wextra -Werror -I"$inc" "$src" -o "$tmp/probe"
            "$tmp/probe"
            # Only wide-pointer hosts can represent an out-of-range EA.
            if printf '#include <stdint.h>\n#if UINTPTR_MAX <= UINT32_MAX\n#error narrow host\n#endif\n' \
                | "$host" -x "$lang" -fsyntax-only - >/dev/null 2>&1; then
                if [ "$mode" = debug ]; then
                    set +e
                    ("$tmp/probe" overflow) >/dev/null 2>&1
                    status=$?
                    set -e
                    if [ "$status" -le 128 ]; then
                        echo "lv2-ea32: FAIL expected debug trap, got $status" >&2
                        exit 1
                    fi
                else
                    "$tmp/probe" overflow
                fi
            fi
            echo "lv2-ea32: PASS host $lang $mode"
            checks=$((checks + 1))
        done
    else
        echo "lv2-ea32: SKIP host $lang (compiler unavailable)"
    fi
done
if [ "$checks" = 0 ]; then
    echo 'lv2-ea32: no compilers available' >&2
    exit 77
fi
echo "lv2-ea32: PASS ($checks configurations)"
