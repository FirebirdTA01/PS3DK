#!/usr/bin/env bash
# The GCC 16 PPU compiler's plain char, wchar_t and wint_t match the reference toolchain in both data models
# (gcc-16.x 0075, 0076).  Expected values were measured with the reference compiler's -dM -E (C and C++):
#   ILP32 (default): __CHAR_SIGNED__ 1 (no __CHAR_UNSIGNED__), __WCHAR_TYPE__ short unsigned int,
#                    __WCHAR_MAX__ 65535U, __WINT_TYPE__ int
#   -mlp64:          __CHAR_UNSIGNED__ 1 (no __CHAR_SIGNED__), the same wchar_t and wint_t
# Each row checks the predefines AND compiles static_asserts on the types themselves (the macros could lie).
# -fsigned-char / -funsigned-char still override the default.  Skips without PS3DEV or on GCC < 16.
set -u -o pipefail
G=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-gcc
if [ -z "${PS3DEV:-}" ] || [ ! -x "$G" ]; then echo "SKIP: ppu-abi-char-wchar-test (no PS3DEV compiler)"; exit 0; fi
ver=$("$G" -dumpversion) || { echo "ppu-abi-char-wchar: FAIL -dumpversion"; exit 1; }
[ "${ver%%.*}" -ge 16 ] || { echo "SKIP: ppu-abi-char-wchar-test (GCC $ver has no 0075/0076)"; exit 0; }
work=$(mktemp -d) || { echo "ppu-abi-char-wchar: FAIL mktemp"; exit 1; }
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "ppu-abi-char-wchar: FAIL $*"; status=1; }

: > "$work/empty.c" || { echo "ppu-abi-char-wchar: FAIL writing the probe"; exit 1; }
# has <macro line> <text>: 0 present, 1 absent, 2 error
has() { grep -qxF -- "$1" <<<"$2"; local rc=$?; [ $rc -le 1 ] && return $rc; return 2; }

for abi in ilp32 lp64; do
    flag=; [ $abi = lp64 ] && flag=-mlp64
    if [ $abi = ilp32 ]; then want_char='#define __CHAR_SIGNED__ 1'; not_char=__CHAR_UNSIGNED__; signed=1
    else want_char='#define __CHAR_UNSIGNED__ 1'; not_char=__CHAR_SIGNED__; signed=0; fi
    for lang in c c++; do
        dm=$("$G" $flag -x $lang -dM -E "$work/empty.c") || { fail "$abi $lang: -dM -E exited non-zero"; continue; }
        for m in "$want_char" '#define __WCHAR_TYPE__ short unsigned int' '#define __WCHAR_MAX__ 65535U' '#define __WINT_TYPE__ int'; do
            has "$m" "$dm"; case $? in 0) ;; 1) fail "$abi $lang: missing '$m'" ;; *) fail "$abi $lang: could not judge '$m'" ;; esac
        done
        grep -q -- "^#define $not_char " <<<"$dm"; case $? in 1) ;; 0) fail "$abi $lang: $not_char is predefined" ;; *) fail "$abi $lang: could not judge $not_char" ;; esac
    done
    # the types themselves
    cat > "$work/t-$abi.c" <<EOF || { fail "$abi: writing the type probe"; continue; }
#include <stddef.h>
#include <wchar.h>
_Static_assert(sizeof(wchar_t) == 2, "wchar_t is 16-bit");
_Static_assert((wchar_t)-1 > 0, "wchar_t is unsigned");
_Static_assert(sizeof(wint_t) == sizeof(int) && (wint_t)-1 < 0, "wint_t is int");
_Static_assert(sizeof(L"ab") == 6, "L\"\" literals are 16-bit units");
_Static_assert(((char)-1 < 0) == $signed, "plain char signedness");
int x;
EOF
    "$G" $flag -std=c11 -c "$work/t-$abi.c" -o "$work/t-$abi.o" > "$work/t-$abi.log" 2>&1 \
        || fail "$abi: type asserts: $(grep -m2 -i 'error' "$work/t-$abi.log")"
    # explicit options keep winning
    for o in -fsigned-char:1 -funsigned-char:0; do
        opt=${o%%:*}; s=${o#*:}
        printf '_Static_assert(((char)-1 < 0) == %s, "override");\nint y;\n' "$s" > "$work/o.c" || { fail "writing the override probe"; continue; }
        "$G" $flag $opt -std=c11 -c "$work/o.c" -o "$work/o.o" > "$work/o.log" 2>&1 || fail "$abi: $opt is not honoured"
    done
    [ $status -eq 0 ] && echo "ppu-abi-char-wchar: ok $abi (predefines in C and C++, type asserts, -f[un]signed-char overrides)"
done
[ $status -eq 0 ] && echo "PASS: ppu-abi-char-wchar-test"
exit $status
