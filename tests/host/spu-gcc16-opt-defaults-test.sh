#!/usr/bin/env bash
# The SPU GCC 16 port turns two passes off by default: loop-pattern
# distribution below -O3 (it turns copy and clear loops into memcpy/memset calls, and much SPU
# code links without a C library) and late-combine (it drops DImode-to-SImode
# truncations on SPU).  The defaults must hold for optimize attributes and
# pragmas too, which recompute the -O defaults; so they live in the target
# optimization table, not in spu_option_override, which those never rerun.
#
# Static mode (default, runs in CI): the series sets both passes in
# spu_option_optimization_table and no patch forces them in
# spu_option_override.
#
# Compiler mode (SPU16_GCC=<path to a GCC 16 spu-elf-gcc>): compile a copy
# loop and check for a memset call, and check which files run late-combine,
# at -O2, -Os, -O3, under optimize attributes and pragmas, and with explicit
# -f options (which must still win).
set -u
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
series="$repo_root/patches/spu/gcc-16.x"
fail=0
ok()  { printf 'spu-gcc16-opt-defaults: ok   %s\n' "$*"; }
bad() { printf 'spu-gcc16-opt-defaults: FAIL %s\n' "$*"; fail=1; }

# Lines the series adds, by file.
added() { # <file path in the gcc tree>
    awk -v f="$1" '/^diff --git /{on = ($3 == "a/" f)} on && /^\+[^+]/' "$series"/[0-9]*.patch | tr -d '\r'
}
common=$(added gcc/common/config/spu/spu-common.cc)
for opt in OPT_ftree_loop_distribute_patterns OPT_flate_combine_instructions; do
    grep -q "$opt" <<<"$common" && ok "spu-common.cc sets $opt in the optimization table" \
        || bad "spu-common.cc does not set $opt"
done
grep -q 'define TARGET_OPTION_OPTIMIZATION_TABLE spu_option_optimization_table' <<<"$common" \
    && ok "TARGET_OPTION_OPTIMIZATION_TABLE is the SPU table" || bad "TARGET_OPTION_OPTIMIZATION_TABLE not defined"
# Net effect on spu.cc: added minus removed assignments of the two flags.
net=$(awk '/^diff --git /{on = ($3 == "a/gcc/config/spu/spu.cc")}
           on && /^[+-][^+-].*flag_(late_combine_instructions|tree_loop_distribute_patterns) *=/ {n += (substr($0,1,1) == "+") ? 1 : -1}
           END {print n + 0}' "$series"/[0-9]*.patch)
[[ "$net" == 0 ]] && ok "spu.cc does not assign either flag" || bad "spu.cc still assigns the flags ($net net)"

if [[ -n "${SPU16_GCC:-}" ]]; then
    cc=$SPU16_GCC
    "$cc" -dumpversion | grep -q '^16\.' || bad "SPU16_GCC is not GCC 16: $("$cc" -dumpversion)"
    work=$(mktemp -d "${TMPDIR:-/tmp}/spu-opt-defaults.XXXXXX"); trap 'rm -rf "$work"' EXIT
    loop='(char *d, int n) { for (int i = 0; i < n; i++) d[i] = 0; }'
    printf 'void f%s\n' "$loop" > "$work/plain.c"
    printf '__attribute__((optimize("O2"))) void f%s\n' "$loop" > "$work/attr-O2.c"
    printf '__attribute__((optimize("Os"))) void f%s\n' "$loop" > "$work/attr-Os.c"
    printf '#pragma GCC optimize ("O2")\nvoid f%s\n' "$loop" > "$work/pragma-O2.c"
    printf '__attribute__((optimize("tree-loop-distribute-patterns"))) void f%s\n' "$loop" > "$work/attr-ldp.c"
    memset() { # <want yes|no> <file> <options...>
        local want=$1 f=$2; shift 2
        if ! "$cc" "$@" -S -o "$work/out.s" "$work/$f" 2>"$work/err"; then bad "$f $*: compile failed: $(head -1 "$work/err")"; return; fi
        local got=no; grep -q 'memset' "$work/out.s" && got=yes
        [[ "$got" == "$want" ]] && ok "$f $*: memset $got" || bad "$f $*: memset $got, want $want"
    }
    memset no  plain.c     -O2
    memset no  plain.c     -Os
    memset yes plain.c     -O3
    memset yes plain.c     -O2 -ftree-loop-distribute-patterns
    memset no  plain.c     -O3 -fno-tree-loop-distribute-patterns
    memset no  attr-O2.c   -O0
    memset no  attr-O2.c   -O3
    memset no  attr-Os.c   -O3
    memset no  pragma-O2.c -O3
    memset yes attr-ldp.c  -O2

    latecombine() { # <want yes|no> <file> <options...>
        local want=$1 f=$2; shift 2
        rm -rf "$work/d"; mkdir "$work/d"
        if ! "$cc" "$@" -fdump-rtl-late_combine1 -dumpdir "$work/d/" -c -o "$work/out.o" "$work/$f" 2>"$work/err"; then
            bad "$f $*: compile failed: $(head -1 "$work/err")"; return; fi
        # The dump of a pass that is gated off is empty or holds no function.
        local got=no; cat "$work"/d/*late_combine1* 2>/dev/null | grep -q '^;; Function' && got=yes
        [[ "$got" == "$want" ]] && ok "$f $*: late-combine $got" || bad "$f $*: late-combine $got, want $want"
    }
    latecombine no  plain.c     -O2
    latecombine no  plain.c     -O3
    latecombine yes plain.c     -O2 -flate-combine-instructions
    latecombine no  attr-O2.c   -O0
    latecombine no  pragma-O2.c -Os
fi

if [[ $fail -ne 0 ]]; then printf 'FAIL: spu-gcc16-opt-defaults-test\n'; exit 1; fi
printf 'PASS: spu-gcc16-opt-defaults-test%s\n' "${SPU16_GCC:+ (compiler mode)}"
