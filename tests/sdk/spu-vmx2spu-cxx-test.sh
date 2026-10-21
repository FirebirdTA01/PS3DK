#!/usr/bin/env bash
# The SPU compiler's <vmx2spu.h> compiles in C++ (SPU gcc patch 0005).  vec_sum2s passed an unparenthesised
# compound literal, (vec_int4){0, -1, 0, -1}, to the spu_and macro; the preprocessor split it at the commas
# ("macro spu_and passed 5 arguments, but takes 2"), so no C++ source could include the header (it is a
# C++-only header: empty in C).  Compiles a source that includes it and calls vec_sum2s as C++ (default, c++11,
# c++17), -O2 -Wall -Werror, with the installed SPU compiler.  Skips without PS3DEV.  usage: spu-vmx2spu-cxx-test.sh [--ps3dev DIR]
set -u
ps3dev="${PS3DEV:-}"; [ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then echo "spu-vmx2spu-cxx: SKIP (set PS3DEV or --ps3dev)"; exit 0; fi
cc="$ps3dev/spu/bin/spu-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "spu-vmx2spu-cxx: FAIL: no SPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cat > "$work/t.c" <<'SRC'
#include <vmx2spu.h>
vec_int4 sum2s_probe(vec_int4 a, vec_int4 b) { return vec_sum2s(a, b); }
SRC
status=0
for mode in "c++ " "c++ -std=c++11" "c++ -std=c++17"; do
    lang=${mode%% *}; std=${mode#* }
    if out=$("$cc" -x "$lang" $std -O2 -Wall -Werror -c "$work/t.c" -o "$work/t.o" 2>&1); then
        echo "spu-vmx2spu-cxx: ok   $lang ${std:-default}"
    else
        echo "spu-vmx2spu-cxx: FAIL $lang ${std:-default}"; printf '%s\n' "$out" | grep -E "error" | head -3 | sed 's/^/    /'; status=1
    fi
done
[ $status -eq 0 ] && echo "PASS: spu-vmx2spu-cxx-test" || echo "FAIL: spu-vmx2spu-cxx-test"
exit $status
