#!/usr/bin/env bash
# The SDK <stdarg.h> wrapper must forward on every inclusion.  GCC's header
# is entered first in a partial mode (newlib's <wchar.h> defines
# __need___va_list before including it) and again later in full mode; if
# the wrapper swallows the second inclusion, va_list and va_start never
# appear and the translation unit fails to compile.
#
# usage: stdarg-reentry-test.sh [--ps3dev DIR] [--include DIR]
#   --include  the SDK header directory to test (default: the installed
#              $PS3DK/ppu/include)
set -u
ps3dev="${PS3DEV:-}"; inc=""
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "stdarg-reentry: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
[ -n "$inc" ] || inc="${PS3DK:-$ps3dev/ps3dk}/ppu/include"
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
status=0
fail() { echo "stdarg-reentry: FAIL: $*"; status=1; }
ok() { echo "stdarg-reentry: ok   $*"; }
[ -x "$cc" ] && [ -x "$cxx" ] || { fail "no PPU compiler under $ps3dev"; exit 1; }
[ -f "$inc/stdarg.h" ] || { fail "no SDK <stdarg.h> in $inc"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Every probe ends by really using the variadic machinery.
use_c='int sum(int n, ...) { va_list ap; va_start(ap, n); int r = va_arg(ap, int); va_end(ap); return r; }'
use_cxx='int sum(int n, ...) { std::va_list ap; va_start(ap, n); int r = va_arg(ap, int); va_end(ap); return r; }'

probe() {  # <name> <lang c|c++> <body>
    local name="$1" lang="$2" body="$3" drv="$cc" ext=c
    [ "$lang" = c++ ] && { drv="$cxx"; ext=cpp; }
    printf '%s\n' "$body" > "$work/$name.$ext"
    for abi in "" -mlp64; do
        if "$drv" $abi -I"$inc" -Wall -Werror -c "$work/$name.$ext" -o "$work/$name.o" \
                > "$work/$name.log" 2>&1; then
            ok "$name ${abi:-ilp32}"
        else
            fail "$name ${abi:-ilp32}: $(grep -m1 error "$work/$name.log")"
        fi
    done
}

probe partial-then-full-c c "#define __need___va_list
#include <stdarg.h>
#undef __need___va_list
#include <stdarg.h>
$use_c"
probe wchar-then-stdarg-c c "#include <wchar.h>
#include <stdarg.h>
$use_c"
probe partial-then-full-cxx c++ "#define __need___va_list
#include <stdarg.h>
#undef __need___va_list
#include <stdarg.h>
$use_cxx"
probe wchar-then-cstdarg-cxx c++ "#include <wchar.h>
#include <cstdarg>
$use_cxx"
probe cwchar-then-stdarg-cxx c++ "#include <cwchar>
#include <stdarg.h>
$use_cxx"
probe stdarg-twice-cxx c++ "#include <stdarg.h>
#include <cstdarg>
#include <stdarg.h>
$use_cxx"

[ "$status" -eq 0 ] && echo "stdarg-reentry: PASS"
exit $status
