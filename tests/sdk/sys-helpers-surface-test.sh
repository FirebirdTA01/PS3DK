#!/usr/bin/env bash
# <sys/random_number.h>, <sys/spinlock.h> and <sys/mempool.h> (see
# sys-helpers-surface-test.c): the eleven sysPrxForUser imports this change
# ships.
#  1. compile with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
#     under -Wall -Wextra -Werror (the layout asserts run in every mode);
#  2. a redefinition check: including <lv2/system.h> then <sys/random_number.h>
#     under -Werror compiles (RANDOM_NUMBER_MAX_SIZE is spelt identically);
#  3. at -O2, each of the eleven imports is a call relocated to its own name
#     and no sc (an inline body proves it is not an import).
#
# Self-check: a mempool.h whose sys_mempool_create is inlined over the
# syscall must fail exactly the sys_mempool_create import row.
#
# usage: sys-helpers-surface-test.sh [--ps3dev DIR] [--include DIR]
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
    echo "sys-helpers-surface: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
objdump="$ps3dev/ppu/bin/powerpc64-ps3-elf-objdump"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sys-helpers-surface: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/ov/sys"
cp "$inc/sys/random_number.h" "$inc/sys/spinlock.h" "$inc/sys/mempool.h" "$work/ov/sys/"
src="$root/tests/sdk/sys-helpers-surface-test.c"
status=0

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/p.o" 2>&1); then
                echo "sys-helpers-surface: ok   compile $label"
            else
                echo "sys-helpers-surface: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# Redefinition check: <lv2/system.h> and <sys/random_number.h> both define
# RANDOM_NUMBER_MAX_SIZE; a token-identical spelling is a legal redundant
# macro redefinition and compiles clean under -Werror.  If the two do not
# match, -Werror rejects the redefinition and this row fails.  <lv2/system.h>
# is a PSL1GHT header, installed with the SDK, not in this tree.
if [ -f "$sdk/ppu/include/lv2/system.h" ]; then
    probe=$(mktemp -d)
    printf '#include <lv2/system.h>\n#include <sys/random_number.h>\n' > "$probe/red.c"
    if out=$("$cc" -x c -std=gnu99 -Wall -Wextra -Werror \
            -I"$work/ov" -I"$sdk/ppu/include" \
            -c "$probe/red.c" -o "$probe/red.o" 2>&1); then
        echo "sys-helpers-surface: ok   redefinition (lv2/system.h + sys/random_number.h)"
    else
        echo "sys-helpers-surface: FAIL redefinition: RANDOM_NUMBER_MAX_SIZE is not spelt identically"
        printf '%s\n' "$out" | grep -E "error|RANDOM_NUMBER" | sed 's/^/    /' | head -6
        status=1
    fi
    rm -rf "$probe"
else
    echo "sys-helpers-surface: FAIL redefinition: no lv2/system.h under $sdk/ppu/include"; status=1
fi

IMPORTS="sys_get_random_number
sys_spinlock_initialize sys_spinlock_lock sys_spinlock_trylock sys_spinlock_unlock
sys_mempool_create sys_mempool_allocate_block sys_mempool_try_allocate_block
sys_mempool_free_block sys_mempool_destroy sys_mempool_get_count"

body() {
    awk -v fn="<$2>:" -v sfn="<.text.$2>:" '
        $2 == fn || $2 == sfn { inside = 1; next }
        inside && /^$/ { exit }
        inside { print }' "$1"
}

check() {
    local obj="$1" label="$2" rc=0 name b
    "$objdump" -dr "$obj" > "$work/dis.txt" 2>&1 || { echo "sys-helpers-surface: FAIL objdump $label"; return 1; }
    for name in $IMPORTS; do
        b=$(body "$work/dis.txt" "v_$name")
        if printf '%s\n' "$b" | grep -qE "R_PPC64_REL24[[:space:]]+$name\$" &&
           ! printf '%s\n' "$b" | grep -qE '[[:space:]]sc([[:space:]]|$)'; then
            echo "sys-helpers-surface: ok   $label $name -> sysPrxForUser import"
        else
            echo "sys-helpers-surface: FAIL $label $name is not a call to the import"
            rc=1
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
        echo "sys-helpers-surface: FAIL -O2 compile $label"; sed 's/^/    /' "$work/n.err" | head -5; status=1
    fi
done

# Self-check: a mempool.h whose sys_mempool_create is inlined must fail
# exactly the sys_mempool_create import row (the other ten imports stay
# clean).  A macro that expands to a constant drops the R_PPC64_REL24 from
# the wrapper; the "is a call to the import" rule then reports FAIL for
# exactly that wrapper.
mkdir -p "$work/badimp/sys"
cp "$inc/sys/random_number.h" "$inc/sys/spinlock.h" "$work/badimp/sys/"
{
    cat "$inc/sys/mempool.h"
    echo ''
    echo '#define sys_mempool_create(m, c, cs, bs, ra) (0)'
} > "$work/badimp/sys/mempool.h"
selfcheck() {
    local dir="$1" expect="$2"
    if "$cc" -x c -std=gnu99 -O2 -I"$dir" -I"$sdk/ppu/include" -c "$src" -o "$work/bad.o" 2>/dev/null \
            && ! check "$work/bad.o" self-check > "$work/self.txt"; then
        if grep -qF "$expect" "$work/self.txt" && [ "$(grep -c FAIL "$work/self.txt")" -eq 1 ]; then
            echo "sys-helpers-surface: ok   self-check: an inlined import is caught"
        else
            echo "sys-helpers-surface: FAIL self-check failed for the wrong reason"; status=1
        fi
    else
        echo "sys-helpers-surface: FAIL self-check: an inlined import was not caught"; status=1
    fi
}
selfcheck "$work/badimp" "FAIL self-check sys_mempool_create is not a call to the import"

[ $status -eq 0 ] && echo "PASS: sys-helpers-surface-test"
exit $status
