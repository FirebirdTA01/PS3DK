#!/usr/bin/env bash
# The PPU compiler searches the SDK headers with no -I (GCC patch 0044), as
# the reference driver does, and places them exactly where an explicit
# -I$PS3DK/ppu/include always put them: ahead of libstdc++, GCC's own
# headers and newlib, so the libc wrappers' #include_next chains are
# unchanged.  An explicit -I of the same directory keeps working.
#
# usage: ppu-default-include-test.sh [--ps3dev DIR] [-B DIR]
set -u
ps3dev="${PS3DEV:-}"; bdir=""
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        -B) bdir="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [-B DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "ppu-default-include: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
sdk="${PS3DK:-$ps3dev/ps3dk}/ppu/include"
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
B=(); [ -n "$bdir" ] && B=("-B$bdir")
status=0
fail() { echo "ppu-default-include: FAIL: $*"; status=1; }
ok() { echo "ppu-default-include: ok   $*"; }
[ -x "$cc" ] && [ -x "$cxx" ] || { fail "no PPU compiler under $ps3dev"; exit 1; }
[ -f "$sdk/pthread.h" ] || { fail "no SDK headers at $sdk"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
sdk_real=$(cd "$sdk" && pwd -P)
# libstdc++'s and GCC's own header directories.
libdirs='/include/c\+\+/|/gcc/powerpc64-ps3-elf/[^/]*/include$'

# search_list <driver> <lang> [flags...]: the <...> search list, one
# canonical directory per line, or a failure.
search_list() {
    local drv="$1" lang="$2"; shift 2
    if ! "$drv" "${B[@]}" "$@" -x "$lang" -E -v /dev/null -o /dev/null \
            > "$work/v.log" 2>&1; then
        return 1
    fi
    sed -n '/#include <...> search starts here:/,/End of search list./p' "$work/v.log" \
        | sed '1d;$d' | while read -r d; do (cd "$d" 2>/dev/null && pwd -P) || echo "$d"; done
}

for abi in "" -mlp64; do
    tag="${abi:-ilp32}"
    for lang in c c++; do
        drv="$cc"; [ "$lang" = c++ ] && drv="$cxx"
        if ! list=$(search_list "$drv" "$lang" $abi); then
            fail "$tag $lang: -v -E failed: $(head -1 "$work/v.log")"; continue
        fi
        # The SDK directory must come before libstdc++ and GCC's own headers,
        # where an explicit -I used to put it.  The only directory allowed
        # ahead of it is the other SDK layout's (the compiler prefix's
        # include directory), which a development stage leaves empty.
        pos=$(printf '%s\n' "$list" | grep -nxF "$sdk_real" | cut -d: -f1)
        lib=$(printf '%s\n' "$list" | grep -nE "$libdirs" | head -1 | cut -d: -f1)
        ahead=$(printf '%s\n' "$list" | head -n $(( ${pos:-1} - 1 )) | grep -v '/ppu/include$' | wc -l)
        if [ -n "$pos" ] && [ -n "$lib" ] && [ "$pos" -lt "$lib" ] && [ "$ahead" -eq 0 ]; then
            ok "$tag $lang: the SDK headers come before libstdc++ and GCC's headers"
        else
            fail "$tag $lang: SDK directory at ${pos:-absent}, first library directory at ${lib:-?}: $(printf '%s ' $list)"
        fi
        # With an explicit -I of the same directory it is still searched
        # once, still ahead of the library headers.
        if ! list_i=$(search_list "$drv" "$lang" $abi "-I$sdk"); then
            fail "$tag $lang -I: -v -E failed"; continue
        fi
        n=$(printf '%s\n' "$list_i" | grep -cxF "$sdk_real")
        pos_i=$(printf '%s\n' "$list_i" | grep -nxF "$sdk_real" | cut -d: -f1)
        lib_i=$(printf '%s\n' "$list_i" | grep -nE "$libdirs" | head -1 | cut -d: -f1)
        [ "$n" -eq 1 ] && [ -n "$lib_i" ] && [ "${pos_i:-99}" -lt "$lib_i" ] \
            && ok "$tag $lang: an explicit -I of the SDK directory leaves it searched once, ahead of the library headers" \
            || fail "$tag $lang: with -I the SDK directory appears $n times, at ${pos_i:-?} vs library ${lib_i:-?}"
        # Everything after it keeps the compiler's own order.
        rest=$(printf '%s\n' "$list" | grep -vxF "$sdk_real" | grep -v '/ps3dk/ppu/include$')
        gccinc=$(printf '%s\n' "$rest" | grep -n '/gcc/powerpc64-ps3-elf/[^/]*/include$' | cut -d: -f1)
        fixed=$(printf '%s\n' "$rest" | grep -n '/include-fixed$' | cut -d: -f1)
        tool=$(printf '%s\n' "$rest" | grep -n '/powerpc64-ps3-elf/include$' | cut -d: -f1)
        if [ -n "$gccinc" ] && [ -n "$fixed" ] && [ -n "$tool" ] \
                && [ "$gccinc" -lt "$fixed" ] && [ "$fixed" -lt "$tool" ]; then
            ok "$tag $lang: GCC, include-fixed and newlib keep their order"
        else
            fail "$tag $lang: default order changed: $(printf '%s ' $rest)"
        fi
    done

    # No -I at all: SDK declarations are found (newlib alone lacks them).
    cat > "$work/p.c" <<'EOF'
#include <pthread.h>
#include <sys/process.h>
int probe(pthread_attr_t *a)
{
    struct sched_param p;
    return pthread_attr_getschedparam(a, &p) + pthread_attr_setschedparam(a, &p);
}
EOF
    if "$cc" "${B[@]}" $abi -std=gnu99 -Wall -Werror -c "$work/p.c" -o "$work/p.o" > "$work/p.log" 2>&1; then
        ok "$tag: <pthread.h> and <sys/process.h> resolve with no -I"
    else
        fail "$tag: no-I SDK compile: $(grep -m1 -E 'error' "$work/p.log")"
    fi
    # The C++ wrappers still stack correctly with and without -I.
    cat > "$work/w.cpp" <<'EOF'
#include <stdarg.h>
#include <stdlib.h>
#include <cstdlib>
#include <cstdio>
std::va_list *keep_va_list_type;
int probe(int x) { return std::abs(x) + abs(x); }
EOF
    for inc in "" "-I$sdk"; do
        label="no -I"; [ -n "$inc" ] && label="-I\$PS3DK/ppu/include"
        if "$cxx" "${B[@]}" $abi $inc -std=c++17 -Wall -Werror -c "$work/w.cpp" -o "$work/w.o" > "$work/w.log" 2>&1; then
            ok "$tag C++ ($label): std::va_list from <stdarg.h>, stdlib wrappers stack"
        else
            fail "$tag C++ ($label): $(grep -m1 -E 'error' "$work/w.log")"
        fi
    done
done

[ "$status" -eq 0 ] && echo "ppu-default-include: PASS"
exit $status
