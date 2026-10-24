#!/usr/bin/env bash
# The SDK-owned <sys/file.h>: tests/sdk/sys-file-test.c compiles as gnu99, c11 and c++17, ILP32 and LP64, -O2 -Wall
# -Wextra -Werror, through <sys/file.h> and through the <sys/lv2_fs_ext.h> forwarder, with this tree's sdk/include
# in front of the installed SDK (every one of the 23 sysLv2Fs* wrappers is called).  By value
# (tests/lib/ppc-const-eval.sh), ILP32 -O2: each probe reaches its sc with the expected syscall number in r11 and
# every pointer argument zero-extended (0x00000000800000xx), not sign-extended.  Skips without PS3DEV.
# usage: sys-file-test.sh [--ps3dev DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; [ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then echo "sys-file: SKIP (set PS3DEV or --ps3dev)"; exit 0; fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sys-file: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
export PS3DK="$sdk"
# shellcheck source=../lib/ppc-const-eval.sh
source "$root/tests/lib/ppc-const-eval.sh"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/sys-file-test.c"
status=0; n=0
for via in "" -DVIA_FORWARDER; do
    for lang in c c++; do
        stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
        for std in $stds; do
            for abi in "" -mlp64; do
                n=$((n + 1))
                if ! out=$("$cc" -x "$lang" "$std" $abi $via -O2 -Wall -Wextra -Werror -I"$root/sdk/include" \
                        -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                    echo "sys-file: FAIL compile ${via:-direct} $lang $std ${abi:-ilp32}"
                    printf '%s\n' "$out" | grep -E "error" | head -4 | sed 's/^/    /'; status=1
                fi
            done
        done
    done
done
[ $status -eq 0 ] && echo "sys-file: ok   $n compiles (sys/file.h and the lv2_fs_ext.h forwarder x gnu99/c11/c++17 x ILP32/LP64)"

if ! "$cc" -std=gnu99 -O2 -Wall -Wextra -Werror -I"$root/sdk/include" -I"$sdk/ppu/include" -S "$src" -o "$work/t.s" 2>"$work/err"; then
    echo "sys-file: FAIL ILP32 -S"; head -3 "$work/err"; status=1
else
    # <probe>:<syscall>:<register=value ...> - the pointer arguments of each probe
    for row in "open_probe:801:3=04 5=10 7=20" "read_probe:802:4=04 6=10" "stat_probe:808:3=04 4=10" \
               "rename_probe:812:3=04 4=10" "lseek_probe:818:6=04" "chown_probe:835:3=04" "mount_probe:837:3=04 4=10 5=20"; do
        f=${row%%:*}; rest=${row#*:}; want=${rest%%:*}; regs=${rest#*:}
        body=$(sed -n "/^\.L\.$f:/,/blr/p" "$work/t.s")
        ppc_eval "$body" '^sc$'
        bad=""
        [ -n "$PPC_STOP_LINE" ] || bad="no sc reached ($PPC_UNSUPPORTED)"
        [ "$(ppc_reg 11)" = "$(printf '0x%016x' $want)" ] || bad="$bad r11=$(ppc_reg 11)"
        for rv in $regs; do r=${rv%%=*}; v=${rv#*=}
            [ "$(ppc_reg $r)" = "0x00000000800000$v" ] || bad="$bad r$r=$(ppc_reg $r)"
        done
        if [ -z "$bad" ]; then echo "sys-file: ok   $f: sc $want, pointer registers zero-extended ($regs)"
        else echo "sys-file: FAIL $f:$bad"; echo "$body" | sed 's/^/    /' | head -20; status=1; fi
    done
fi
[ $status -eq 0 ] && echo "PASS: sys-file-test" || echo "FAIL: sys-file-test"
exit $status
