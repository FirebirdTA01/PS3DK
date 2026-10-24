#!/usr/bin/env bash
# The SDK-owned <sys/tty.h> and <sys/interrupt.h>.  tests/sdk/sys-tty-interrupt-test.c compiles as gnu99, c11 and
# c++17, ILP32 and LP64, -O2 -Wall -Wextra -Werror (every reference and PSL1GHT name, constants pinned by static
# asserts), alone and with PSL1GHT's <lv2/interrupt.h> included before and after ours, with this tree's sdk/include
# in front of the installed SDK.  tests/sdk/sys-interrupt-link-test.c links the interrupt-thread teardown library
# call under both names (sys_interrupt_thread_disestablish, sysInterruptThreadDisestablish) against liblv2_stub,
# both ABIs.  By value (tests/lib/ppc-const-eval.sh), TTY calls only, in the ILP32 -O2 build: tty_probe and
# tty_read_probe reach `sc` with r11 = 403 / 402, r4 = 0x0000000080000004 and r6 = 0x0000000080000010 - the 32-bit
# addresses zero-extended, not sign-extended; the evaluator's control: a sign-extending lis/ori reads as such.
# Skips without PS3DEV.  usage: sys-tty-interrupt-test.sh [--ps3dev DIR]
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
for order in "" -DLV2_FIRST -DLV2_AFTER; do
    for lang in c c++; do
        stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
        for std in $stds; do
            for abi in "" -mlp64; do
                n=$((n + 1))
                if ! out=$("$cc" -x "$lang" "$std" $abi $order -O2 -Wall -Wextra -Werror -I"$root/sdk/include" \
                        -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                    echo "sys-tty-interrupt: FAIL compile ${order:-default} $lang $std ${abi:-ilp32}"
                    printf '%s\n' "$out" | grep -E "error" | head -4 | sed 's/^/    /'; status=1
                fi
            done
        done
    done
done
[ $status -eq 0 ] && echo "sys-tty-interrupt: ok   $n compiles (lv2/interrupt.h none/first/after x gnu99/c11/c++17 x ILP32/LP64)"

# the teardown library call under both names links against liblv2_stub, both ABIs
for abi in "" -mlp64; do
    lib="$sdk/ppu/lib"; [ "$abi" = -mlp64 ] && lib="$sdk/ppu/lib/lp64"
    if out=$("$cc" -std=c11 $abi -Wall -Wextra -Werror -I"$root/sdk/include" -I"$sdk/ppu/include" \
            "$root/tests/sdk/sys-interrupt-link-test.c" -o "$work/l.elf" -L"$lib" -llv2_stub 2>&1); then
        echo "sys-tty-interrupt: ok   ${abi:-ilp32} link: sys_interrupt_thread_disestablish and sysInterruptThreadDisestablish resolve"
    else
        echo "sys-tty-interrupt: FAIL ${abi:-ilp32} link"; printf '%s\n' "$out" | grep -E "error|undefined" | head -4 | sed 's/^/    /'; status=1
    fi
done

# by value: registers at the sc of each TTY probe, ILP32 -O2
if ! "$cc" -std=gnu99 -O2 -Wall -Wextra -Werror -I"$root/sdk/include" -I"$sdk/ppu/include" -S "$src" -o "$work/t.s" 2>"$work/err"; then
    echo "sys-tty-interrupt: FAIL ILP32 -S"; head -3 "$work/err"; status=1
else
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
