#!/usr/bin/env bash
# <sys/timer.h> LV2 timer surface (see sys-timer-surface-test.c):
#  1. compiles with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
#     under -Wall -Wextra -Werror;
#  2. at -O2, each wrapper function loads its own syscall number into r11
#     before its sc (70-76 for the timers, 141/142 for the sleeps), in both
#     data models;
#  3. sys_time_get_system_time() is a call to the sysPrxForUser import (the
#     clock the timers use), not an inline wall-clock syscall.
# A self-check runs the number check against an object whose t_stop wrapper
# was rebuilt with the wrong number, and requires that check to fail.
#
# usage: sys-timer-surface-test.sh [--ps3dev DIR] [--include DIR]
#   --include  header tree under test (default: this tree's sdk/include);
#              only sys/timer.h is overlaid on the installed SDK headers.
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
    echo "sys-timer-surface: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
objdump="$ps3dev/ppu/bin/powerpc64-ps3-elf-objdump"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sys-timer-surface: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/ov/sys"
cp "$inc/sys/timer.h" "$work/ov/sys/timer.h"
cp "$inc/sys/sys_time.h" "$work/ov/sys/sys_time.h"
src="$root/tests/sdk/sys-timer-surface-test.c"
status=0

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/p.o" 2>&1); then
                echo "sys-timer-surface: ok   compile $label"
            else
                echo "sys-timer-surface: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# check_numbers <object> <label>: every wrapper's last "li r11,N" before its
# sc must be the expected number.  Prints one line per wrapper; returns 1 on
# any mismatch or missing sc.
check_numbers() {
    local obj=$1 label=$2 rc=0 fn want got
    "$objdump" -d "$obj" > "$work/dis.txt" 2>&1 || { echo "sys-timer-surface: FAIL objdump $label"; return 1; }
    for pair in t_create:70 t_destroy:71 t_info:72 t_start:73 t_stop:74 \
                t_connect:75 t_disconnect:76 t_usleep:141 t_sleep:142; do
        fn=${pair%%:*}; want=${pair#*:}
        # -O2 objects use one section per function, so objdump labels the
        # function either <name>: or <.text.name>:.
        got=$(awk -v fn="<$fn>:" -v sfn="<.text.$fn>:" '
            $2 == fn || $2 == sfn { inside = 1; next }
            inside && /^$/ { exit }
            inside && $0 ~ /[[:space:]]li[[:space:]]+r11,/ { n = $0; sub(/.*li[[:space:]]+r11,/, "", n); sub(/[^0-9].*/, "", n); last = n }
            inside && $0 ~ /[[:space:]]sc([[:space:]]|$)/ { print last; exit }
        ' "$work/dis.txt")
        if [ "$got" = "$want" ]; then
            echo "sys-timer-surface: ok   $label $fn -> sc $want"
        else
            echo "sys-timer-surface: FAIL $label $fn: want sc $want, got '${got:-none}'"
            rc=1
        fi
    done
    return $rc
}

for abi in "" -mlp64; do
    label="${abi:-ilp32}"
    if "$cc" -x c -std=gnu99 $abi -O2 -I"$work/ov" -I"$sdk/ppu/include" \
            -c "$src" -o "$work/n.o" 2>"$work/n.err"; then
        check_numbers "$work/n.o" "$label" || status=1
        # sys_time_get_system_time: a call relocated to the import, no sc
        # (an inline sys_time_get_current_time returned wall-clock time).
        "$objdump" -dr "$work/n.o" > "$work/r.txt"
        st=$(awk '$2 == "<t_systime>:" || $2 == "<.text.t_systime>:" { inside = 1; next }
                  inside && /^$/ { exit }
                  inside && /R_PPC64_REL24[[:space:]]+sys_time_get_system_time/ { call = 1 }
                  inside && /[[:space:]]sc([[:space:]]|$)/ { sc = 1 }
                  END { print (call && !sc) ? "import" : (sc ? "inline-sc" : "none") }' "$work/r.txt")
        if [ "$st" = import ]; then
            echo "sys-timer-surface: ok   $label sys_time_get_system_time -> sysPrxForUser import"
        else
            echo "sys-timer-surface: FAIL $label sys_time_get_system_time is '$st', not the import"; status=1
        fi
    else
        echo "sys-timer-surface: FAIL -O2 compile $label"; sed 's/^/    /' "$work/n.err" | head -5; status=1
    fi
done

# Self-check: the same check on a header whose stop wrapper uses 75 must fail.
mkdir -p "$work/bad/sys"
sed 's/^#define SYS_TIMER_STOP  *74$/#define SYS_TIMER_STOP                     75/' \
    "$inc/sys/timer.h" > "$work/bad/sys/timer.h"
if cmp -s "$inc/sys/timer.h" "$work/bad/sys/timer.h"; then
    echo "sys-timer-surface: FAIL self-check could not plant the wrong number"; status=1
elif "$cc" -x c -std=gnu99 -O2 -I"$work/bad" -I"$sdk/ppu/include" -c "$src" -o "$work/bad.o" 2>/dev/null \
        && ! check_numbers "$work/bad.o" "self-check" > "$work/self.txt"; then
    # Caught for the right reason: t_stop reports the planted 75, and it is
    # the only failure (the other eight wrappers still check out).
    if grep -q "FAIL self-check t_stop: want sc 74, got '75'" "$work/self.txt" \
            && [ "$(grep -c FAIL "$work/self.txt")" -eq 1 ] \
            && [ "$(grep -c ' ok ' "$work/self.txt")" -eq 8 ]; then
        echo "sys-timer-surface: ok   self-check: a wrong syscall number is caught"
    else
        echo "sys-timer-surface: FAIL self-check failed for the wrong reason"; status=1
    fi
else
    echo "sys-timer-surface: FAIL self-check: a wrong syscall number was not caught"; status=1
fi

[ $status -eq 0 ] && echo "PASS: sys-timer-surface-test"
exit $status
