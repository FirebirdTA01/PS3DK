#!/usr/bin/env bash
# lwmutex-synchronization-test.sh — verifies sys_lwmutex and sys_lwcond in
# <sys/synchronization.h> across PPU cross compilers (ILP32, LP64, C99, C11, C++17),
# including header include ordering probes and sys/event.h co-existence.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=${LV2_INCLUDE_DIR:-"$root/sdk/include"}
stage_inc=${STAGE_INCLUDE_DIR:-${PS3DK_INC:-${PS3DEV:+$PS3DEV/ps3dk/ppu/include}}}
src="$root/tests/sdk/lwmutex-synchronization-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true
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

if ! command -v "$PPU_CC" >/dev/null 2>&1; then
    echo "lwmutex-sync: SKIP PPU cross-compiler not found ($PPU_CC)"
    exit 0
fi

if [ -d "$stage_inc" ]; then
    overlay="$tmp/inc"
    mkdir -p "$overlay"
    cp -r "$stage_inc"/* "$overlay/"
    cp -r "$inc"/* "$overlay/"
    inc_flags=(-isystem "$overlay")
else
    inc_flags=(-isystem "$inc")
fi

# 1. Cross compiler layout, prototype, and struct tag checks for both ABIs
for abi in ilp32 lp64; do
    flags=()
    if [ "$abi" = lp64 ]; then flags=(-mlp64); fi

    "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float \
        "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
        -c "$src" -o "$tmp/test_${abi}_gnu99.o"
    echo "lwmutex-sync: PASS PPU C (gnu99) $abi"
    ppu_checks=$((ppu_checks + 1))

    "$PPU_CC" -x c -std=c11 -mcpu=cell -mhard-float \
        "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
        -c "$src" -o "$tmp/test_${abi}_c11.o"
    echo "lwmutex-sync: PASS PPU C (c11) $abi"
    ppu_checks=$((ppu_checks + 1))

    if command -v "$PPU_CXX" >/dev/null 2>&1; then
        "$PPU_CXX" -x c++ -std=c++17 -mcpu=cell -mhard-float \
            "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" \
            -c "$src" -o "$tmp/test_${abi}_cxx17.o"
        echo "lwmutex-sync: PASS PPU C++ (c++17) $abi"
        ppu_checks=$((ppu_checks + 1))
    else
        echo "lwmutex-sync: SKIP PPU C++ (c++17) $abi ($PPU_CXX not found)"
        ppu_skips=$((ppu_skips + 1))
    fi
done

# 2. Both-orders probes: lv2/mutex.h <-> sys/synchronization.h
printf '#include <lv2/mutex.h>\n#include <sys/synchronization.h>\nint x;\n' > "$tmp/probe_lv2_first.c"
printf '#include <sys/synchronization.h>\n#include <lv2/mutex.h>\nint x;\n' > "$tmp/probe_sync_first.c"
printf '#include <sys/synchronization.h>\nint j(sys_mutex_t *mu){ sys_mutex_attr_t a; sysMutexAttrInitialize(a); return sysMutexCreate(mu,&a) + SYS_MUTEX_PROTOCOL_FIFO; }\n' > "$tmp/probe_syncmutex.c"

for abi in ilp32 lp64; do
    flags=()
    if [ "$abi" = lp64 ]; then flags=(-mlp64); fi

    "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" -c "$tmp/probe_lv2_first.c" -o /dev/null
    "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" -c "$tmp/probe_sync_first.c" -o /dev/null
    "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" -c "$tmp/probe_syncmutex.c" -o /dev/null
    echo "lwmutex-sync: PASS include order & syncmutex probes C $abi"
    ppu_checks=$((ppu_checks + 3))

    if command -v "$PPU_CXX" >/dev/null 2>&1; then
        "$PPU_CXX" -x c++ -std=c++17 -mcpu=cell -mhard-float "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" -c "$tmp/probe_lv2_first.c" -o /dev/null
        "$PPU_CXX" -x c++ -std=c++17 -mcpu=cell -mhard-float "${flags[@]}" -Wall -Wextra -Werror "${inc_flags[@]}" -c "$tmp/probe_sync_first.c" -o /dev/null
        echo "lwmutex-sync: PASS include order probes C++ $abi"
        ppu_checks=$((ppu_checks + 2))
    fi
done

# 3. sys/event.h behind each header probe
event_headers=(sys/mutex.h sys/thread.h ppu-lv2.h sys/process.h sys/spu.h sysutil/sysutil.h lv2/cond.h sys/event_queue.h)
for h in "${event_headers[@]}"; do
    printf '#include <%s>\n#include <sys/event.h>\nint x;\n' "$h" > "$tmp/probe_event_pair.c"
    "$PPU_CC" -x c -std=gnu99 -mcpu=cell -mhard-float -Wall -Wextra -Werror "${inc_flags[@]}" -c "$tmp/probe_event_pair.c" -o /dev/null
    ppu_checks=$((ppu_checks + 1))
done
echo "lwmutex-sync: PASS sys/event.h behind 8 headers"

echo "lwmutex-sync: ALL PASS ($ppu_checks configurations checked)"
exit 0
