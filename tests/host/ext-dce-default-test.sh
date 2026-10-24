#!/usr/bin/env bash
# gcc-16.x 0080: ext-dce runs by default with correct pointer facts under ILP32; LP64 is untouched.
#
#  0. self-test: the checks below report a failing compiler, a failing objdump and a missing function as errors,
#     never as a count of 0; with no argument, a compiler whose optimizer query fails is a failure, and only one that
#     answers without an ext-dce option (GCC 12) is skipped;
#  1. effective default (-Q --help=optimizers) at -O2, -O3 and -Os: both ABIs [enabled];
#  2. an explicit -fext-dce is honoured under ILP32 ([enabled]) and -fno-ext-dce under LP64 ([disabled]);
#  3. code: the ilp32-ext-dce row's walk.c keeps the zero-extension of its buffer pointer (an rldicl ...,0,32 that
#     ext-dce would drop) under ILP32 at -O2 by default; also when the function carries
#     __attribute__((optimize("O2"))), inside #pragma GCC push_options / optimize ("O2"), and through -flto
#     (compile and link-time optimisation). Explicit -fext-dce must retain the same protection.
#     #pragma GCC optimize ("no-ext-dce") inside push_options matches explicit -fno-ext-dce;
#     a function after pop_options matches the enabled default again.
#     Instruction counts are a focused codegen check, not runtime correctness evidence; the
#     ilp32-ext-dce regression must also execute against its independent checksum reference.
#
# Every compiler, objdump and query step is status-checked; a count is taken only from a function that was found,
# and between that function's start and its end.
#
# usage: ext-dce-default-test.sh [--ppu-gcc <powerpc64-ps3-elf-gcc>]
#   With no argument: $PS3DEV's compiler; it skips without PS3DEV or on GCC 12 (no ext-dce pass).  A named
#   compiler never skips.  Exit 0 pass (or skip), 1 fail, 2 usage or missing input.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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
        echo "SKIP: ext-dce-default-test (no PS3DEV compiler)"; exit 0
    fi
    # Only a compiler that answers the query, without an ext-dce option, is skipped; a failing query fails.
    if ! q=$(PS3DK_NO_DEFAULT_INCLUDE=1 "$gcc" -Q --help=optimizers 2>&1); then
        echo "ext-dce-default: FAIL optimizer query failed: $gcc -Q --help=optimizers"
        printf '%s\n' "$q" | head -5 | sed 's/^/    /'
        echo "FAIL: ext-dce-default-test"; exit 1
    fi
    if ! printf '%s\n' "$q" | grep -q -- '-fext-dce'; then
        echo "SKIP: ext-dce-default-test ($gcc has no ext-dce pass)"; exit 0
    fi
fi
[ -x "$gcc" ] || { echo "ext-dce-default: no compiler at '$gcc'"; exit 2; }
od="${gcc%gcc}objdump"
[ -x "$od" ] || { echo "ext-dce-default: no objdump at '$od'"; exit 2; }
# The LTO check links an executable, which needs the SDK: $PS3DK, else the compiler's own stage (<prefix>/ps3dk).
if [ -z "${PS3DK:-}" ]; then
    sdk="$(cd "$(dirname "$gcc")/../.." && pwd)/ps3dk"
    [ -d "$sdk" ] && export PS3DK="$sdk"
fi
walk="$root/tests/regression/ilp32-ext-dce/walk.c"
[ -f "$walk" ] || { echo "ext-dce-default: missing $walk"; exit 2; }

status=0
ok()   { echo "ext-dce-default: ok   $*"; }
fail() { echo "ext-dce-default: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
export PS3DK_NO_DEFAULT_INCLUDE=1

# cc <label> <args...>: run the compiler; on failure report it (with its first lines of output) and return 1.
cc() {
    local label=$1; shift
    if "$gcc" "$@" > "$work/cc.log" 2>&1; then return 0; fi
    fail "$label: compiler failed: $gcc $*"; head -5 "$work/cc.log" | sed 's/^/    /'; return 1
}

# state <flags...>: sets $got to [enabled] or [disabled]; returns 1 if the query fails or has no -fext-dce line.
state() {
    got=""
    "$gcc" "$@" -Q --help=optimizers > "$work/q.txt" 2>&1 || return 1
    got=$(awk '$1 == "-fext-dce" {print $2}' "$work/q.txt")
    [ "$got" = "[enabled]" ] || [ "$got" = "[disabled]" ]
}
expect_state() { # <label> <want> <flags...>
    local label=$1 want=$2; shift 2
    if ! state "$@"; then fail "$label: optimizer query failed or has no -fext-dce line (got '$got')"; return; fi
    [ "$got" = "$want" ] && ok "$label: $got" || fail "$label: got '$got', want $want"
}

# Count clrldi rX,rY,32 (rldicl ...,0,32) in one function's machine code.  The target is ELFv1 with function
# sections: "walk_sum" names the descriptor in .opd and objdump shows the code as "<.text.walk_sum>:"; a "<walk_sum>:"
# or "<.walk_sum>:" label is accepted too.  Exactly one of them must be present.
# zext_count <object> <function>: sets $n; returns 1 (n empty) if objdump fails or the function is not in it once.
zext_count() {
    n=""
    "$od" -d "$1" > "$work/od.txt" 2>"$work/od.err" || return 1
    awk -v fn="$2" '$NF == ("<" fn ">:") || $NF == ("<." fn ">:") || $NF == ("<.text." fn ">:") {f=1; found++; next}
                    f && (/^$/ || /^Disassembly of section/) {f=0} f {print} END {exit found != 1}' \
        "$work/od.txt" > "$work/fn.txt" || return 1
    n=$(grep -cE 'clrldi[[:space:]]+r?[0-9]+,r?[0-9]+,32' "$work/fn.txt")
    return 0
}
# The same over assembly: from "<function>:" (its descriptor; the code follows at ".L.<function>:") to its
# ".size <function>," directive.
# zext_count_s <function> <asm files...>: sets $n; returns 1 if no file has the function or it never ends.
zext_count_s() {
    local fn=$1; shift
    n=""
    awk -v fn="$fn" '$0 == (fn ":") {f=1; found++; next}
                     f && $1 == ".size" && index($2, fn ",") == 1 {f=0; ended++}
                     f {print}
                     END {exit !(found == 1 && ended == 1)}' "$@" > "$work/fn.s" 2>/dev/null || return 1
    n=$(grep -cE 'rldicl[[:space:]]+[0-9]+,[0-9]+,0,32' "$work/fn.s")
    return 0
}
# count <label> <object> <function>: sets $n or fails the label and returns 1.
count() {
    zext_count "$2" "$3" && return 0
    fail "$1: no '$3' in $2's disassembly (objdump failed or the function is missing)"; return 1
}

# 0: self-test of the instruments, so a broken tool is never read as "0 zero-extensions".
printf 'int other(int x) { return x + 1; }\n' > "$work/other.c"
if "$gcc" -O2 -fno-such-flag-ext-dce-selftest -c "$walk" -o "$work/bad.o" > /dev/null 2>&1; then
    fail "self-test: an invalid flag compiled (the compiler status cannot be trusted)"
else ok "self-test: a failing compile is reported by its status"; fi
if cc "self-test" -O2 -c "$work/other.c" -o "$work/other.o"; then
    zext_count "$work/other.o" walk_sum && fail "self-test: a missing function counted as $n" \
        || ok "self-test: a missing function is an error, not a count"
    zext_count "$work/missing.o" walk_sum && fail "self-test: a missing object counted as $n" \
        || ok "self-test: a failing objdump is an error, not a count"
    printf 'walk_sum:\n\trldicl 9,9,0,32\n' > "$work/open.s"
    zext_count_s walk_sum "$work/open.s" && fail "self-test: an unterminated function counted as $n" \
        || ok "self-test: an assembly function without its .size end is an error"
fi
# The no-argument ($PS3DEV) path: a compiler whose query fails is a failure, not a skip; one that answers without an
# ext-dce option (GCC 12) is the only skip.  Each runs this script against a stand-in compiler.
mkdir -p "$work/broken/ppu/bin" "$work/old/ppu/bin"
printf '#!/bin/sh\necho "stand-in: cannot run" >&2\nexit 1\n' > "$work/broken/ppu/bin/powerpc64-ps3-elf-gcc"
printf '#!/bin/sh\necho "  -fgcse  [enabled]"\nexit 0\n' > "$work/old/ppu/bin/powerpc64-ps3-elf-gcc"
chmod +x "$work/broken/ppu/bin/powerpc64-ps3-elf-gcc" "$work/old/ppu/bin/powerpc64-ps3-elf-gcc"
out=$(PS3DEV="$work/broken" bash "${BASH_SOURCE[0]}" 2>&1); rc=$?
[ $rc -eq 1 ] && ! printf '%s\n' "$out" | grep -q '^SKIP' && ok "self-test: no-argument run with a failing query fails (rc 1)" \
    || fail "self-test: no-argument run with a failing query gave rc $rc: $(printf '%s' "$out" | tail -n 1)"
out=$(PS3DEV="$work/old" bash "${BASH_SOURCE[0]}" 2>&1); rc=$?
[ $rc -eq 0 ] && printf '%s\n' "$out" | grep -q '^SKIP' && ok "self-test: no-argument run with a compiler without ext-dce skips (rc 0)" \
    || fail "self-test: no-argument run with a compiler without ext-dce gave rc $rc: $(printf '%s' "$out" | tail -n 1)"
state -O2 -fno-such-flag-ext-dce-selftest && fail "self-test: a failing optimizer query gave '$got'" \
    || ok "self-test: a failing optimizer query is an error"

# 1 + 2
for o in -O2 -O3 -Os; do
    expect_state "ILP32 $o default" "[enabled]" $o
    expect_state "LP64 $o default" "[enabled]" -mlp64 $o
done
expect_state "ILP32 -O2 explicit -fext-dce" "[enabled]" -O2 -fext-dce
expect_state "ILP32 -O2 explicit -fno-ext-dce" "[disabled]" -O2 -fno-ext-dce
expect_state "LP64 -O2 explicit -fno-ext-dce" "[disabled]" -mlp64 -O2 -fno-ext-dce

# Arithmetic-derived pointer: copy-only fact invalidation is insufficient.
# This RTL reduction must retain one zero-extension on both ABIs, whether
# ext-dce is defaulted, explicitly enabled or explicitly disabled.
derived="$root/tests/regression/ilp32-ext-dce/derived-pointer-rtl.c"
for abi in ilp32 lp64; do
    abi_flags=(); [ "$abi" = ilp32 ] || abi_flags=(-mlp64)
    for opt in -O2 -O3 -Os; do
        for mode in default on off; do
            pass_flags=()
            case "$mode" in on) pass_flags=(-fext-dce);; off) pass_flags=(-fno-ext-dce);; esac
            label="derived pointer $abi $opt $mode"
            if cc "$label" "${abi_flags[@]}" "$opt" "${pass_flags[@]}" -S "$derived" -o "$work/derived.s"; then
                if ! zext_count_s derived_pointer "$work/derived.s"; then
                    fail "$label: no single complete function in assembly"
                else
                    [ "$n" -eq 1 ] && ok "$label keeps the zero-extension" || fail "$label has $n zero-extensions, want 1"
                fi
            fi
        done
    done
done

# 3: a necessary pointer zero-extension survives with the pass enabled.
d=""; f=""; off=""
cc "ILP32 -O2" -O2 -c "$walk" -o "$work/default.o" && count "ILP32 -O2" "$work/default.o" walk_sum && d=$n
cc "ILP32 -O2 -fext-dce" -O2 -fext-dce -c "$walk" -o "$work/forced.o" && count "ILP32 -O2 -fext-dce" "$work/forced.o" walk_sum && f=$n
cc "ILP32 -O2 -fno-ext-dce" -O2 -fno-ext-dce -c "$walk" -o "$work/off.o" && count "ILP32 -O2 -fno-ext-dce" "$work/off.o" walk_sum && off=$n
if [ -z "$d" ] || [ -z "$f" ] || [ -z "$off" ]; then
    fail "ILP32 -O2: no baseline counts; the remaining code checks are not run"
    echo "FAIL: ext-dce-default-test"; exit 1
fi
if [ "$d" -gt 0 ] && [ "$d" -eq "$f" ] && [ "$off" -gt 0 ]; then
    ok "ILP32 -O2: default and explicit -fext-dce keep $d zero-extensions (pass off: $off)"
else
    fail "ILP32 -O2: default $d, explicit on $f, off $off; enabled forms must agree and all must retain a zero-extension"
fi

sed 's/^uint32_t walk_sum(/__attribute__((optimize("O2"))) uint32_t walk_sum(/' "$walk" > "$work/attr.c"
grep -q '^__attribute__((optimize("O2"))) uint32_t walk_sum(' "$work/attr.c" || fail "attribute: walk.c's walk_sum definition line changed; the attribute was not applied"
cc "optimize(\"O2\") attribute" -O0 -c "$work/attr.c" -o "$work/attr.o" && count "optimize(\"O2\") attribute" "$work/attr.o" walk_sum &&
    { [ "$n" -eq "$d" ] && ok "optimize(\"O2\") attribute keeps $n" || fail "optimize(\"O2\") attribute: $n zero-extensions, default -O2 has $d"; }

# Inside push_options / optimize ("O2") from -O0: the default applies.
{ echo '#pragma GCC push_options'; echo '#pragma GCC optimize ("O2")'; cat "$walk"; echo '#pragma GCC pop_options'; } > "$work/pragma.c"
cc "pragma optimize" -O0 -c "$work/pragma.c" -o "$work/pragma.o" && count "pragma optimize" "$work/pragma.o" walk_sum &&
    { [ "$n" -eq "$d" ] && ok "push_options/optimize (\"O2\") keeps $n" || fail "pragma optimize: $n zero-extensions, default -O2 has $d"; }

# Restoration from -O2: disable the pass inside push_options, then return to the enabled default.
{ echo '#pragma GCC push_options'; echo '#pragma GCC optimize ("no-ext-dce")'
  sed 's/^uint32_t walk_sum(/uint32_t walk_in(/' "$walk"
  echo '#pragma GCC pop_options'
  sed 's/^uint32_t walk_sum(/uint32_t walk_after(/' "$walk"; } > "$work/pop.c"
cc "pragma pop" -O2 -c "$work/pop.c" -o "$work/pop.o" && {
    count "pragma pop (inside)" "$work/pop.o" walk_in &&
        { [ "$n" -eq "$off" ] && ok "optimize (\"no-ext-dce\") inside push_options keeps $n (as -fno-ext-dce)" || fail "optimize (\"no-ext-dce\") inside push_options: $n zero-extensions, -fno-ext-dce has $off"; }
    count "pragma pop (after)" "$work/pop.o" walk_after &&
        { [ "$n" -eq "$d" ] && ok "after pop_options keeps $n (default restored)" || fail "after pop_options: $n zero-extensions, default -O2 has $d"; }
}

# LTO: compile with -flto and link an executable; lto1 optimises walk_sum at link time, and -save-temps keeps its
# assembly (<out>.ltrans0.ltrans.s).  -fno-inline keeps walk_sum a function of its own.  The count runs from
# "walk_sum:" to ".size walk_sum," only.
printf 'extern unsigned walk_sum(unsigned, const unsigned char *, unsigned);
static unsigned char b[64];
int main(void) { return (int)walk_sum(1, b, 33); }
' > "$work/main.c"
if [ -z "${PS3DK:-}" ]; then
    fail "LTO: no SDK to link against (PS3DK unset and no ps3dk beside the compiler)"
elif (cd "$work" && "$gcc" -O2 -flto -fno-inline -c "$walk" -o walk.o && "$gcc" -O0 -c main.c -o main.o &&
    "$gcc" -O2 -flto -fno-inline -save-temps walk.o main.o -o lto.elf) > "$work/lto.log" 2>&1; then
    set -- "$work"/lto.elf.ltrans*.s
    if [ -f "$1" ] && zext_count_s walk_sum "$@"; then
        [ "$n" -eq "$d" ] && ok "LTO keeps $n" || fail "LTO: $n zero-extensions, default -O2 has $d"
    else
        fail "LTO: no single complete walk_sum in the ltrans assembly"
    fi
else
    fail "LTO link failed"; head -5 "$work/lto.log" | sed 's/^/    /'
fi

[ $status -eq 0 ] && echo "PASS: ext-dce-default-test" || echo "FAIL: ext-dce-default-test"
exit $status
