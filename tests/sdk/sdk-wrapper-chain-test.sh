#!/usr/bin/env bash
# The libc wrapper headers in sdk/include forward to libc with #include_next
# OUTSIDE their include guards, so that every copy of a wrapper on the include
# path chains on and libc's header is always reached.  The SDK's own build has
# two copies on the path (sdk/include and the installed $PS3DK/ppu/include);
# with the forward inside the guard the second copy was skipped whole, libc's
# header was never read, and libPSGL compiled with implicit declarations of
# strncmp, memcpy, printf, sinf ... (hard errors from GCC 14 on).
#
#  1. static (always): each chaining wrapper below has its #include_next
#     before its include guard's #ifndef.  Controls: a synthetic wrapper with
#     the forward inside the guard is rejected, a chained one accepted.
#  2. compile (needs a PPU compiler): two copies of sdk/include on the path,
#     no -D__PS3DK_SDK_SELFBUILD__; for each wrapper a C unit calls functions
#     that only libc's header declares, under
#     -Werror=implicit-function-declaration, ILP32 and LP64.  Including the
#     header twice must also be clean (in strict C11 too, for pthread.h's
#     first-inclusion check).
#
# usage: sdk-wrapper-chain-test.sh [--ps3dev DIR] [--include DIR]
#   --include DIR tests another sdk/include tree (e.g. an older one, as a red control).
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
T=sdk-wrapper-chain
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
status=0
WRAPPERS="string.h stdio.h stdlib.h math.h time.h sys/types.h sys/cdefs.h sys/stat.h pthread.h"

# forward_first <file>: 0 if the first #include_next precedes the first #ifndef.
forward_first() {
    awk '
        /^[ \t]*#[ \t]*include_next[ \t]*[<"]/ && !inc { inc = NR }
        /^[ \t]*#[ \t]*ifndef/ && !grd { grd = NR }
        END { exit !(inc && grd && inc < grd) }' "$1"
}

# 1. static
printf '#ifndef G\n#define G\n#include_next <x.h>\n#endif\n' > "$work/bad.h"
printf '#include_next <x.h>\n#ifndef G\n#define G\n#endif\n' > "$work/good.h"
printf '/* doc:\n  #include_next adds this\n*/\n#ifndef G\n#define G\n#include_next <x.h>\n#endif\n' > "$work/bad-doc.h"
if ! forward_first "$work/bad.h" && ! forward_first "$work/bad-doc.h" && forward_first "$work/good.h"; then
    echo "$T: ok   controls: a forward inside the guard is rejected (also behind a doc comment that mentions #include_next), a chained one accepted"
else
    echo "$T: FAIL controls"; status=1
fi
for w in $WRAPPERS; do
    if [ ! -f "$inc/$w" ]; then echo "$T: FAIL $w: not found under $inc"; status=1
    elif forward_first "$inc/$w"; then echo "$T: ok   $w forwards before its guard"
    else echo "$T: FAIL $w: #include_next is inside the include guard (a second copy on the path is skipped and libc is never reached)"; status=1
    fi
done

# 2. compile
if [ -z "$ps3dev" ]; then
    echo "$T: SKIP compile leg (set PS3DEV or --ps3dev)"
    [ $status -eq 0 ] && echo "PASS: sdk-wrapper-chain-test (static only)"
    exit $status
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "$T: FAIL: no PPU compiler under $ps3dev"; exit 1; }
cp -r "$inc" "$work/copy"
use() {  # use <header>: a call only libc's header declares
    case $1 in
        string.h)    echo 'int f (const char *a, const char *b) { char d[4]; memcpy (d, a, 4); return strncmp (d, b, 4); }' ;;
        stdio.h)     echo 'int f (const char *a, const char *b) { return printf ("%s", a) + puts (b); }' ;;
        stdlib.h)    echo 'int f (const char *a, const char *b) { return (int) strtol (a, 0, 10) + abs ((int) (b != 0)) + (memalign (16, 16) != 0); }' ;;
        math.h)      echo 'int f (const char *a, const char *b) { return (int) (sinf (1.0f) + cosf (2.0f) + sqrtf (4.0f) + M_PI) + (a == b); }' ;;
        time.h)      echo 'int f (const char *a, const char *b) { struct timespec t; return (int) time (0) + clock_gettime (CLOCK_MONOTONIC, &t) + (a == b); }' ;;
        sys/types.h) echo 'pid_t p; off_t o; usecond_t u; int f (const char *a, const char *b) { return (int) sizeof p + (int) sizeof o + (int) sizeof u + (a == b); }' ;;
        sys/cdefs.h) echo '__BEGIN_DECLS int f (const char *a, const char *b); __END_DECLS CDECL_BEGIN int g (void); CDECL_END int f (const char *a, const char *b) { return a == b; }' ;;
        sys/stat.h)  echo 'int f (const char *a, const char *b) { struct stat s; return stat (a, &s) + mkdir (b, 0755) + S_ISDIR (s.st_mode); }' ;;
        pthread.h)   echo 'pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; int f (const char *a, const char *b) { return pthread_mutex_lock (&m) + pthread_mutex_unlock (&m) + (a == b); }' ;;
    esac
}
for w in $WRAPPERS; do
    for twice in 1 2; do
        { echo "#include <$w>"; [ $twice = 2 ] && echo "#include <$w>"; use "$w"; } > "$work/u.c"
        for abi in "" -mlp64; do
            for std in -std=gnu11 -std=c11; do
                [ $std = -std=c11 ] && [ "$w" != pthread.h ] && continue
                label="$w ${abi:-ilp32} $std x$twice"
                "$cc" $abi $std -fsyntax-only -Werror=implicit-function-declaration -Werror=implicit-int \
                    -I"$inc" -I"$work/copy" "$work/u.c" > "$work/err" 2>&1; rc=$?
                if [ $rc -eq 0 ]; then echo "$T: ok   $label"
                else echo "$T: FAIL $label (two copies on the path)"; grep -m3 -E 'error' "$work/err" | sed 's/^/    /'; status=1; fi
            done
        done
    done
done
[ $status -eq 0 ] && echo "PASS: sdk-wrapper-chain-test"
exit $status
