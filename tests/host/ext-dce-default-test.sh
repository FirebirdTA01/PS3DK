#!/usr/bin/env bash
# gcc-16.x 0072: ext-dce is off by default for powerpc64-ps3-elf under ILP32 and untouched under LP64.
#
#  1. effective default (-Q --help=optimizers) at -O2, -O3 and -Os: ILP32 [disabled], LP64 [enabled];
#  2. an explicit -fext-dce is honoured under ILP32 ([enabled]) and -fno-ext-dce under LP64 ([disabled]);
#  3. code: the ilp32-ext-dce row's walk.c keeps the zero-extension of its buffer pointer (an rldicl ...,0,32 that
#     ext-dce would drop) under ILP32 at -O2 by default; also when the function carries
#     __attribute__((optimize("O2"))), inside #pragma GCC push_options / optimize / pop_options, and through -flto
#     (compile and link-time optimisation); with an explicit -fext-dce it is dropped (the check can see the
#     difference).
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
    if ! "$gcc" -Q --help=optimizers 2>/dev/null | grep -q -- '-fext-dce'; then
        echo "SKIP: ext-dce-default-test ($gcc has no ext-dce pass)"; exit 0
    fi
fi
[ -x "$gcc" ] || { echo "ext-dce-default: no compiler at '$gcc'"; exit 2; }
walk="$root/tests/regression/ilp32-ext-dce/walk.c"
[ -f "$walk" ] || { echo "ext-dce-default: missing $walk"; exit 2; }

status=0
ok()   { echo "ext-dce-default: ok   $*"; }
fail() { echo "ext-dce-default: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
export PS3DK_NO_DEFAULT_INCLUDE=1

state() { # <flags...>: [enabled] or [disabled]
    "$gcc" "$@" -Q --help=optimizers 2>/dev/null | awk '$1 == "-fext-dce" {print $2}'
}
expect_state() { # <label> <want> <flags...>
    local label=$1 want=$2; shift 2
    local got; got=$(state "$@")
    [ "$got" = "$want" ] && ok "$label: $got" || fail "$label: got '$got', want $want"
}

# 1 + 2
for o in -O2 -O3 -Os; do
    expect_state "ILP32 $o default" "[disabled]" $o
    expect_state "LP64 $o default" "[enabled]" -mlp64 $o
done
expect_state "ILP32 -O2 explicit -fext-dce" "[enabled]" -O2 -fext-dce
expect_state "ILP32 -O2 explicit -fno-ext-dce" "[disabled]" -O2 -fno-ext-dce
expect_state "LP64 -O2 explicit -fno-ext-dce" "[disabled]" -mlp64 -O2 -fno-ext-dce

# 3: the zero-extension ext-dce drops is kept.  Count clrldi rX,rY,32 (rldicl ...,0,32) in walk_sum's machine code.
od="${gcc%gcc}objdump"
zext_count() { "$od" -d "$1" | awk '/walk_sum>:$/ {f=1; next} f && /^$/ {f=0} f' | grep -cE 'clrldi[[:space:]]+r?[0-9]+,r?[0-9]+,32'; }
"$gcc" -O2 -c "$walk" -o "$work/default.o"
"$gcc" -O2 -fext-dce -c "$walk" -o "$work/forced.o"
d=$(zext_count "$work/default.o"); f=$(zext_count "$work/forced.o")
if [ "$d" -gt "$f" ]; then ok "ILP32 -O2: walk_sum keeps $d zero-extensions (explicit -fext-dce: $f)"; else fail "ILP32 -O2: $d zero-extensions by default, $f with -fext-dce (the default must keep more)"; fi

sed 's/^uint32_t walk_sum(/__attribute__((optimize("O2"))) uint32_t walk_sum(/' "$walk" > "$work/attr.c"
"$gcc" -O0 -c "$work/attr.c" -o "$work/attr.o"
a=$(zext_count "$work/attr.o"); [ "$a" -eq "$d" ] && ok "optimize(\"O2\") attribute keeps $a" || fail "optimize(\"O2\") attribute: $a zero-extensions, default -O2 has $d"

{ echo '#pragma GCC push_options'; echo '#pragma GCC optimize ("O2")'; cat "$walk"; echo '#pragma GCC pop_options'; } > "$work/pragma.c"
"$gcc" -O0 -c "$work/pragma.c" -o "$work/pragma.o"
a=$(zext_count "$work/pragma.o"); [ "$a" -eq "$d" ] && ok "push_options/optimize/pop_options keeps $a" || fail "pragma optimize: $a zero-extensions, default -O2 has $d"

# LTO: compile with -flto and link an executable; lto1 optimises walk_sum at link time, and -save-temps keeps its
# assembly (<out>.ltrans0.ltrans.s).  -fno-inline keeps walk_sum a function of its own.
printf 'extern unsigned walk_sum(unsigned, const unsigned char *, unsigned);
static unsigned char b[64];
int main(void) { return (int)walk_sum(1, b, 33); }
' > "$work/main.c"
if (cd "$work" && "$gcc" -O2 -flto -fno-inline -c "$walk" -o walk.o && "$gcc" -O0 -c main.c -o main.o &&
    "$gcc" -O2 -flto -fno-inline -save-temps walk.o main.o -o lto.elf) > "$work/lto.log" 2>&1; then
    a=$(cat "$work"/lto.elf.ltrans*.s 2>/dev/null | awk '/^walk_sum:/ {f=1} f' | grep -cE 'rldicl[[:space:]]+[0-9]+,[0-9]+,0,32')
    [ "$a" -ge "$d" ] && ok "LTO keeps $a" || fail "LTO: $a zero-extensions, default -O2 has $d"
else
    fail "LTO link failed"; head -5 "$work/lto.log" | sed 's/^/    /'
fi

[ $status -eq 0 ] && echo "PASS: ext-dce-default-test"
exit $status
