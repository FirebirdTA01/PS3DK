#!/usr/bin/env bash
# The eleven np/ include-path forwarders (np-path-forwarders-test.c):
#  1. the test TU, which pins one function per forwarder, compiles with the
#     PPU compiler as gnu99, c11 and c++17, ILP32 and LP64, under
#     -Wall -Wextra -Werror;
#  2. each forwarder compiles alone in its own TU;
#  3. control: <np/ranking.h> reaches cell/np/score.h - a TU pinning
#     sceNpScoreAbortTransaction compiles.
# Self-check: with <np/ranking.h> pointed at cell/np/basic.h the control
# must fail, and a TU pinning sceNpBasicInit through <np/basic.h> must still
# compile.  Only the eleven forwarders go in front of the installed SDK.
#
# usage: np-path-forwarders-test.sh [--ps3dev DIR] [--include DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "np-path-forwarders: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "np-path-forwarders: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/np-path-forwarders-test.c"
status=0

forwarders="basic custom_menu friendlist lookup manager profile ranking signaling sns util auth_oauth"
mkdir -p "$work/ov/np"
for f in $forwarders; do cp "$inc/np/$f.h" "$work/ov/np/"; done

compile() {  # <include dir> <file> <lang> <std> <abi>
    "$cc" -x "$3" "$4" $5 -Wall -Wextra -Werror -I"$1" -I"$sdk/ppu/include" -c "$2" -o "$work/t.o" 2>&1
}

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$(compile "$work/ov" "$src" "$lang" "$std" "$abi"); then
                echo "np-path-forwarders: ok   compile $label"
            else
                echo "np-path-forwarders: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

for f in $forwarders; do
    printf '%s\n' "#include <np/$f.h>" 'int main(void) { return 0; }' > "$work/lone.c"
    if out=$(compile "$work/ov" "$work/lone.c" c -std=gnu99 ""); then
        echo "np-path-forwarders: ok   lone forwarder np/$f.h"
    else
        echo "np-path-forwarders: FAIL lone forwarder np/$f.h"
        printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -4
        status=1
    fi
done

# The control and its self-check pin a function through a typedef'd
# pointer; a function's address in a condition is always true and -Waddress
# would reject it.
printf '%s\n' '#include <np/ranking.h>' \
    'typedef int abort_fn(int32_t);' \
    'abort_fn *volatile pin_ranking = &sceNpScoreAbortTransaction;' > "$work/control.c"
printf '%s\n' '#include <np/basic.h>' \
    'typedef int init_fn(void);' \
    'init_fn *volatile pin_basic = &sceNpBasicInit;' > "$work/control_basic.c"
if out=$(compile "$work/ov" "$work/control.c" c -std=gnu99 ""); then
    echo "np-path-forwarders: ok   control: np/ranking.h reaches cell/np/score.h"
else
    echo "np-path-forwarders: FAIL control"; printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -4
    status=1
fi

mkdir -p "$work/bad"; cp -r "$work/ov/." "$work/bad/"
printf '%s\n' '#include <cell/np/basic.h>' > "$work/bad/np/ranking.h"
if compile "$work/bad" "$work/control.c" c -std=gnu99 "" > /dev/null; then
    echo "np-path-forwarders: FAIL self-check: np/ranking.h pointed at cell/np/basic.h still compiled"
    status=1
elif ! compile "$work/bad" "$work/control_basic.c" c -std=gnu99 "" > /dev/null; then
    echo "np-path-forwarders: FAIL self-check: the np/basic.h pin does not compile"
    status=1
else
    echo "np-path-forwarders: ok   self-check: a forwarder pointed at the wrong header is caught"
fi

[ $status -eq 0 ] && echo "PASS: np-path-forwarders-test"
exit $status
