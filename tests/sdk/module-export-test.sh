#!/usr/bin/env bash
# <sys/moduleexport.h>: SYS_MODULE_INFO must record exactly the module's
# name, attributes and version in the non-loaded .ps3dk.module_identity
# section that `prx-gen build' reads; SYS_MODULE_START / SYS_MODULE_STOP must
# bind module_start / module_stop to the named functions (and, after a real
# PRX link, override the crt's weak defaults); and every unsupported or
# out-of-range use must fail to compile.  C (gnu99, c11) and C++17, both PPU
# data models, -Wall -Wextra -Werror.  Two header mutants prove the checks
# notice a wrong record and a missing binding.
#
# usage: module-export-test.sh [--ps3dev DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "module-export: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
bin="$ps3dev/ppu/bin"
cc="$bin/powerpc64-ps3-elf-gcc"; cxx="$bin/powerpc64-ps3-elf-g++"
for t in "$cc" "$cxx"; do
    [ -x "$t" ] || [ -x "$t.exe" ] || { echo "module-export: FAIL: missing $t"; exit 1; }
done
py=$(command -v python3 || command -v python) || { echo "module-export: FAIL: no python"; exit 1; }
src="$root/tests/sdk/module-export-test.c"
installed="$ps3dev/ps3dk/ppu/include"
[ -d "$installed" ] || installed="$ps3dev/ppu/include"
status=0; passed=0
fail() { echo "module-export: FAIL: $*"; status=1; }
ok() { echo "module-export: ok   $*"; passed=$((passed + 1)); }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

stage() {  # stage <dir> [mutant-sed-expression]
    mkdir -p "$1/sys"
    cp "$root/sdk/include/sys/prx_module.h" "$1/sys/prx_module.h"
    if [ -n "${2:-}" ]; then
        sed "$2" "$root/sdk/include/sys/moduleexport.h" > "$1/sys/moduleexport.h"
        cmp -s "$1/sys/moduleexport.h" "$root/sdk/include/sys/moduleexport.h" \
            && { echo "module-export: FAIL: mutant '$2' did not change the header"; exit 1; }
    else
        cp "$root/sdk/include/sys/moduleexport.h" "$1/sys/moduleexport.h"
    fi
}
stage "$work/inc"

# Expected record: "PMI\1", attributes 0x0005, version 3.9, name in 28 bytes.
expected=$("$py" -c '
import sys
name = b"probe_mod_27_character_long"
rec = b"PMI\x01" + (5).to_bytes(2, "big") + bytes([3, 9]) + name.ljust(28, b"\0")
sys.stdout.write(rec.hex())')

compile() {  # compile <lang> <std> <abi> <incdir> <out> [defines...]
    local lang=$1 std=$2 abi=$3 inc=$4 out=$5; shift 5
    local drv="$cc"; [ "$lang" = c++ ] && drv="$cxx"
    "$drv" -x "$lang" "-std=$std" $abi -O2 -Wall -Wextra -Werror "$@" \
        -I"$inc" -I"$installed" -c "$src" -o "$out"
}

crt=""
[ -f "$ps3dev/ppu/lib/lv2-prx-crt.o" ] && [ -f "$ps3dev/ppu/lib/lv2-prx.ld" ] && crt="$ps3dev/ppu/lib/lv2-prx-crt.o"
link_prx() {  # link_prx <abi> <object> <out>
    [ -n "$crt" ] || return 0
    "$cc" $1 -specs="$ps3dev/ppu/lib/lv2-prx.specs" -nostartfiles -Wl,-q         -Wl,--no-warn-rwx-segments -Wl,-T,"$ps3dev/ppu/lib/lv2-prx.ld" "$2" "$crt" -o "$3"
}
inspect="$root/tests/sdk/module-export-inspect.py"
check_object() {  # check_object <object|linked> <file> -> prints problems, returns non-zero if any
    "$py" -I -B "$inspect" "$1" "$2" "$expected" probe_start probe_stop
}

for lang in c c++; do
    stds="gnu99 c11"; [ "$lang" = c++ ] && stds="c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            o="$work/good-$lang-$std${abi}.o"
            if ! compile "$lang" "$std" "$abi" "$work/inc" "$o" > "$work/e.log" 2>&1; then
                fail "$label: $(grep -m1 -E 'error' "$work/e.log")"; continue
            fi
            if why=$(check_object object "$o"); then ok "$label record and bindings"; else fail "$label: $why"; fi
            for c in NAME_TOO_LONG:"module name is too long" NAME_EMPTY:"module name is empty" \
                     ATTR_TOO_WIDE:"attributes must fit 16 bits" VERSION_TOO_WIDE:"version parts must fit 8 bits" \
                     EXIT:"SYS_MODULE_EXIT is not supported" STOP_LEVEL:"SYS_MODULE_STOP_LEVEL is not supported"; do
                case=${c%%:*} msg=${c#*:}
                if compile "$lang" "$std" "$abi" "$work/inc" "$work/neg.o" "-DCASE_$case" > "$work/n.log" 2>&1; then
                    fail "$label CASE_$case compiled"
                elif grep -q "$msg" "$work/n.log"; then
                    ok "$label CASE_$case refused"
                else
                    fail "$label CASE_$case failed without '$msg': $(grep -m1 error "$work/n.log")"
                fi
            done
            # A binding whose target is not defined must not build: the crt's
            # weak default would otherwise run instead of the named function.
            for case in MISSING_START MISSING_STOP; do
                if ! compile "$lang" "$std" "$abi" "$work/inc" "$work/miss.o" "-DCASE_$case" > "$work/n.log" 2>&1; then
                    ok "$label CASE_$case refused at compile time"
                elif [ -z "$crt" ]; then
                    fail "$label CASE_$case compiled (no PRX crt to try the link)"
                elif link_prx "$abi" "$work/miss.o" "$work/miss.elf" > "$work/l.log" 2>&1; then
                    fail "$label CASE_$case compiled and linked"
                else
                    ok "$label CASE_$case refused at link time"
                fi
            done
        done
    done
done

# Mutants: the checks above must notice a wrong record and a missing binding.
# Each mutant still compiles; only the record/binding checks may catch it.
for m in 's/XSTR(major) ", "/XSTR(minor) ", "/' \
         's/module_identity,\\"\\",/module_identity,\\"a\\",/' \
         's/__PS3DK_MODEXP_BIND(module_stop, fn)/extern int __ps3dk_unbound/'; do
    stage "$work/mut" "$m"
    if ! compile c c11 "" "$work/mut" "$work/mut.o" > "$work/m.log" 2>&1; then
        fail "mutant '$m' did not compile: $(grep -m1 error "$work/m.log")"
    elif check_object object "$work/mut.o" > /dev/null; then
        fail "mutant '$m' was not caught"
    else
        ok "mutant '$m' caught"
    fi
    rm -rf "$work/mut"
done

# A real PRX link: the source's entry points must replace the crt's weak
# defaults, and the management record must point at them.
if [ -n "$crt" ]; then
    for abi in "" -mlp64; do
        o="$work/good-c-c11${abi}.o"; e="$work/link${abi}.elf"
        if link_prx "$abi" "$o" "$e" > "$work/l.log" 2>&1; then
            if why=$(check_object linked "$e"); then ok "${abi:-ilp32} PRX link: record, bindings and management record"
            else fail "${abi:-ilp32} PRX link: $why"; fi
        else
            fail "${abi:-ilp32} PRX link: $(grep -m1 -iE 'error|undefined' "$work/l.log")"
        fi
    done
else
    echo "module-export: note: no PRX crt under $ps3dev; link rows skipped"
fi

[ $status -eq 0 ] && echo "module-export: PASS ($passed checks)" || echo "module-export: FAIL"
exit $status
