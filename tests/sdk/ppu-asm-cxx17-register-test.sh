#!/usr/bin/env bash
# ISO C++17 removed the 'register' storage class.  The statement-expression
# macros of <sys/ppu-asm.h> and <ppu-asm.h> (__build_opd32, __read8..64,
# __gettime) must declare their asm-output temporaries without it, so a
# C++17 user does not inherit -Wregister.  The explicit-register variables
# of the lv2 syscall macros (register T x __asm__("N")) are exempt and stay.
#
# ppu-asm-cxx17-register-test.c expands every touched macro; it is compiled
# as C++17 under -Wall -Werror -Werror=register and as C11 under -Wall
# -Werror, ILP32 and -mlp64, once per header.
#
# usage: ppu-asm-cxx17-register-test.sh [--ps3dev DIR] [--include DIR]
#                                       [--psl1ght-src DIR]
#   --include      header tree under test (default: this tree's sdk/include).
#                  Its sys/ppu-asm.h and ppu-asm.h are overlaid on the
#                  installed SDK's ppu/include.  The installed ppu-asm.h is
#                  the SDK-owned sdk/include/ppu-asm.h (install-psl1ght.sh
#                  never installs PSL1GHT's own copy).
#   --psl1ght-src  a PSL1GHT source tree with patches/psl1ght applied; its
#                  ppu/include/ppu-asm.h is checked as a third header.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"; psl=""
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        --psl1ght-src) psl="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR] [--psl1ght-src DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "ppu-asm-register: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ppu-asm-register: FAIL: no PPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/ppu-asm-cxx17-register-test.c"

# One overlay per header under test: name|overlay dir|-D selecting it.
rows=()
mkdir -p "$work/sdk/sys"
for h in sys/ppu-asm.h ppu-asm.h; do
    [ -f "$inc/$h" ] || { echo "ppu-asm-register: FAIL: no $h under $inc"; exit 1; }
    cp "$inc/$h" "$work/sdk/$h"
done
rows+=("sys/ppu-asm.h|$work/sdk|-DUSE_SYS_PPU_ASM" "ppu-asm.h|$work/sdk|")
if [ -n "$psl" ]; then
    [ -f "$psl/ppu/include/ppu-asm.h" ] || { echo "ppu-asm-register: FAIL: no ppu/include/ppu-asm.h under $psl"; exit 1; }
    mkdir -p "$work/psl"
    cp "$psl/ppu/include/ppu-asm.h" "$work/psl/ppu-asm.h"
    rows+=("psl1ght ppu-asm.h|$work/psl|")
fi

status=0
for row in "${rows[@]}"; do
    IFS='|' read -r name ov def <<< "$row"
    for mode in "c++ -std=c++17 -Werror=register" "c -std=c11"; do
        set -- $mode
        for abi in "" -mlp64; do
            label="$name $1 $2 ${abi:-ilp32}"
            if out=$("$cc" -x "$1" "$2" ${3:-} $abi $def -Wall -Werror \
                    -I"$ov" -I"$ps3dev/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                echo "ppu-asm-register: ok   $label"
            else
                echo "ppu-asm-register: FAIL $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done
[ "$status" -eq 0 ] && echo "ppu-asm-register: PASS"
exit $status
