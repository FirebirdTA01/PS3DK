#!/usr/bin/env bash
# gcc-16.x 0078: C++ defaults to GNU C++17 on powerpc64-ps3-elf (GCC 16's own default is GNU C++20); every explicit
# dialect stays authoritative.  Under ILP32 and LP64 (-mlp64), with both g++ and gcc -x c++:
#
#  0. self-test: a failing compile is reported as an error, never as a value; the probe tells GNU from strict mode
#     and C++17 from C++20 (an explicit -std=gnu++20 / -std=c++17 reads back as such);
#  1. no -std option: __cplusplus 201703L in GNU mode (no __STRICT_ANSI__), by preprocessing (-E -dM) and by
#     compiling (static_assert in the source); cc1plus is given -std=gnu++17 (-###);
#  2. explicit -std=c++NN / gnu++NN for 98, 11, 14, 17, 20 and 23 give that standard and mode; -ansi is strict
#     C++98; with repeated -std options the last one wins; a -std option in a response file (@file) is honoured;
#  3. preprocessed input (.ii, and -x c++-cpp-output) is compiled as GNU C++17 with no option (char8_t is not a
#     keyword) and as C++20 with -std=c++20 (it is);
#  4. C is untouched: gcc on a .c file gets no C++ dialect (no -std=gnu++17 on cc1's line, no warning under
#     -Werror) and keeps GCC 16's C default (__STDC_VERSION__ 202311L).
#
#  5. self-test of the status handling: the same checks run against a stand-in driver that runs the real compiler,
#     prints its real output and then exits 1 for the C -E -dM query; that run must fail (rc 1) on exactly the two
#     C-default rows.  Every compiler step's status is checked before its output is read, and a macro must be
#     defined exactly once.
#
# Scope: GCC 16 with gcc-16.x 0078 only.  It asserts 0078's cc1plus option (-###) and GCC 16's C default; other
# compilers (GCC 12 included) are expected to fail it.
#
# usage: cxx-default-dialect-test.sh [--ppu-gcc <powerpc64-ps3-elf-gcc>]
#   With no argument: $PS3DEV's compiler; it skips without PS3DEV.  A named compiler never skips.
#   Exit 0 pass (or skip), 1 fail, 2 usage or missing input.
set -u
gcc=""
while [ $# -gt 0 ]; do
    case $1 in
        --ppu-gcc) gcc=$2; shift 2 ;;
        *) echo "usage: $0 [--ppu-gcc <gcc>]"; exit 2 ;;
    esac
done
if [ -z "$gcc" ]; then
    gcc="${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-gcc"
    if [ -z "${PS3DEV:-}" ] || [ ! -x "$gcc" ]; then
        echo "SKIP: cxx-default-dialect-test (no PS3DEV compiler)"; exit 0
    fi
fi
[ -x "$gcc" ] || { echo "cxx-default-dialect: no compiler at '$gcc'"; exit 2; }
gxx="${gcc%gcc}g++"
[ -x "$gxx" ] || { echo "cxx-default-dialect: no g++ at '$gxx'"; exit 2; }

status=0
ok()   { echo "cxx-default-dialect: ok   $*"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
fail() { echo "cxx-default-dialect: FAIL $*"; echo "cxx-default-dialect: FAIL $*" >> "$work/fails.txt"; status=1; }
: > "$work/fails.txt"
export PS3DK_NO_DEFAULT_INCLUDE=1
: > "$work/empty.cc"

# probe <driver...> -- <flags...>: sets $got to "<__cplusplus> gnu|strict" from -E -dM; returns 1 (got empty) if the
# preprocessor fails or does not define __cplusplus exactly once.
probe() {
    local drv=() a
    while [ "$1" != "--" ]; do drv+=("$1"); shift; done; shift
    got=""
    "${drv[@]}" "$@" -E -dM "$work/empty.cc" > "$work/dm.txt" 2> "$work/dm.err" || return 1
    [ "$(grep -c '^#define __cplusplus ' "$work/dm.txt")" = 1 ] || return 1
    a=$(awk '$2 == "__cplusplus" {print $3}' "$work/dm.txt")
    [ -n "$a" ] || return 1
    if grep -q '^#define __STRICT_ANSI__ ' "$work/dm.txt"; then got="$a strict"; else got="$a gnu"; fi
}
# expect <label> <want> <driver...> -- <flags...>
expect() {
    local label=$1 want=$2; shift 2
    if ! probe "$@"; then fail "$label: preprocessing failed or no __cplusplus: $(head -2 "$work/dm.err" | tr '\n' ' ')"; return; fi
    [ "$got" = "$want" ] && ok "$label: $got" || fail "$label: got '$got', want '$want'"
}

# 0: self-test.
probe "$gxx" -- -std=no-such-dialect && fail "self-test: an invalid -std read back as '$got'" \
    || ok "self-test: a failing preprocess is an error, not a value"
for abi in ilp32 lp64; do
    af=(); [ $abi = lp64 ] && af=(-mlp64)
    expect "$abi self-test: explicit -std=gnu++20 is visible" "202002L gnu" "$gxx" -- "${af[@]}" -std=gnu++20
    expect "$abi self-test: explicit -std=c++17 is visible as strict" "201703L strict" "$gxx" -- "${af[@]}" -std=c++17
done

for abi in ilp32 lp64; do
    af=(); [ $abi = lp64 ] && af=(-mlp64)
    for d in g++ "gcc -x c++"; do
        if [ "$d" = g++ ]; then drv=("$gxx"); else drv=("$gcc" -x c++); fi
        # 1
        expect "$abi $d no option" "201703L gnu" "${drv[@]}" -- "${af[@]}"
        # 2
        for v in 98:199711L 11:201103L 14:201402L 17:201703L 20:202002L 23:202302L; do
            n=${v%%:*}; c=${v#*:}
            expect "$abi $d -std=c++$n" "$c strict" "${drv[@]}" -- "${af[@]}" -std=c++$n
            expect "$abi $d -std=gnu++$n" "$c gnu" "${drv[@]}" -- "${af[@]}" -std=gnu++$n
        done
        expect "$abi $d -ansi" "199711L strict" "${drv[@]}" -- "${af[@]}" -ansi
        expect "$abi $d -std=c++20 -std=gnu++14 (last wins)" "201402L gnu" "${drv[@]}" -- "${af[@]}" -std=c++20 -std=gnu++14
        expect "$abi $d -std=gnu++17 -std=c++23 (last wins)" "202302L strict" "${drv[@]}" -- "${af[@]}" -std=gnu++17 -std=c++23
        printf -- '-std=c++20\n' > "$work/std.rsp"
        expect "$abi $d @response-file -std=c++20" "202002L strict" "${drv[@]}" -- "${af[@]}" @"$work/std.rsp"
    done

    # 1 by compiling: the dialect the compiler proper runs in, not only the preprocessor's macro.
    printf '#if !defined(__STRICT_ANSI__)\nstatic_assert(__cplusplus == 201703L, "gnu++17");\n#else\n#error strict\n#endif\nint f() { return 0; }\n' > "$work/d17.cc"
    "$gxx" "${af[@]}" -c "$work/d17.cc" -o "$work/d17.o" > "$work/c.log" 2>&1 && ok "$abi g++ compile: static_assert gnu++17 holds" \
        || { fail "$abi g++ compile: static_assert gnu++17 failed"; head -3 "$work/c.log" | sed 's/^/    /'; }
    # Controls fail for the asserted reason only: the compiler's status is 1 and its diagnostic names it.
    "$gxx" "${af[@]}" -std=gnu++20 -c "$work/d17.cc" -o "$work/d20.o" > "$work/c.log" 2>&1; rc=$?
    [ $rc -eq 1 ] && grep -q 'static assertion failed' "$work/c.log" && ok "$abi control: the gnu++17 static_assert fails under -std=gnu++20" \
        || { fail "$abi control: -std=gnu++20 compile gave status $rc without the static_assert diagnostic"; head -3 "$work/c.log" | sed 's/^/    /'; }
    # cc1plus's own command line (-###): -std=gnu++17 with no option, none added beside an explicit one.
    "$gxx" "${af[@]}" -### -c "$work/d17.cc" > "$work/v.txt" 2>&1 || fail "$abi -### failed"
    grep 'cc1plus' "$work/v.txt" | grep -q '"-std=gnu++17"' && ok "$abi -###: cc1plus gets -std=gnu++17" || fail "$abi -###: no -std=gnu++17 on cc1plus's line"
    "$gxx" "${af[@]}" -std=c++20 -### -c "$work/d17.cc" > "$work/v.txt" 2>&1 || fail "$abi -### -std=c++20 failed"
    l=$(grep 'cc1plus' "$work/v.txt")
    printf '%s' "$l" | grep -q '"-std=c++20"' && ! printf '%s' "$l" | grep -q 'gnu++17' && ok "$abi -### -std=c++20: only the explicit dialect" \
        || fail "$abi -### -std=c++20: cc1plus line is: $l"

    # 3: preprocessed input.  char8_t is a keyword from C++20: as an identifier it compiles under gnu++17 only.
    printf 'int char8_t = 1;\nint g() { return char8_t; }\n' > "$work/pp.ii"
    "$gxx" "${af[@]}" -c "$work/pp.ii" -o "$work/pp.o" > "$work/c.log" 2>&1 && ok "$abi .ii no option: compiled as GNU C++17 (char8_t not a keyword)" \
        || { fail "$abi .ii no option: did not compile as C++17"; head -3 "$work/c.log" | sed 's/^/    /'; }
    "$gcc" "${af[@]}" -x c++-cpp-output -c "$work/pp.ii" -o "$work/pp2.o" > "$work/c.log" 2>&1 && ok "$abi gcc -x c++-cpp-output no option: GNU C++17" \
        || { fail "$abi gcc -x c++-cpp-output no option: did not compile as C++17"; head -3 "$work/c.log" | sed 's/^/    /'; }
    "$gxx" "${af[@]}" -std=c++20 -c "$work/pp.ii" -o "$work/pp3.o" > "$work/c.log" 2>&1; rc=$?
    [ $rc -eq 1 ] && grep -q 'char8_t' "$work/c.log" && ok "$abi .ii control: -std=c++20 rejects char8_t as an identifier" \
        || { fail "$abi .ii control: -std=c++20 compile gave status $rc without a char8_t diagnostic"; head -3 "$work/c.log" | sed 's/^/    /'; }

    # 4: C.
    printf 'int c(void) { return 0; }\n' > "$work/c.c"
    "$gcc" "${af[@]}" -Werror -c "$work/c.c" -o "$work/c.o" > "$work/c.log" 2>&1 && ok "$abi C compile clean under -Werror" \
        || { fail "$abi C compile"; head -3 "$work/c.log" | sed 's/^/    /'; }
    "$gcc" "${af[@]}" -### -c "$work/c.c" > "$work/v.txt" 2>&1 && ! grep 'cc1"* ' "$work/v.txt" | grep -q 'std=' && grep -q '/cc1"* ' "$work/v.txt" \
        && ok "$abi C: cc1 gets no -std" || fail "$abi C: cc1's line has a -std option (or no cc1 line): $(grep 'cc1' "$work/v.txt")"
    # The status is checked before the output is read, and __STDC_VERSION__ must be defined exactly once.
    if "$gcc" "${af[@]}" -E -dM -x c /dev/null > "$work/cdm.txt" 2> "$work/cdm.err"; then
        n=$(grep -c '^#define __STDC_VERSION__ ' "$work/cdm.txt")
        sv=$(awk '$2 == "__STDC_VERSION__" {print $3}' "$work/cdm.txt")
        [ "$n" = 1 ] && [ "$sv" = "202311L" ] && ok "$abi C default __STDC_VERSION__ $sv" \
            || fail "$abi C default __STDC_VERSION__: $n definition(s), value '$sv', want one 202311L"
    else
        fail "$abi C default __STDC_VERSION__: the C preprocessor failed (status $?)"; head -3 "$work/cdm.err" | sed 's/^/    /'
    fi
done

# 5: status handling, against a stand-in that runs the real driver, prints its real output, then exits 1 for the C
# -E -dM query only.  The nested run (no further self-test) must fail with status 1, report the two C-default rows
# as preprocessor failures, and otherwise fail exactly the rows this run failed (none, on a correct compiler).
if [ -z "${CXXDD_NESTED:-}" ]; then
    grep -v 'C default __STDC_VERSION__' "$work/fails.txt" | sed 's/[[:space:]]*$//' | sort > "$work/parent-fails.txt"
    mkdir -p "$work/exit1"
    for t in gcc g++; do
        real="${gcc%gcc}$t"
        printf '#!/bin/bash\n"%s" "$@"; rc=$?\ncase " $* " in *" -E -dM -x c /dev/null "*) echo "stand-in: exit 1 after real output" >&2; exit 1 ;; esac\nexit $rc\n' \
            "$real" > "$work/exit1/powerpc64-ps3-elf-$t"
        chmod +x "$work/exit1/powerpc64-ps3-elf-$t"
    done
    out=$(CXXDD_NESTED=1 bash "${BASH_SOURCE[0]}" --ppu-gcc "$work/exit1/powerpc64-ps3-elf-gcc" 2>&1); rc=$?
    nc=$(printf '%s\n' "$out" | grep -c ': FAIL .*C default __STDC_VERSION__: the C preprocessor failed')
    # The stand-in's own path differs from the real driver's only in diagnostics; rows are compared by their text.
    printf '%s\n' "$out" | grep ': FAIL ' | grep -v 'C default __STDC_VERSION__' | sed 's/[[:space:]]*$//' | sort > "$work/nested-fails.txt"
    if [ $rc -eq 1 ] && [ "$nc" = 2 ] && cmp -s "$work/parent-fails.txt" "$work/nested-fails.txt"; then
        ok "self-test: real output then exit 1 fails the two C-default rows and nothing else new (rc 1)"
    else
        fail "self-test: stand-in exiting 1 after real output gave rc $rc, $nc C-default preprocessor failure(s); other differences:"
        diff "$work/parent-fails.txt" "$work/nested-fails.txt" | head -5 | sed 's/^/    /'
    fi
fi

[ $status -eq 0 ] && echo "PASS: cxx-default-dialect-test" || echo "FAIL: cxx-default-dialect-test"
exit $status
