#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
source_file=${SBRK_SOURCE:-$root/runtime/lv2/librt/sbrk.c}
cc=${CC:-cc}
failed=0
run_case() {
    local name=$1 request=$2 capacity=$3 failure=$4 available=$5 info_error=$6 alloc_error=$7 calls=$8
    local extra=()
    if [[ $request != default ]]; then
        printf '#include <stdint.h>\nconst uint64_t __ps3tc_heap_size = %s;\n' "$request" > "$work/app.c"
        extra+=("$work/app.c")
    fi
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
        -I"$root/tests/sdk/heap-fixture" -I"$root/sdk/include" \
        "-DSBRK_SOURCE=\"$source_file\"" -DEXPECT_CAPACITY="$capacity" \
        -DEXPECT_FAILURE="$failure" -DAVAILABLE="$available" \
        -DINFO_ERROR="$info_error" -DALLOC_ERROR="$alloc_error" \
        -DEXPECT_ALLOCATION_CALLS="$calls" \
        "$root/tests/sdk/librt-heap-test.c" "${extra[@]}" -o "$work/test"
    if "$work/test"; then echo "heap case $name: PASS"; else failed=1; echo "heap case $name: FAIL"; fi
}
run_case default default 67108864 0 268435456 0 0 1
run_case override 134217728 134217728 0 268435456 0 0 1
run_case round-up 1048577 2097152 0 268435456 0 0 1
run_case zero 0 0 1 268435456 0 0 0
run_case round-overflow UINT64_MAX 0 1 268435456 0 0 0
run_case syscall-overflow 'UINT64_C(4294967296)' 0 1 268435456 0 0 0
run_case unavailable 67108864 0 1 33554432 0 0 0
run_case query-failure default 0 1 268435456 -17 0 0
run_case allocation-failure default 0 1 268435456 0 -12 1
# Exercise the public macro in both languages, with both sides built with LTO.
cxx=${CXX:-c++}
ar=${AR:-ar}
for language in c c++; do
    compiler=$cc
    standard=c11
    if [[ $language == c++ ]]; then compiler=$cxx; standard=c++17; fi
    printf '#include <sys/heap_config.h>\nPS3TC_HEAP_SIZE(UINT64_C(128) * 1024 * 1024);\n' > "$work/config.c"
    for lto in off on; do
        flags=()
        [[ $lto == off ]] || flags+=(-flto)
        "$compiler" -x "$language" -std="$standard" -O2 -Wall -Wextra -Werror \
            "${flags[@]}" -I"$root/sdk/include" -c "$work/config.c" -o "$work/config.o"
        "$ar" rcs "$work/config.a" "$work/config.o"
        "$cc" -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
            "${flags[@]}" -I"$root/tests/sdk/heap-fixture" -I"$root/sdk/include" \
            "-DSBRK_SOURCE=\"$source_file\"" -DEXPECT_CAPACITY=134217728 \
            -DEXPECT_FAILURE=0 -DAVAILABLE=268435456 -DINFO_ERROR=0 \
            -DALLOC_ERROR=0 -DEXPECT_ALLOCATION_CALLS=1 \
            -c "$root/tests/sdk/librt-heap-test.c" -o "$work/allocator.o"
        for mode in object archive; do
            inputs=("$work/config.o")
            [[ $mode == object ]] || inputs=(-Wl,-u,__ps3tc_heap_config_anchor "$work/config.a")
            "$cc" "${flags[@]}" "$work/allocator.o" "${inputs[@]}" -o "$work/link-test"
            if "$work/link-test"; then
                echo "heap linkage $language LTO=$lto $mode: PASS"
            else
                failed=1
                echo "heap linkage $language LTO=$lto $mode: FAIL"
            fi
        done
        cp "$work/config.o" "$work/duplicate.o"
        if "$cc" "${flags[@]}" "$work/allocator.o" "$work/config.o" \
            "$work/duplicate.o" -o "$work/duplicate" > "$work/duplicate.log" 2>&1; then
            echo "heap duplicate definition unexpectedly linked" >&2
            failed=1
        elif ! grep -Eq 'multiple definition|duplicate symbol' "$work/duplicate.log"; then
            cat "$work/duplicate.log" >&2
            failed=1
        fi
    done
done
exit "$failed"
