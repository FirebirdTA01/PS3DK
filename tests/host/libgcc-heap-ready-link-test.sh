#!/usr/bin/env bash
# The GCC 16 PPU libgcc FDE registry (gcc-16.x 0074) REQUIRES the runtime's heap-ready flag at link time.
#
# crtbegin registers the program's frames from _init, before the constructors that create the heap and the
# syscall table; the registry takes its first nodes from a static reserve until the runtime's lv2-crt0.o sets
# __ps3dk_heap_ready.  A CRT without the flag (an older runtime) must FAIL TO LINK rather than let the registry
# guess.  Rows, both ABIs, a C++ program that throws (pulls in the unwinder):
#   1. the stage's own CRT: links, and the program defines __ps3dk_heap_ready;
#   2. a copy of the stage's lv2-crt0.o placed for -B: the driver RESOLVES lv2-crt0.o to that copy (checked with
#      -print-file-name: under -mlp64 it looks in the multilib subdirectory, -print-multi-directory), and it links;
#   3. the same copy with the symbol renamed away (an old CRT), placed and resolved the same way: the link FAILS
#      naming __ps3dk_heap_ready;
#   4. with OLD_CRT / OLD_CRT_LP64 = an older runtime's lv2-crt0.o (e.g. the 90c8e669 stage's): likewise.
# Every tool's exit status is checked; NM / OBJCOPY may name other tools (the adversary test substitutes ones that
# print valid output and then fail - the whole test must fail).  Skips without PS3DEV or on a libgcc without 0074.
set -u -o pipefail
G=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-g++
P=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf
if [ -z "${PS3DEV:-}" ] || [ ! -x "$G" ]; then echo "SKIP: libgcc-heap-ready-link-test (no PS3DEV compiler)"; exit 0; fi
NM=${NM:-$P-nm}
OBJCOPY=${OBJCOPY:-$P-objcopy}
# The driver's link spec reads PS3DK (the SDK library directory); default it to the stage's own SDK.
export PS3DK=${PS3DK:-$PS3DEV/ps3dk}
work=$(mktemp -d) || { echo "libgcc-heap-ready-link: FAIL mktemp"; exit 1; }
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "libgcc-heap-ready-link: FAIL $*"; status=1; }

cat > "$work/t.cpp" <<'EOF' || { echo "libgcc-heap-ready-link: FAIL writing the probe"; exit 1; }
#include <stdexcept>
int main() { try { throw std::runtime_error("x"); } catch (const std::exception &) { return 0; } return 1; }
EOF

# place <abi flag> <dir> <source crt>: copy the CRT where the driver looks under -B<dir>/ and prove it resolves there.
place() {
    local flag=$1 dir=$2 src=$3 md target resolved
    md=$("$G" $flag -print-multi-directory) || { echo "-print-multi-directory failed"; return 1; }
    if [ "$md" = . ]; then target=$dir/lv2-crt0.o; else target=$dir/$md/lv2-crt0.o; fi
    mkdir -p "$(dirname "$target")" || { echo "mkdir failed"; return 1; }
    cp "$src" "$target" || { echo "cp failed"; return 1; }
    resolved=$("$G" $flag -B"$dir/" -print-file-name=lv2-crt0.o) || { echo "-print-file-name failed"; return 1; }
    [ "$(realpath "$resolved")" = "$(realpath "$target")" ] || { echo "the driver resolves lv2-crt0.o to $resolved, not $target"; return 1; }
}

for abi in ilp32 lp64; do
    flag=; [ $abi = lp64 ] && flag=-mlp64
    lib=$("$G" $flag -print-libgcc-file-name) || { fail "$abi: -print-libgcc-file-name"; continue; }
    # nm's whole output first, status checked: no pipe into grep -q (pipefail + SIGPIPE reads a hit as a miss)
    libsyms=$("$NM" "$lib" 2>/dev/null) || { fail "$abi: nm $lib exited non-zero"; continue; }
    grep -q ' U __ps3dk_heap_ready$' <<<"$libsyms"; rc=$?
    [ $rc -le 1 ] || { fail "$abi: grep error $rc"; continue; }
    if [ $rc -eq 1 ]; then
        echo "SKIP: libgcc-heap-ready-link-test $abi (libgcc does not require __ps3dk_heap_ready: no 0074 registry)"
        continue
    fi
    crt=$("$G" $flag -print-file-name=lv2-crt0.o) || { fail "$abi: -print-file-name=lv2-crt0.o"; continue; }
    [ -f "$crt" ] || { fail "$abi: no lv2-crt0.o ($crt)"; continue; }
    d=$work/$abi
    mkdir -p "$d" || { fail "$abi: mkdir"; continue; }
    # 1. stage CRT
    if "$G" $flag -O2 "$work/t.cpp" -o "$d/stage.elf" > "$d/stage.log" 2>&1; then
        if elfsyms=$("$NM" "$d/stage.elf" 2>/dev/null); then
            grep -qE ' [BDGS] __ps3dk_heap_ready$' <<<"$elfsyms" \
                && echo "libgcc-heap-ready-link: ok $abi stage CRT links and defines __ps3dk_heap_ready" \
                || fail "$abi: linked, but __ps3dk_heap_ready is not defined in the program"
        else
            fail "$abi: nm of the linked program exited non-zero"
        fi
    else
        fail "$abi: the stage CRT does not link: $(head -n 3 "$d/stage.log")"
    fi
    # 2. an unmodified copy, proven to be the CRT the driver uses
    if why=$(place "$flag" "$d/good" "$crt"); then
        if "$G" $flag -B"$d/good/" -O2 "$work/t.cpp" -o "$d/good.elf" > "$d/good.log" 2>&1; then
            echo "libgcc-heap-ready-link: ok $abi an unmodified CRT copy, resolved by the driver through -B, links"
        else
            fail "$abi: the unmodified -B copy does not link: $(head -n 3 "$d/good.log")"
        fi
    else
        fail "$abi: placing the unmodified copy: $why"
    fi
    # 3. the flag renamed away = an old CRT; 4. a real older runtime's CRT
    real=${OLD_CRT:-}; [ $abi = lp64 ] && real=${OLD_CRT_LP64:-}
    "$OBJCOPY" --redefine-sym __ps3dk_heap_ready=__ps3dk_heap_ready_absent "$crt" "$d/renamed.o" \
        || { fail "$abi: objcopy exited non-zero"; continue; }
    for c in "renamed:$d/renamed.o" ${real:+"older:$real"}; do
        kind=${c%%:*}; src=${c#*:}
        why=$(place "$flag" "$d/$kind" "$src") || { fail "$abi: placing the $kind CRT: $why"; continue; }
        if "$G" $flag -B"$d/$kind/" -O2 "$work/t.cpp" -o "$d/$kind.elf" > "$d/$kind.log" 2>&1; then
            fail "$abi: the $kind CRT ($src), resolved by the driver, LINKED (the requirement is not enforced)"
        elif grep -q "__ps3dk_heap_ready" "$d/$kind.log"; then
            echo "libgcc-heap-ready-link: ok $abi the $kind CRT ($src), resolved by the driver, fails to link naming __ps3dk_heap_ready"
        else
            fail "$abi: the $kind CRT link failed for another reason: $(head -n 3 "$d/$kind.log")"
        fi
    done
done
[ $status -eq 0 ] && echo "PASS: libgcc-heap-ready-link-test"
exit $status
