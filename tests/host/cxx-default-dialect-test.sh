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
# usage: cxx-default-dialect-test.sh [--ppu-gcc <powerpc64-ps3-elf-gcc>]
#   With no argument: $PS3DEV's compiler; it skips without PS3DEV.  A named compiler never skips.  The GCC 12
#   toolchain passes too (its own default is GNU C++17), except check 4's C default.
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
fail() { echo "cxx-default-dialect: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
export PS3DK_NO_DEFAULT_INCLUDE=1
: > "$work/empty.cc"

# probe <driver...> -- <flags...>: sets $got to "<__cplusplus> gnu|strict" from -E -dM; returns 1 (got empty) if the
# preprocessor fails or defines no __cplusplus.
probe() {
    local drv=() a
    while [ "$1" != "--" ]; do drv+=("$1"); shift; done; shift
    got=""
    "${drv[@]}" "$@" -E -dM "$work/empty.cc" > "$work/dm.txt" 2> "$work/dm.err" || return 1
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
    "$gxx" "${af[@]}" -std=gnu++20 -c "$work/d17.cc" -o "$work/d20.o" > /dev/null 2>&1 && fail "$abi control: the gnu++17 static_assert compiled under -std=gnu++20" \
        || ok "$abi control: the gnu++17 static_assert fails under -std=gnu++20"
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
    "$gxx" "${af[@]}" -std=c++20 -c "$work/pp.ii" -o "$work/pp3.o" > /dev/null 2>&1 && fail "$abi .ii control: -std=c++20 accepted char8_t as an identifier" \
        || ok "$abi .ii control: -std=c++20 rejects char8_t as an identifier"

    # 4: C.
    printf 'int c(void) { return 0; }\n' > "$work/c.c"
    "$gcc" "${af[@]}" -Werror -c "$work/c.c" -o "$work/c.o" > "$work/c.log" 2>&1 && ok "$abi C compile clean under -Werror" \
        || { fail "$abi C compile"; head -3 "$work/c.log" | sed 's/^/    /'; }
    "$gcc" "${af[@]}" -### -c "$work/c.c" > "$work/v.txt" 2>&1 && ! grep 'cc1"* ' "$work/v.txt" | grep -q 'std=' && grep -q '/cc1"* ' "$work/v.txt" \
        && ok "$abi C: cc1 gets no -std" || fail "$abi C: cc1's line has a -std option (or no cc1 line): $(grep 'cc1' "$work/v.txt")"
    sv=$("$gcc" "${af[@]}" -E -dM -x c /dev/null 2>/dev/null | awk '$2 == "__STDC_VERSION__" {print $3}')
    [ "$sv" = "202311L" ] && ok "$abi C default __STDC_VERSION__ $sv" || fail "$abi C default __STDC_VERSION__ '$sv', want 202311L"
done

[ $status -eq 0 ] && echo "PASS: cxx-default-dialect-test" || echo "FAIL: cxx-default-dialect-test"
exit $status
