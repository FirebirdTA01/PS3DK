#!/usr/bin/env bash
# The SPU compiler's plain char, wchar_t and wint_t match the reference SPU toolchain (spu gcc-9.5.0 0016).
# Measured with the reference spu-lv2-gcc -dM -E (C and C++): __CHAR_SIGNED__ 1 (no __CHAR_UNSIGNED__),
# __WCHAR_TYPE__ short unsigned int, __WCHAR_MAX__ 65535U (judged by value), __WINT_TYPE__ int.
# Checks the predefines AND static_asserts on the types; -f[un]signed-char still override.  Skips without an SPU
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
cat > "$work/t.c" <<'EOF' || { echo "spu-abi-char-wchar: FAIL writing the type probe"; exit 1; }
#include <stddef.h>
#include <wchar.h>
_Static_assert(sizeof(wchar_t) == 2, "wchar_t is 16-bit");
_Static_assert(__WCHAR_MAX__ == 65535, "__WCHAR_MAX__ is 65535");
_Static_assert((wchar_t)-1 > 0, "wchar_t is unsigned");
_Static_assert(sizeof(wint_t) == sizeof(int) && (wint_t)-1 < 0, "wint_t is int");
_Static_assert(sizeof(L"ab") == 6, "L\"\" literals are 16-bit units");
_Static_assert((char)-1 < 0, "plain char is signed");
int x;
EOF
"$G" -std=c11 -c "$work/t.c" -o "$work/t.o" > "$work/t.log" 2>&1 || fail "type asserts: $(grep -m2 -i error "$work/t.log")"
for o in -fsigned-char:1 -funsigned-char:0; do
    opt=${o%%:*}; s=${o#*:}
    printf '_Static_assert(((char)-1 < 0) == %s, "override");\nint y;\n' "$s" > "$work/o.c" || { fail "writing the override probe"; continue; }
    "$G" $opt -std=c11 -c "$work/o.c" -o "$work/o.o" > "$work/o.log" 2>&1 || fail "$opt is not honoured"
done
[ $status -eq 0 ] && echo "PASS: spu-abi-char-wchar-test"
exit $status
