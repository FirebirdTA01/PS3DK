#!/usr/bin/env bash
# The SPU GCC 16 port turns loop-pattern distribution off below -O3 (it turns
# copy and clear loops into memcpy/memset calls, and much SPU code links
# without a C library).  The default must hold for optimize attributes and
# pragmas too, which recompute the -O defaults; so it lives in the target
# optimization table, not in spu_option_override, which those never rerun.
# late-combine runs at its GCC 16 default: patch 0040 makes every SPU address
# mode dependent, which is what kept the pass from narrowing a reloaded MEM.
#
# Static mode (default, runs in CI): the series sets loop distribution in
# spu_option_optimization_table, leaves late-combine alone, defines the
# mode-dependent address hook, and no patch forces either pass in
# spu_option_override.
#
# Compiler mode (SPU16_GCC=<path to a GCC 16 spu-elf-gcc>): compile a copy
# loop and check for a memset call, and check which files run late-combine,
# at -O2, -Os, -O3, under optimize attributes and pragmas, and with explicit
# -f options (which must still win).
set -uo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
series="$repo_root/patches/spu/gcc-16.x"
fail=0
ok()  { printf 'spu-gcc16-opt-defaults: ok   %s\n' "$*"; }
bad() { printf 'spu-gcc16-opt-defaults: FAIL %s\n' "$*"; fail=1; }

# A failed search is not evidence that an option or dump is absent.
match() { # <yes|no> <pattern> <text> <label>
    local want=$1 pattern=$2 text=$3 label=$4 rc=0 got=no
    grep -q -- "$pattern" <<<"$text" || rc=$?
    case $rc in
        0) got=yes ;;
        1) ;;
        *) bad "$label: grep failed ($rc)"; return ;;
    esac
    [[ "$got" == "$want" ]] && ok "$label" || bad "$label: match $got, want $want"
}

# Lines the series adds, by file.
added() { # <file path in the gcc tree>
    awk -v f="$1" '/^diff --git /{on = ($3 == "a/" f)} on && /^\+[^+]/' "$series"/[0-9]*.patch | tr -d '\r'
}
common=$(added gcc/common/config/spu/spu-common.cc) || { bad "reading spu-common.cc patch lines failed"; exit 1; }
match yes 'OPT_ftree_loop_distribute_patterns' "$common" "spu-common.cc sets OPT_ftree_loop_distribute_patterns in the optimization table"
match no 'OPT_flate_combine_instructions' "$common" "spu-common.cc leaves late-combine at the GCC default"
match yes 'define TARGET_OPTION_OPTIMIZATION_TABLE spu_option_optimization_table' "$common" "TARGET_OPTION_OPTIMIZATION_TABLE is the SPU table"
spu=$(added gcc/config/spu/spu.cc) || { bad "reading spu.cc patch lines failed"; exit 1; }
match yes 'define TARGET_MODE_DEPENDENT_ADDRESS_P spu_mode_dependent_address_p' "$spu" "TARGET_MODE_DEPENDENT_ADDRESS_P is the SPU hook"
# The hook body: exactly "return true;" between its signature and the closing brace.
body=$(awk '{sub(/\r$/, "")} /^\+spu_mode_dependent_address_p \(/{on=1} on && /^\+  return true;$/ {n++} on && /^\+}/{exit} END {print n+0}' "$series"/[0-9]*.patch) \
    || { bad "reading hook body failed"; exit 1; }
[[ "$body" == 1 ]] && ok "spu_mode_dependent_address_p returns true" || bad "spu_mode_dependent_address_p does not return true"
# Net effect on spu.cc: added minus removed assignments of the two flags.
net=$(awk '/^diff --git /{on = ($3 == "a/gcc/config/spu/spu.cc")}
           on && /^[+-][^+-].*flag_(late_combine_instructions|tree_loop_distribute_patterns) *=/ {n += (substr($0,1,1) == "+") ? 1 : -1}
           END {print n + 0}' "$series"/[0-9]*.patch) || { bad "reading flag assignments failed"; exit 1; }
[[ "$net" == 0 ]] && ok "spu.cc does not assign either flag" || bad "spu.cc still assigns the flags ($net net)"

for definition in TARGET_MODE_DEPENDENT_ADDRESS_P TARGET_OPTION_OPTIMIZATION_TABLE; do
    net=$(awk -v name="$definition" '
        /^diff --git / {on = ($3 == "a/gcc/config/spu/spu.cc" || $3 == "a/gcc/common/config/spu/spu-common.cc")}
        on && ($1 == "+#define" || $1 == "-#define") && $2 == name {n += ($1 == "+#define") ? 1 : -1}
        END {print n+0}' "$series"/[0-9]*.patch) || { bad "reading $definition definitions failed"; exit 1; }
    [[ "$net" == 1 ]] && ok "$definition is net-defined" || bad "$definition net define count $net, want 1"
done

if [[ -n "${SPU16_GCC:-}" ]]; then
    cc=$SPU16_GCC
    version=$("$cc" -dumpversion) || { bad "SPU16_GCC version query failed"; exit 1; }
    [[ "$version" == 16.* ]] || { bad "SPU16_GCC is not GCC 16: $version"; exit 1; }
    work=$(mktemp -d "${TMPDIR:-/tmp}/spu-opt-defaults.XXXXXX") || exit 1
    trap 'rm -rf "$work"' EXIT
    loop='(char *d, int n) { for (int i = 0; i < n; i++) d[i] = 0; }'
    printf 'void f%s\n' "$loop" > "$work/plain.c"
    printf '__attribute__((optimize("O2"))) void f%s\n' "$loop" > "$work/attr-O2.c"
    printf '__attribute__((optimize("Os"))) void f%s\n' "$loop" > "$work/attr-Os.c"
    printf '#pragma GCC optimize ("O2")\nvoid f%s\n' "$loop" > "$work/pragma-O2.c"
    printf '__attribute__((optimize("tree-loop-distribute-patterns"))) void f%s\n' "$loop" > "$work/attr-ldp.c"
    printf '__attribute__((optimize("no-late-combine-instructions"))) void f%s\n' "$loop" > "$work/attr-nolc.c"
    memset() { # <want yes|no> <file> <options...>
        local want=$1 f=$2; shift 2
        if ! "$cc" "$@" -S -o "$work/out.s" "$work/$f" 2>"$work/err"; then bad "$f $*: compile failed: $(head -1 "$work/err")"; return; fi
        local got=no rc=0
        grep -q 'memset' "$work/out.s" || rc=$?
        case $rc in 0) got=yes;; 1) ;; *) bad "$f $*: assembly search failed ($rc)"; return;; esac
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
        local dumpdir
        dumpdir=$(mktemp -d "$work/d.XXXXXX") || { bad "cannot create dump directory"; return; }
        if ! "$cc" "$@" -fdump-rtl-late_combine1 -dumpdir "$dumpdir/" -c -o "$work/out.o" "$work/$f" 2>"$work/err"; then
            bad "$f $*: compile failed: $(head -1 "$work/err")"; return; fi
        # The dump of a pass that is gated off is empty or holds no function.
        local got=no dump rc
        for dump in "$dumpdir/"*late_combine1*; do
            [[ -e "$dump" ]] || continue
            rc=0
            grep -q '^;; Function' "$dump" || rc=$?
            case $rc in 0) got=yes;; 1) ;; *) bad "$f $*: dump search failed ($rc)"; return;; esac
        done
        [[ "$got" == "$want" ]] && ok "$f $*: late-combine $got" || bad "$f $*: late-combine $got, want $want"
    }
    latecombine no  plain.c     -O1
    latecombine yes plain.c     -O2
    latecombine yes plain.c     -Os
    latecombine yes plain.c     -O3
    latecombine no  plain.c     -O2 -fno-late-combine-instructions
    latecombine yes attr-O2.c   -O0
    latecombine yes pragma-O2.c -O1
    latecombine no  attr-nolc.c -O2
fi

if [[ $fail -ne 0 ]]; then printf 'FAIL: spu-gcc16-opt-defaults-test\n'; exit 1; fi
printf 'PASS: spu-gcc16-opt-defaults-test%s\n' "${SPU16_GCC:+ (compiler mode)}"
