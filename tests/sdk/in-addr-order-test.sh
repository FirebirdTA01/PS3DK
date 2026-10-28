#!/usr/bin/env bash
# <cell/libnetctl.h> and <netinet/in.h> compile in any order (struct in_addr declared once, by netinet/in.h, as
# the system's libnetctl.h does).  Compiles tests/sdk/in-addr-order-test.c for each include order (netinet
# first, libnetctl first, the netex forwarder first, libnetctl alone) as gnu99, c11 and c++17, ILP32 and LP64,
# under -Wall -Wextra -Werror, with this tree's sdk/include in front of the installed SDK.
# Skips without PS3DEV.  usage: in-addr-order-test.sh [--ps3dev DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; [ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then echo "in-addr-order: SKIP (set PS3DEV or --ps3dev)"; exit 0; fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "in-addr-order: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
export PS3DK="$sdk"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/in-addr-order-test.c"
status=0; n=0
for order in ORDER_NETINET_FIRST ORDER_LIBNETCTL_FIRST ORDER_NETEX_FIRST ORDER_LIBNETCTL_ALONE; do
    for lang in c c++; do
        stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
        for std in $stds; do
            for abi in "" -mlp64; do
                n=$((n + 1))
                if ! out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror -D$order -I"$root/sdk/include" \
                        -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                    echo "in-addr-order: FAIL $order $lang $std ${abi:-ilp32}"
                    printf '%s\n' "$out" | grep -E "error" | head -4 | sed 's/^/    /'; status=1
                fi
            done
        done
    done
done
[ $status -eq 0 ] && echo "in-addr-order: ok   $n compiles (4 orders x gnu99/c11/c++17 x ILP32/LP64)"
[ $status -eq 0 ] && echo "PASS: in-addr-order-test" || echo "FAIL: in-addr-order-test"
exit $status
