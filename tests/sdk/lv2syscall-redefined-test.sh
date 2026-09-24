#!/usr/bin/env bash
# Verifies that <sys/process.h> and <sys/sys_time.h> can be included in either
# order without triggering lv2syscall0..8 or register_passing_1..7 redefinitions.
# Tests both include orders under -Wall -Wextra -Werror in C and C++, for both
# ILP32 and LP64 when PPU tools are present, plus a red control proving that
# unpatched legacy headers fail without sdk/include/ppu-lv2.h.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
inc="$root/sdk/include"
fixture="$root/tests/sdk/lv2syscall-fixture/legacy-psl1ght"
src="$root/tests/sdk/lv2syscall-redefined-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

checks=0

# Determine available compilers
ppu_cc=${PPU_CC:-powerpc64-ps3-elf-gcc}
ppu_cxx=${PPU_CXX:-powerpc64-ps3-elf-g++}
host_cc=${CC:-cc}
host_cxx=${CXX:-c++}

if ! command -v "$host_cc" >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
    host_cc=gcc
fi
if ! command -v "$host_cxx" >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
    host_cxx=g++
fi

# 1. PPU Cross-compiler tests (ILP32 and LP64, C and C++, both orders)
for lang in c c++; do
    if [ "$lang" = c ]; then
        compiler="$ppu_cc"
        std_flag="-std=c11"
        extra_flags=("-Wno-pointer-to-int-cast")
    else
        compiler="$ppu_cxx"
        std_flag="-std=c++17"
        extra_flags=()
    fi

    if command -v "$compiler" >/dev/null 2>&1; then
        for abi in ilp32 lp64; do
            abi_flags=()
            if [ "$abi" = lp64 ]; then abi_flags=(-mlp64); fi

            for order in process_first systime_first; do
                order_flags=()
                if [ "$order" = process_first ]; then order_flags=(-DORDER_PROCESS_FIRST); fi

                "$compiler" -x "$lang" "$std_flag" -mcpu=cell -mhard-float \
                    "${abi_flags[@]}" "${order_flags[@]}" "${extra_flags[@]}" \
                    -Wall -Wextra -Werror -I"$inc" -I"$fixture" \
                    -c "$src" -o "$tmp/probe_${lang}_${abi}_${order}.o"
                echo "lv2syscall-redefined: PASS PPU $lang $abi $order"
                checks=$((checks + 1))
            done
        done
    else
        echo "lv2syscall-redefined: SKIP PPU $lang (compiler unavailable)"
    fi
done

# 2. Host compiler tests (syntax check in C and C++, both orders)
for lang in c c++; do
    if [ "$lang" = c ]; then
        compiler="$host_cc"
        std_flag="-std=c11"
        extra_flags=("-Wno-pointer-to-int-cast")
    else
        compiler="$host_cxx"
        std_flag="-std=c++17"
        extra_flags=()
    fi

    if command -v "$compiler" >/dev/null 2>&1; then
        for order in process_first systime_first; do
            order_flags=()
            if [ "$order" = process_first ]; then order_flags=(-DORDER_PROCESS_FIRST); fi

            "$compiler" -x "$lang" "$std_flag" -fsyntax-only \
                "${order_flags[@]}" "${extra_flags[@]}" \
                -Wall -Wextra -Werror -I"$inc" -I"$fixture" \
                "$src"
            echo "lv2syscall-redefined: PASS host $lang $order"
            checks=$((checks + 1))
        done
    else
        echo "lv2syscall-redefined: SKIP host $lang (compiler unavailable)"
    fi
done

if [ "$checks" -eq 0 ]; then
    echo "lv2syscall-redefined: FAIL no compilers available" >&2
    exit 1
fi

# 3. Executable red control: prove that if legacy fixture precedes sdk/include
# (simulating the unpatched baseline without sdk/include/ppu-lv2.h shadowing),
# ORDER_PROCESS_FIRST fails compilation due to macro redefinition.
red_compiler=""
red_lang="c"
red_flags=("-std=c11" "-Wno-pointer-to-int-cast")
if command -v "$ppu_cc" >/dev/null 2>&1; then
    red_compiler="$ppu_cc"
    red_flags+=("-mcpu=cell" "-mhard-float" "-c" "-o" "$tmp/red.o")
elif command -v "$host_cc" >/dev/null 2>&1; then
    red_compiler="$host_cc"
    red_flags+=("-fsyntax-only")
fi

if [ -n "$red_compiler" ]; then
    set +e
    red_output=$("$red_compiler" -x "$red_lang" "${red_flags[@]}" \
        -DORDER_PROCESS_FIRST -Wall -Wextra -Werror \
        -I"$fixture" -I"$inc" "$src" 2>&1)
    red_status=$?
    set -e

    if [ "$red_status" -eq 0 ]; then
        echo "lv2syscall-redefined: FAIL red control unexpectedly succeeded" >&2
        exit 1
    fi
    if ! printf '%s\n' "$red_output" | grep -E '(lv2syscall[0-8]|register_passing_[1-7]).*redefined' >/dev/null; then
        echo "lv2syscall-redefined: FAIL red control failed for wrong reason: $red_output" >&2
        exit 1
    fi
    echo "lv2syscall-redefined: PASS red control (redefinition caught when legacy ppu-lv2.h is active)"

    # 4. Mutant control: prove that an injected unrelated error (even when mentioning
    # the test filename lv2syscall-redefined-test.c) is rejected by the red guard.
    mutant_output="$src: fatal error: injected unrelated failure"
    if printf '%s\n' "$mutant_output" | grep -E '(lv2syscall[0-8]|register_passing_[1-7]).*redefined' >/dev/null; then
        echo "lv2syscall-redefined: FAIL mutant control accepted unrelated error" >&2
        exit 1
    fi
    echo "lv2syscall-redefined: PASS mutant control (unrelated compiler failure correctly rejected by red guard)"
fi

echo "lv2syscall-redefined: PASS ($checks configurations verified, red control passed)"
