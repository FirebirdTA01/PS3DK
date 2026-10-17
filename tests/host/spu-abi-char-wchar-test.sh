#!/usr/bin/env bash
# The SPU compiler's plain char, wchar_t and wint_t match the reference SPU toolchain (spu gcc-9.5.0 0016).
# Measured with the reference spu-lv2-gcc -dM -E (C and C++): __CHAR_SIGNED__ 1 (no __CHAR_UNSIGNED__),
# __WCHAR_TYPE__ short unsigned int, __WCHAR_MAX__ 65535U (judged by value), __WINT_TYPE__ int.
# Checks the predefines AND compiles C11 and C++17 probes through the real headers (wchar_t size, signedness,
# maximum and identity, wint_t identity, wide-literal size, plain char and its macros), default and both overrides.  Skips without an SPU
# compiler under PS3DEV.
set -u -o pipefail
G=${PS3DEV:-}/spu/bin/spu-elf-gcc
if [ -z "${PS3DEV:-}" ] || [ ! -x "$G" ]; then echo "SKIP: spu-abi-char-wchar-test (no PS3DEV SPU compiler)"; exit 0; fi
work=$(mktemp -d) || { echo "spu-abi-char-wchar: FAIL mktemp"; exit 1; }
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "spu-abi-char-wchar: FAIL $*"; status=1; }
: > "$work/empty.c" || { echo "spu-abi-char-wchar: FAIL writing the probe"; exit 1; }
has() { grep -qxF -- "$1" <<<"$2"; local rc=$?; [ $rc -le 1 ] && return $rc; return 2; }

for lang in c c++; do
    dm=$("$G" -x $lang -dM -E "$work/empty.c") || { fail "$lang: -dM -E exited non-zero"; continue; }
    for m in '#define __CHAR_SIGNED__ 1' '#define __WCHAR_TYPE__ short unsigned int' '#define __WINT_TYPE__ int'; do
        has "$m" "$dm"; case $? in 0) ;; 1) fail "$lang: missing '$m'" ;; *) fail "$lang: could not judge '$m'" ;; esac
    done
    grep -q -- '^#define __CHAR_UNSIGNED__ ' <<<"$dm"; case $? in 1) ;; 0) fail "$lang: __CHAR_UNSIGNED__ is predefined" ;; *) fail "$lang: could not judge __CHAR_UNSIGNED__" ;; esac
done
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

# the types themselves, through the real headers: C11 and C++17, the default (signed) and both overrides
for opt in default -fsigned-char -funsigned-char; do
    case $opt in default) o=; sg=1 ;; -fsigned-char) o=$opt; sg=1 ;; *) o=$opt; sg=0 ;; esac
    probe_c spu "$o" "$sg" "$opt"
    probe_cxx spu "$o" "$sg" "$opt" c++17
done
[ $status -eq 0 ] && echo "PASS: spu-abi-char-wchar-test"
exit $status
