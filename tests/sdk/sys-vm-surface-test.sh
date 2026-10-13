#!/usr/bin/env bash
# <sys/vm.h> and the <sys/memory.h> additions (see sys-vm-surface-test.c):
#  1. compile with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
#     under -Wall -Wextra -Werror (the layout asserts run in every mode);
#  2. at -O2, each syscall wrapper loads its own number into r11 before its
#     sc: sys_vm_* 300-312, sys_memory_container_get_size 343,
#     sys_memory_get_page_attribute 351;
#  3. the five mmapper memory-object calls are sysPrxForUser imports: a call
#     relocated to the function's own name and no sc.
# Self-checks: a header with SYS_VM_SYNC planted as 311 must fail exactly
# the sys_vm_sync row, and a memory.h whose sys_mmapper_free_memory is an
# inline syscall must fail exactly that import row.
#
# usage: sys-vm-surface-test.sh [--ps3dev DIR] [--include DIR]
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
    echo "sys-vm-surface: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
objdump="$ps3dev/ppu/bin/powerpc64-ps3-elf-objdump"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sys-vm-surface: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/ov/sys"
cp "$inc/sys/vm.h" "$inc/sys/memory.h" "$work/ov/sys/"
src="$root/tests/sdk/sys-vm-surface-test.c"
status=0

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/p.o" 2>&1); then
                echo "sys-vm-surface: ok   compile $label"
            else
                echo "sys-vm-surface: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

NUMBERS="v_sys_vm_memory_map:300 v_sys_vm_unmap:301 v_sys_vm_append_memory:302
v_sys_vm_return_memory:303 v_sys_vm_lock:304 v_sys_vm_unlock:305 v_sys_vm_touch:306
v_sys_vm_flush:307 v_sys_vm_invalidate:308 v_sys_vm_store:309 v_sys_vm_sync:310
v_sys_vm_test:311 v_sys_vm_get_statistics:312 v_sys_memory_container_get_size:343
v_sys_memory_get_page_attribute:351"
IMPORTS="sys_mmapper_allocate_memory sys_mmapper_allocate_memory_from_container
sys_mmapper_free_memory sys_mmapper_map_memory sys_mmapper_unmap_memory"

# body <objdump -dr file> <function>: the function's lines (named <f>: or
# <.text.f>: under -ffunction-sections).
body() {
    awk -v fn="<$2>:" -v sfn="<.text.$2>:" '
        $2 == fn || $2 == sfn { inside = 1; next }
        inside && /^$/ { exit }
        inside { print }' "$1"
}

# check <object> <label>: one line per syscall row and per import row.
check() {
    local obj=$1 label=$2 rc=0 pair fn want got name b
    "$objdump" -dr "$obj" > "$work/dis.txt" 2>&1 || { echo "sys-vm-surface: FAIL objdump $label"; return 1; }
    for pair in $NUMBERS; do
        fn=${pair%%:*}; want=${pair#*:}
        got=$(body "$work/dis.txt" "$fn" | awk '
            /[[:space:]]li[[:space:]]+r11,/ { n = $0; sub(/.*li[[:space:]]+r11,/, "", n); sub(/[^0-9].*/, "", n); last = n }
            /[[:space:]]sc([[:space:]]|$)/ { print last; exit }')
        if [ "$got" = "$want" ]; then
            echo "sys-vm-surface: ok   $label $fn -> sc $want"
        else
            echo "sys-vm-surface: FAIL $label $fn: want sc $want, got '${got:-none}'"; rc=1
        fi
    done
    for name in $IMPORTS; do
        b=$(body "$work/dis.txt" "v_$name")
        if printf '%s\n' "$b" | grep -qE "R_PPC64_REL24[[:space:]]+$name\$" &&
           ! printf '%s\n' "$b" | grep -qE '[[:space:]]sc([[:space:]]|$)'; then
            echo "sys-vm-surface: ok   $label $name -> sysPrxForUser import"
        else
            echo "sys-vm-surface: FAIL $label $name is not a call to the import"; rc=1
        fi
    done
    return $rc
}

for abi in "" -mlp64; do
    label="${abi:-ilp32}"
    if "$cc" -x c -std=gnu99 $abi -O2 -I"$work/ov" -I"$sdk/ppu/include" \
            -c "$src" -o "$work/n.o" 2>"$work/n.err"; then
        check "$work/n.o" "$label" || status=1
    else
        echo "sys-vm-surface: FAIL -O2 compile $label"; sed 's/^/    /' "$work/n.err" | head -5; status=1
    fi
done

# selfcheck <label> <dir> <expected FAIL line>: exactly one FAIL, that one.
selfcheck() {
    local label=$1 dir=$2 expect=$3
    if "$cc" -x c -std=gnu99 -O2 -I"$dir" -I"$sdk/ppu/include" -c "$src" -o "$work/bad.o" 2>/dev/null \
            && ! check "$work/bad.o" self-check > "$work/self.txt"; then
        if grep -qF "$expect" "$work/self.txt" && [ "$(grep -c FAIL "$work/self.txt")" -eq 1 ]; then
            echo "sys-vm-surface: ok   self-check: $label is caught"
        else
            echo "sys-vm-surface: FAIL self-check $label failed for the wrong reason"; status=1
        fi
    else
        echo "sys-vm-surface: FAIL self-check: $label was not caught"; status=1
    fi
}
mkdir -p "$work/badnum/sys" "$work/badimp/sys"
sed 's/^#define SYS_VM_SYNC  *310$/#define SYS_VM_SYNC             311/' "$inc/sys/vm.h" > "$work/badnum/sys/vm.h"
cp "$inc/sys/memory.h" "$work/badnum/sys/memory.h"
if cmp -s "$inc/sys/vm.h" "$work/badnum/sys/vm.h"; then
    echo "sys-vm-surface: FAIL self-check could not plant the wrong number"; status=1
else
    selfcheck "a wrong syscall number" "$work/badnum" "FAIL self-check v_sys_vm_sync: want sc 310, got '311'"
fi
cp "$inc/sys/vm.h" "$work/badimp/sys/vm.h"
{ cat "$inc/sys/memory.h"; echo '#define sys_mmapper_free_memory(id) ((int)sys_vm_unmap(id))'; } > "$work/badimp/sys/memory.h"
selfcheck "an inlined import" "$work/badimp" "FAIL self-check sys_mmapper_free_memory is not a call to the import"

[ $status -eq 0 ] && echo "PASS: sys-vm-surface-test"
exit $status
