#!/usr/bin/env bash
# The SDK-owned <sys/tty.h> and <sys/interrupt.h>: tests/sdk/sys-tty-interrupt-test.c compiles as gnu99, c11 and
# c++17, ILP32 and LP64, -O2 -Wall -Wextra -Werror (every reference and PSL1GHT name, constants pinned by static
# asserts), with this tree's sdk/include in front of the installed SDK.  By value (tests/lib/ppc-const-eval.sh), in
# the ILP32 -O2 build: tty_probe and tty_read_probe reach `sc` with r11 = 403 / 402, r4 = 0x0000000080000004 and
# r6 = 0x0000000080000010 - the 32-bit addresses zero-extended, not sign-extended.  The evaluator's controls: a
# sign-extending lis is refused.  Skips without PS3DEV.  usage: sys-tty-interrupt-test.sh [--ps3dev DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; [ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then echo "sys-tty-interrupt: SKIP (set PS3DEV or --ps3dev)"; exit 0; fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sys-tty-interrupt: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
export PS3DK="$sdk"
# shellcheck source=../lib/ppc-const-eval.sh
source "$root/tests/lib/ppc-const-eval.sh"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/sys-tty-interrupt-test.c"
status=0; n=0
for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            n=$((n + 1))
            if ! out=$("$cc" -x "$lang" "$std" $abi -O2 -Wall -Wextra -Werror -I"$root/sdk/include" \
                    -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                echo "sys-tty-interrupt: FAIL compile $lang $std ${abi:-ilp32}"
                printf '%s\n' "$out" | grep -E "error" | head -4 | sed 's/^/    /'; status=1
            fi
        done
    done
done
[ $status -eq 0 ] && echo "sys-tty-interrupt: ok   $n compiles (gnu99/c11/c++17 x ILP32/LP64)"

# by value: registers at the sc of each probe, ILP32 -O2
if ! "$cc" -std=gnu99 -O2 -Wall -Wextra -Werror -I"$root/sdk/include" -I"$sdk/ppu/include" -S "$src" -o "$work/t.s" 2>"$work/err"; then
    echo "sys-tty-interrupt: FAIL ILP32 -S"; head -3 "$work/err"; status=1
else
    # the evaluator's own control: a sign-extending lis/ori pair must not pass for a zero-extended address
    ppc_eval $'lis 4,0x8000\nori 4,4,4\nsc' '^sc'
    if [ "$(ppc_reg 4)" = 0xffffffff80000004 ]; then
        echo "sys-tty-interrupt: ok   evaluator control: lis/ori reads as sign-extended"
    else
        echo "sys-tty-interrupt: FAIL evaluator control read $(ppc_reg 4)"; status=1
    fi
    for fn in tty_probe:403 tty_read_probe:402; do
        f=${fn%%:*}; want=${fn##*:}
        body=$(sed -n "/^\.L\.$f:/,/blr/p" "$work/t.s")
        ppc_eval "$body" '^sc$'
        r11=$(ppc_reg 11); r4=$(ppc_reg 4); r6=$(ppc_reg 6)
        if [ -n "$PPC_STOP_LINE" ] && [ "$r11" = "$(printf '0x%016x' $want)" ] && [ "$r4" = 0x0000000080000004 ] &&
           [ "$r6" = 0x0000000080000010 ]; then
            echo "sys-tty-interrupt: ok   $f: sc $want with r4/r6 zero-extended"
        else
            echo "sys-tty-interrupt: FAIL $f: stop='$PPC_STOP_LINE' r11=$r11 r4=$r4 r6=$r6"; echo "$body" | sed 's/^/    /'; status=1
        fi
    done
fi
[ $status -eq 0 ] && echo "PASS: sys-tty-interrupt-test" || echo "FAIL: sys-tty-interrupt-test"
exit $status
