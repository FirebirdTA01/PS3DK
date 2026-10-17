#!/usr/bin/env bash
# The GCC 16 PPU compiler's plain char, wchar_t and wint_t match the reference toolchain in both data models
# (gcc-16.x 0075, 0076).  Expected values were measured with the reference compiler's -dM -E (C and C++):
#   ILP32 (default): __CHAR_SIGNED__ 1 (no __CHAR_UNSIGNED__), __WCHAR_TYPE__ short unsigned int,
#                    __WCHAR_MAX__ 65535U (judged by value), __WINT_TYPE__ int
#   -mlp64:          __CHAR_UNSIGNED__ 1 (no __CHAR_SIGNED__), the same wchar_t and wint_t
# Each row checks the predefines AND compiles C11, C++17 and C++20 probes through the real headers: wchar_t size,
# signedness, maximum and (C) _Generic identity, wint_t type identity (_Generic / std::is_same), wide-literal size,
# plain-char signedness and the __CHAR_SIGNED__/__CHAR_UNSIGNED__ macros - by default and under both overrides.  Skips without PS3DEV or on GCC < 16.
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

# probe_c <abi> <flags> <char-signed 0|1> <label>: C11 type identities and char macros
probe_c() {
    local abi=$1 flags=$2 sg=$3 label=$4 src=$work/t.c
    cat > "$src" <<EOF || { fail "$abi $label: writing the C probe"; return; }
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
_Static_assert(sizeof(wchar_t) == 2, "wchar_t is 16-bit");
_Static_assert(WCHAR_MAX == 65535 && __WCHAR_MAX__ == 65535, "WCHAR_MAX is 65535");
_Static_assert((wchar_t)-1 > 0, "wchar_t is unsigned");
_Static_assert(_Generic((wchar_t)0, unsigned short: 1, default: 0), "wchar_t is unsigned short");
_Static_assert(_Generic((wint_t)0, int: 1, default: 0), "wint_t is int");
_Static_assert(sizeof(L"ab") == 6, "L\"\" literals are 16-bit units");
_Static_assert(((char)-1 < 0) == $sg, "plain char signedness");
#if $sg
# if !defined(__CHAR_SIGNED__) || defined(__CHAR_UNSIGNED__)
#  error "signed plain char: __CHAR_SIGNED__ only"
# endif
#else
# if defined(__CHAR_SIGNED__) || !defined(__CHAR_UNSIGNED__)
#  error "unsigned plain char: __CHAR_UNSIGNED__ only"
# endif
#endif
int x;
EOF
    "$G" $flags -std=c11 -c "$src" -o "$work/t.o" > "$work/t.log" 2>&1 \
        || fail "$abi $label C11: $(grep -m2 -i 'error' "$work/t.log")"
}
# probe_cxx <abi> <flags> <char-signed 0|1> <label> <std>: the C++ built-in wchar_t, wint_t identity, char
probe_cxx() {
    local abi=$1 flags=$2 sg=$3 label=$4 std=$5 src=$work/t.cpp
    cat > "$src" <<EOF || { fail "$abi $label $std: writing the C++ probe"; return; }
#include <climits>
#include <cwchar>
#include <limits>
#include <type_traits>
static_assert(sizeof(wchar_t) == 2, "wchar_t is 16-bit");
static_assert(!std::numeric_limits<wchar_t>::is_signed, "wchar_t is unsigned");
static_assert(std::numeric_limits<wchar_t>::max() == 65535 && WCHAR_MAX == 65535, "wchar_t max is 65535");
static_assert(std::is_same<std::wint_t, int>::value, "wint_t is int");
static_assert(sizeof(L"ab") == 6, "L\"\" literals are 16-bit units");
static_assert(std::numeric_limits<char>::is_signed == bool($sg), "plain char signedness");
static_assert(std::is_signed<char>::value == bool($sg), "plain char signedness (traits)");
#if $sg
# if !defined(__CHAR_SIGNED__) || defined(__CHAR_UNSIGNED__)
#  error "signed plain char: __CHAR_SIGNED__ only"
# endif
#else
# if defined(__CHAR_SIGNED__) || !defined(__CHAR_UNSIGNED__)
#  error "unsigned plain char: __CHAR_UNSIGNED__ only"
# endif
#endif
int x;
EOF
    "${G%gcc}g++" $flags -std=$std -c "$src" -o "$work/t.o" > "$work/t.log" 2>&1 \
        || fail "$abi $label $std: $(grep -m2 -i 'error' "$work/t.log")"
}

for abi in ilp32 lp64; do
    flag=; [ $abi = lp64 ] && flag=-mlp64
    if [ $abi = ilp32 ]; then want_char='#define __CHAR_SIGNED__ 1'; not_char=__CHAR_UNSIGNED__; signed=1
    else want_char='#define __CHAR_UNSIGNED__ 1'; not_char=__CHAR_SIGNED__; signed=0; fi
    for lang in c c++; do
        dm=$("$G" $flag -x $lang -dM -E "$work/empty.c") || { fail "$abi $lang: -dM -E exited non-zero"; continue; }
        # __WCHAR_MAX__ is judged by value below (GCC 16 spells it 0xffff, the reference 65535U)
        for m in "$want_char" '#define __WCHAR_TYPE__ short unsigned int' '#define __WINT_TYPE__ int'; do
            has "$m" "$dm"; case $? in 0) ;; 1) fail "$abi $lang: missing '$m'" ;; *) fail "$abi $lang: could not judge '$m'" ;; esac
        done
        grep -q -- "^#define $not_char " <<<"$dm"; case $? in 1) ;; 0) fail "$abi $lang: $not_char is predefined" ;; *) fail "$abi $lang: could not judge $not_char" ;; esac
    done
    # the types themselves, through the real headers: C11, C++17 and C++20, the default and both overrides
    for opt in default -fsigned-char -funsigned-char; do
        case $opt in default) o=; sg=$signed ;; -fsigned-char) o=$opt; sg=1 ;; *) o=$opt; sg=0 ;; esac
        probe_c "$abi" "$flag $o" "$sg" "$opt"
        for std in c++17 c++20; do probe_cxx "$abi" "$flag $o" "$sg" "$opt" "$std"; done
    done
    [ $status -eq 0 ] && echo "ppu-abi-char-wchar: ok $abi (predefines in C and C++; C11, C++17 and C++20 type identities through the real headers, default and both -f[un]signed-char overrides)"
done
[ $status -eq 0 ] && echo "PASS: ppu-abi-char-wchar-test"
exit $status
