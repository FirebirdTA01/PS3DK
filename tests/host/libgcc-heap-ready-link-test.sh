#!/usr/bin/env bash
# The GCC 16 PPU libgcc FDE registry (gcc-16.x 0074) REQUIRES the runtime's heap-ready flag at link time.
#
# crtbegin registers the program's frames from _init, before the constructors that create the heap and the
# syscall table; the registry takes its first nodes from a static reserve until the runtime's lv2-crt0.o sets
# __ps3dk_heap_ready.  A CRT without the flag (an older runtime) must FAIL TO LINK rather than let the registry
# guess.  Rows, both ABIs, a C++ program that throws (pulls in the unwinder):
#   1. the stage's own CRT: links, and the program defines __ps3dk_heap_ready;
#   2. a copy of the stage's lv2-crt0.o, found through -B: links (proves the -B route reaches the CRT);
#   3. the same copy with the symbol renamed away (an old CRT): the link FAILS naming __ps3dk_heap_ready;
#   4. with OLD_CRT=<an older runtime's lv2-crt0.o> (e.g. the 90c8e669 stage's): the link FAILS likewise.
# Skips without PS3DEV or on a libgcc without the 0074 registry.
set -u -o pipefail
G=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-g++
P=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf
if [ -z "${PS3DEV:-}" ] || [ ! -x "$G" ]; then echo "SKIP: libgcc-heap-ready-link-test (no PS3DEV compiler)"; exit 0; fi
# The driver's link spec reads PS3DK (the SDK library directory); default it to the stage's own SDK.
export PS3DK=${PS3DK:-$PS3DEV/ps3dk}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "libgcc-heap-ready-link: FAIL $*"; status=1; }

cat > "$work/t.cpp" <<'EOF'
#include <stdexcept>
int main() { try { throw std::runtime_error("x"); } catch (const std::exception &) { return 0; } return 1; }
EOF

for abi in ilp32 lp64; do
    flag=; [ $abi = lp64 ] && flag=-mlp64
    lib=$("$G" $flag -print-libgcc-file-name) || { fail "$abi: -print-libgcc-file-name"; continue; }
    # nm's whole output first: grep -q under pipefail would SIGPIPE nm and read as "no reference"
    libsyms=$("$P-nm" "$lib" 2>/dev/null) || { fail "$abi: nm $lib"; continue; }
    if ! grep -q ' U __ps3dk_heap_ready$' <<<"$libsyms"; then
        echo "SKIP: libgcc-heap-ready-link-test $abi (libgcc does not require __ps3dk_heap_ready: no 0074 registry)"
        continue
    fi
    crt=$("$G" $flag -print-file-name=lv2-crt0.o)
    [ -f "$crt" ] || { fail "$abi: no lv2-crt0.o ($crt)"; continue; }
    d=$work/$abi; mkdir -p "$d/good" "$d/old"
    # 1. stage CRT
    if "$G" $flag -O2 "$work/t.cpp" -o "$d/stage.elf" > "$d/stage.log" 2>&1; then
        elfsyms=$("$P-nm" "$d/stage.elf" 2>/dev/null)
        grep -qE ' [BDGS] __ps3dk_heap_ready$' <<<"$elfsyms" \
            && echo "libgcc-heap-ready-link: ok $abi stage CRT links and defines __ps3dk_heap_ready" \
            || fail "$abi: linked, but __ps3dk_heap_ready is not defined in the program"
    else
        fail "$abi: the stage CRT does not link: $(head -n 3 "$d/stage.log")"
    fi
    # 2. an unmodified copy through -B
    cp "$crt" "$d/good/lv2-crt0.o"
    if "$G" $flag -B"$d/good/" -O2 "$work/t.cpp" -o "$d/good.elf" > "$d/good.log" 2>&1; then
        echo "libgcc-heap-ready-link: ok $abi an unmodified CRT copy found through -B links"
    else
        fail "$abi: the unmodified -B copy does not link (the -B route is not proven): $(head -n 3 "$d/good.log")"
    fi
    # 3. the flag renamed away = an old CRT
    "$P-objcopy" --redefine-sym __ps3dk_heap_ready=__ps3dk_heap_ready_absent "$crt" "$d/old/lv2-crt0.o" \
        || { fail "$abi: objcopy"; continue; }
    if "$G" $flag -B"$d/old/" -O2 "$work/t.cpp" -o "$d/old.elf" > "$d/old.log" 2>&1; then
        fail "$abi: a CRT without __ps3dk_heap_ready LINKED (the requirement is not enforced)"
    elif grep -q "__ps3dk_heap_ready" "$d/old.log"; then
        echo "libgcc-heap-ready-link: ok $abi a CRT without the flag fails to link, naming __ps3dk_heap_ready"
    else
        fail "$abi: the old-CRT link failed for another reason: $(head -n 3 "$d/old.log")"
    fi
    # 4. a real older runtime's CRT, when given
    real=${OLD_CRT:-}; [ $abi = lp64 ] && real=${OLD_CRT_LP64:-}
    if [ -n "$real" ]; then
        mkdir -p "$d/real" && cp "$real" "$d/real/lv2-crt0.o"
        if "$G" $flag -B"$d/real/" -O2 "$work/t.cpp" -o "$d/real.elf" > "$d/real.log" 2>&1; then
            fail "$abi: the older runtime's CRT ($real) LINKED"
        elif grep -q "__ps3dk_heap_ready" "$d/real.log"; then
            echo "libgcc-heap-ready-link: ok $abi the older runtime's CRT ($real) fails to link, naming __ps3dk_heap_ready"
        else
            fail "$abi: the older runtime's CRT failed for another reason: $(head -n 3 "$d/real.log")"
        fi
    fi
done
[ $status -eq 0 ] && echo "PASS: libgcc-heap-ready-link-test"
exit $status
